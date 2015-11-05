#include <mongoc.h>
#include <mongoc-cursor-private.h>
#include <assert.h>

#include "TestSuite.h"
#include "test-libmongoc.h"
#include "test-conveniences.h"
#include "mock_server/mock-server.h"
#include "mock_server/future.h"
#include "mock_server/future-functions.h"


typedef struct
{
   /* if do_live is true (the default), actually query the server using the
    * appropriate wire protocol: either OP_QUERY or a "find" command */
   bool                  do_live;
   bool                  requires_wire_version_4;
   const char           *docs;
   bson_t               *docs_bson;
   const char           *query_input;
   bson_t               *query_bson;
   const char           *fields;
   bson_t               *fields_bson;
   const char           *expected_find_command;
   const char           *expected_op_query;
   uint32_t              n_return;
   const char           *expected_result;
   bson_t               *expected_result_bson;
   uint32_t              skip;
   uint32_t              limit;
   uint32_t              batch_size;
   mongoc_query_flags_t  flags;
   mongoc_read_prefs_t  *read_prefs;
   const char           *filename;
   int                   lineno;
   const char           *funcname;
   uint32_t              n_results;
} test_collection_find_t;


#define TEST_COLLECTION_FIND_INIT { true, false }


static void
_insert_test_docs (mongoc_collection_t *collection,
                   const bson_t        *docs)
{
   bson_iter_t iter;
   uint32_t len;
   const uint8_t *data;
   bson_t doc;
   bool r;
   bson_error_t error;

   bson_iter_init (&iter, docs);
   while (bson_iter_next (&iter)) {
      bson_iter_document (&iter, &len, &data);
      bson_init_static (&doc, data, len);
      r = mongoc_collection_insert (collection, MONGOC_INSERT_NONE, &doc, NULL,
                                    &error);
      ASSERT_OR_PRINT (r, error);
   }
}


static void
_check_cursor (mongoc_cursor_t        *cursor,
               test_collection_find_t *test_data)
{
   const bson_t *doc;
   bson_t actual_result = BSON_INITIALIZER;
   char str[16];
   const char *key;
   uint32_t i = 0;
   bson_error_t error;

   while (mongoc_cursor_next (cursor, &doc)) {
      bson_uint32_to_string (i, &key, str, sizeof str);
      bson_append_document (&actual_result, key, -1, doc);
      i++;
   }

   ASSERT_OR_PRINT (!mongoc_cursor_error (cursor, &error), error);

   if (i != test_data->n_results) {
      fprintf (stderr, "expect %d results, got %d\n", test_data->n_results, i);
      abort ();
   }

   ASSERT (match_json (&actual_result, false /* is_command */,
                       test_data->filename, test_data->lineno,
                       test_data->funcname, test_data->expected_result));

   bson_destroy (&actual_result);
}


static void
_test_collection_find_live (test_collection_find_t *test_data)

{
   mongoc_client_t *client;
   mongoc_database_t *database;
   char *collection_name;
   mongoc_collection_t *collection;
   char *drop_cmd;
   bool r;
   bson_error_t error;
   mongoc_cursor_t *cursor;

   client = test_framework_client_new ();
   database = mongoc_client_get_database (client, "test");
   collection_name = gen_collection_name ("test");
   collection = mongoc_database_create_collection (
      database, collection_name,
      tmp_bson ("{'capped': true, 'size': 10000}"), &error);

   ASSERT_OR_PRINT (collection, error);

   _insert_test_docs (collection, test_data->docs_bson);

   cursor = mongoc_collection_find (collection,
                                    MONGOC_QUERY_NONE,
                                    test_data->skip,
                                    test_data->limit,
                                    test_data->batch_size,
                                    test_data->query_bson,
                                    test_data->fields_bson,
                                    test_data->read_prefs);

   _check_cursor (cursor, test_data);

   drop_cmd = bson_strdup_printf ("{'drop': '%s'}", collection_name);
   r = mongoc_client_command_simple (client, "test", tmp_bson (drop_cmd), NULL,
                                     NULL, &error);
   ASSERT_OR_PRINT (r, error);

   bson_free (drop_cmd);
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   bson_free (collection_name);
   mongoc_client_destroy (client);
}


typedef request_t *(*check_request_fn_t) (mock_server_t          *server,
                                          test_collection_find_t *test_data);


typedef void (*reply_fn_t) (request_t              *request,
                            test_collection_find_t *test_data);


/*--------------------------------------------------------------------------
 *
 * _test_collection_op_query_or_find_command --
 *
 *       Start a mock server with @max_wire_version, connect a client, and
 *       execute @test_data->query. Use the @check_request_fn callback to
 *       verify the client formatted the query correctly, and @reply_fn to
 *       response to the client. Check that the client cursor's results
 *       match @test_data->expected_result.
 *
 *--------------------------------------------------------------------------
 */

static void
_test_collection_op_query_or_find_command (
   test_collection_find_t *test_data,
   check_request_fn_t      check_request_fn,
   reply_fn_t              reply_fn,
   int32_t                 max_wire_version)
{
   mock_server_t *server;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   future_t *future;
   request_t *request;
   const bson_t *doc;
   bool cursor_next_result;
   bson_t actual_result = BSON_INITIALIZER;
   char str[16];
   const char *key;
   uint32_t i = 0;

   server = mock_server_with_autoismaster (max_wire_version);
   mock_server_run (server);
   client = mongoc_client_new_from_uri (mock_server_get_uri (server));
   collection = mongoc_client_get_collection (client, "db", "collection");
   cursor = mongoc_collection_find (collection,
                                    test_data->flags,
                                    test_data->skip,
                                    test_data->limit,
                                    test_data->batch_size,
                                    test_data->query_bson,
                                    test_data->fields_bson,
                                    test_data->read_prefs);

   future = future_cursor_next (cursor, &doc);
   request = check_request_fn (server, test_data);
   ASSERT (request);
   reply_fn (request, test_data);

   cursor_next_result = future_get_bool (future);
   /* did we expect at least one result? */
   ASSERT (cursor_next_result == (test_data->n_results > 0));
   assert (!mongoc_cursor_error (cursor, NULL));

   if (cursor_next_result) {
      bson_append_document (&actual_result, "0", -1, doc);
      i++;

      /* check remaining results */
      while (mongoc_cursor_next (cursor, &doc)) {
         bson_uint32_to_string (i, &key, str, sizeof str);
         bson_append_document (&actual_result, key, -1, doc);
         i++;
      }

      assert (!mongoc_cursor_error (cursor, NULL));
   }

   if (i != test_data->n_results) {
      fprintf (stderr,
               "Expected %d results, got %d\n", test_data->n_results, i);
      abort ();
   }

   ASSERT (match_json (&actual_result, false /* is_command */,
                       test_data->filename, test_data->lineno,
                       test_data->funcname, test_data->expected_result));

   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
   mock_server_destroy (server);
}


static request_t *
_check_op_query (mock_server_t *server,
                 test_collection_find_t *test_data)
{
   mongoc_query_flags_t flags;

   flags = test_data->flags | MONGOC_QUERY_SLAVE_OK;

   return mock_server_receives_query (server,
                                      "db.collection",
                                      flags,
                                      test_data->skip,
                                      test_data->n_return,
                                      test_data->expected_op_query,
                                      test_data->fields);
}


static void
_reply_to_op_query (request_t              *request,
                    test_collection_find_t *test_data)
{
   bson_t *docs;
   int i;
   bson_iter_t iter;
   uint32_t len;
   const uint8_t *data;

   docs = bson_malloc (test_data->n_results * sizeof (bson_t));
   bson_iter_init (&iter, test_data->expected_result_bson);

   for (i = 0; i < test_data->n_results; i++) {
      bson_iter_next (&iter);
      bson_iter_document (&iter, &len, &data);
      bson_init_static (&docs[i], data, len);
   }

   mock_server_reply_multi (request, MONGOC_REPLY_NONE, docs,
                            test_data->n_results, 0 /* cursor_id */);
}


static void
_test_collection_op_query (test_collection_find_t *test_data)
{
   _test_collection_op_query_or_find_command (test_data,
                                              _check_op_query,
                                              _reply_to_op_query,
                                              3 /* wire version */);
}


static request_t *
_check_find_command (mock_server_t *server,
                     test_collection_find_t *test_data)
{

   /* Server Selection Spec: all queries to standalone set slaveOk.
    *
    * Find, getMore And killCursors Commands Spec: "When sending a find command
    * rather than a legacy OP_QUERY find only the slaveOk flag is honored".
    */
   return mock_server_receives_command (server, "db", MONGOC_QUERY_SLAVE_OK,
                                        test_data->expected_find_command);
}


static void
_reply_to_find_command (request_t *request,
                        test_collection_find_t *test_data)
{
   const char *result_json;
   char *reply_json;

   result_json = test_data->expected_result ?
                 test_data->expected_result :
                 "[]";

   reply_json = bson_strdup_printf ("{'ok': 1,"
                                    " 'cursor': {"
                                    "    'id': 0,"
                                    "    'ns': 'db.collection',"
                                    "    'firstBatch': %s}}",
                                    result_json);

   mock_server_replies_simple (request, reply_json);

   bson_free (reply_json);
}


static void
_test_collection_find_command (test_collection_find_t *test_data)

{
   _test_collection_op_query_or_find_command (test_data,
                                              _check_find_command,
                                              _reply_to_find_command,
                                              4 /* max wire version */);
}


static void
_test_collection_find (test_collection_find_t *test_data)
{
   /* catch typos in tests' setup */
   if (test_data->query_input) {
      BSON_ASSERT (test_data->requires_wire_version_4 ||
                   test_data->expected_op_query);
   }

   BSON_ASSERT (test_data->expected_find_command);

   test_data->docs_bson = tmp_bson (test_data->docs);
   test_data->query_bson = tmp_bson (test_data->query_input);
   test_data->fields_bson = test_data->fields ?
                            tmp_bson (test_data->fields) :
                            NULL;
   test_data->expected_result_bson = tmp_bson (test_data->expected_result);
   test_data->n_results = bson_count_keys (test_data->expected_result_bson);

   if (test_data->do_live &&
         (!test_data->requires_wire_version_4 ||
          test_framework_max_wire_version_at_least (4)))
   {
      _test_collection_find_live (test_data);
   }

   if (!test_data->requires_wire_version_4) {
      _test_collection_op_query (test_data);
   }

   _test_collection_find_command (test_data);
}


static void
test_dollar_query (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}, {'_id': 2}]";
   test_data.query_input = "{'$query': {'_id': 1}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {'_id': 1}}";
   test_data.expected_result = "[{'_id': 1}]";
   _test_collection_find (&test_data);
}


/* test that we can query for a document by a key named "filter" */
static void
test_key_named_filter (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1, 'filter': 1}, {'_id': 2, 'filter': 2}]";
   test_data.query_input = "{'$query': {'filter': 2}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {'filter': 2}}";
   test_data.expected_result = "[{'_id': 2, 'filter': 2}]";
   _test_collection_find (&test_data);
}

/* test '$query': {'filter': {'i': 2}} */
static void
test_op_query_subdoc_named_filter (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1, 'filter': {'i': 1}}, {'_id': 2, 'filter': {'i': 2}}]";
   test_data.query_input = "{'$query': {'filter': {'i': 2}}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {'filter': {'i': 2}}}";
   test_data.expected_result = "[{'_id': 2, 'filter': {'i': 2}}]";
   _test_collection_find (&test_data);
}


/* test new-style 'filter': {'filter': {'i': 2}} */
static void
test_find_cmd_subdoc_named_filter (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1, 'filter': {'i': 1}}, {'_id': 2, 'filter': {'i': 2}}]";
   test_data.query_input = "{'filter': {'filter': {'i': 2}}}";
   test_data.expected_find_command = "{'find': 'collection', 'filter': {'filter': {'i': 2}}}";
   test_data.expected_result = "[{'_id': 2, 'filter': {'i': 2}}]";

   /* this only works if you know you're talking wire version 4 */
   test_data.requires_wire_version_4 = true;

   _test_collection_find (&test_data);
}


/* test new-style 'filter': {'filter': {'i': 2}}, 'singleBatch': true
 * we just use singleBatch to prove that a new-style option can be passed
 * alongside 'filter'
 */
static void
test_find_cmd_subdoc_named_filter_with_option (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1, 'filter': {'i': 1}}, {'_id': 2, 'filter': {'i': 2}}]";
   test_data.query_input = "{'filter': {'filter': {'i': 2}}, 'singleBatch': true}";
   test_data.expected_find_command = "{'find': 'collection', 'filter': {'filter': {'i': 2}}, 'singleBatch': true}";
   test_data.expected_result = "[{'_id': 2, 'filter': {'i': 2}}]";

   /* this only works if you know you're talking wire version 4 */
   test_data.requires_wire_version_4 = true;

   _test_collection_find (&test_data);
}


/* test future-compatibility with a new server's find command options */
static void
test_newoption (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.query_input = "{'filter': {'_id': 1}, 'newOption': true}";
   test_data.expected_find_command = "{'find': 'collection', 'filter': {'_id': 1}, 'newOption': true}";

   /* won't work today */
   test_data.do_live = false;
   test_data.requires_wire_version_4 = true;

   _test_collection_find (&test_data);
}


static void
test_orderby (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}, {'_id': 2}]";
   test_data.query_input = "{'$query': {}, '$orderby': {'_id': -1}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'sort': {'_id': -1}}";
   test_data.expected_result = "[{'_id': 2}, {'_id': 1}]";
   _test_collection_find (&test_data);
}


static void
test_fields (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1, 'a': 1, 'b': 2}]";
   test_data.fields = "{'_id': 0, 'b': 1}";
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'projection': {'_id': 0, 'b': 1}}";
   test_data.expected_result = "[{'b': 2}]";
   _test_collection_find (&test_data);
}


static void
test_int_modifiers (void)
{
   const char *modifiers[] = {
      "maxScan",
      "maxTimeMS",
   };

   const char *mod;
   size_t i;
   char *query;
   char *find_command;
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;

   test_data.expected_result = test_data.docs = "[{'_id': 1}]";

   for (i = 0; i < sizeof (modifiers) / sizeof (const char *); i++) {
      mod = modifiers[i];
      query = bson_strdup_printf ("{'$query': {}, '$%s': 9999}", mod);

      /* find command has same modifier, without the $-prefix */
      find_command = bson_strdup_printf (
         "{'find': 'collection', 'filter': {}, '%s': 9999}", mod);

      test_data.expected_op_query = test_data.query_input = query;
      test_data.expected_find_command = find_command;
      _test_collection_find (&test_data);
      bson_free (query);
      bson_free (find_command);
   }
}


static void
test_bool_modifiers (void)
{
   const char *modifiers[] = {
      "snapshot",
      "showRecordId",
   };

   const char *mod;
   size_t i;
   char *query;
   char *find_command;
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;

   test_data.expected_result = test_data.docs = "[{'_id': 1}]";

   for (i = 0; i < sizeof (modifiers) / sizeof (const char *); i++) {
      mod = modifiers[i];
      query = bson_strdup_printf ("{'$query': {}, '$%s': true}", mod);

      /* find command has same modifier, without the $-prefix */
      find_command = bson_strdup_printf (
         "{'find': 'collection', 'filter': {}, '%s': true}", mod);

      test_data.expected_op_query = test_data.query_input = query;
      test_data.expected_find_command = find_command;
      _test_collection_find (&test_data);

      bson_free (query);
      bson_free (find_command);
   }
}


static void
test_index_spec_modifiers (void)
{
   /* don't include $max, it needs a slightly different argument to succeed */
   const char *modifiers[] = {
      "hint",
      "min",
   };

   const char *mod;
   size_t i;
   char *query;
   char *find_command;
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;

   test_data.expected_result = test_data.docs = "[{'_id': 1}]";

   for (i = 0; i < sizeof (modifiers) / sizeof (const char *); i++) {
      mod = modifiers[i];
      query = bson_strdup_printf ("{'$query': {}, '$%s': {'_id': 1}}", mod);

      /* find command has same modifier, without the $-prefix */
      find_command = bson_strdup_printf (
         "{'find': 'collection', 'filter': {}, '%s': {'_id': 1}}", mod);

      test_data.expected_op_query = test_data.query_input = query;
      test_data.expected_find_command = find_command;
      _test_collection_find (&test_data);

      bson_free (query);
      bson_free (find_command);
   }
}


static void
test_comment (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}]";
   test_data.query_input = "{'$query': {}, '$comment': 'hi'}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'comment': 'hi'}";
   test_data.expected_result = "[{'_id': 1}]";
   _test_collection_find (&test_data);
}


static void
test_max (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}]";
   test_data.query_input = "{'$query': {}, '$max': {'_id': 100}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'max': {'_id': 100}}";
   test_data.expected_result = "[{'_id': 1}]";
   _test_collection_find (&test_data);
}


/* $showDiskLoc becomes showRecordId */
static void
test_diskloc (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}]";
   test_data.query_input = "{'$query': {}, '$showDiskLoc': true}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'showRecordId': true}";
   test_data.expected_result = "[{'_id': 1}]";
   _test_collection_find (&test_data);
}


static void
test_returnkey (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}]";
   test_data.query_input = "{'$query': {}, '$returnKey': true}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'returnKey': true}";
   test_data.expected_result = "[{}]";
   _test_collection_find (&test_data);
}


static void
test_skip (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}, {'_id': 2}]";
   test_data.skip = 1;
   test_data.query_input = "{'$query': {}, '$orderby': {'_id': 1}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'sort': {'_id': 1}, 'skip': {'$numberLong': '1'}}";
   test_data.expected_result = "[{'_id': 2}]";
   _test_collection_find (&test_data);
}


static void
test_batch_size (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}]";
   test_data.batch_size = 2;
   test_data.n_return = 2;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'batchSize': {'$numberLong': '2'}}";
   test_data.expected_result = "[{'_id': 1}]";
   _test_collection_find (&test_data);
}


static void
test_limit (void)
{
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;
   test_data.docs = "[{'_id': 1}, {'_id': 2}, {'_id': 3}]";
   test_data.limit = 2;
   test_data.query_input = "{'$query': {}, '$orderby': {'_id': 1}}";
   test_data.expected_op_query = test_data.query_input;
   test_data.n_return = 2;
   test_data.expected_find_command = "{'find': 'collection', 'filter': {}, 'sort': {'_id': 1}, 'limit': {'$numberLong': '2'}}";
   test_data.expected_result = "[{'_id': 1}, {'_id': 2}]";
   _test_collection_find (&test_data);
}


static void
test_query_flags (void)
{
   int i;
   char *find_cmd;
   test_collection_find_t test_data = TEST_COLLECTION_FIND_INIT;

   typedef struct
   {
      mongoc_query_flags_t flag;
      const char          *name;
   } flag_and_name_t;

   /* slaveok is still in the wire protocol header, exhaust is not supported */
   flag_and_name_t flags_and_names[] = {
      { MONGOC_QUERY_TAILABLE_CURSOR,   "tailable"            },
      { MONGOC_QUERY_OPLOG_REPLAY,      "oplogReplay"         },
      { MONGOC_QUERY_NO_CURSOR_TIMEOUT, "noCursorTimeout"     },
      { MONGOC_QUERY_AWAIT_DATA,        "awaitData"           },
      { MONGOC_QUERY_PARTIAL,           "allowPartialResults" },
   };

   test_data.expected_result = test_data.docs = "[{'_id': 1}]";

   for (i = 0; i < (sizeof flags_and_names) / (sizeof (flag_and_name_t)); i++) {
      find_cmd = bson_strdup_printf (
         "{'find': 'collection', 'filter': {}, '%s': true}",
         flags_and_names[i].name);

      test_data.flags = flags_and_names[i].flag;
      test_data.expected_find_command = find_cmd;

      _test_collection_find (&test_data);

      bson_free (find_cmd);
   }
}


void
test_collection_find_install (TestSuite *suite)
{
   TestSuite_Add (suite, "/Collection/find/dollar_query",
                  test_dollar_query);
   TestSuite_Add (suite, "/Collection/find/key_named_filter",
                  test_key_named_filter);
   TestSuite_Add (suite, "/Collection/find/cmd/subdoc_named_filter",
                  test_find_cmd_subdoc_named_filter);
   TestSuite_Add (suite, "/Collection/find/query/subdoc_named_filter",
                  test_op_query_subdoc_named_filter);
   TestSuite_Add (suite, "/Collection/find/newoption",
                  test_newoption);
   TestSuite_Add (suite, "/Collection/find/cmd/subdoc_named_filter_with_option",
                  test_find_cmd_subdoc_named_filter_with_option);
   TestSuite_Add (suite, "/Collection/find/orderby",
                  test_orderby);
   TestSuite_Add (suite, "/Collection/find/fields",
                  test_fields);
   TestSuite_Add (suite, "/Collection/find/modifiers/integer",
                  test_int_modifiers);
   TestSuite_Add (suite, "/Collection/find/modifiers/bool",
                  test_bool_modifiers);
   TestSuite_Add (suite, "/Collection/find/modifiers/index_spec",
                  test_index_spec_modifiers);
   TestSuite_Add (suite, "/Collection/find/comment",
                  test_comment);
   TestSuite_Add (suite, "/Collection/find/max",
                  test_max);
   TestSuite_Add (suite, "/Collection/find/showdiskloc",
                  test_diskloc);
   TestSuite_Add (suite, "/Collection/find/returnkey",
                  test_returnkey);
   TestSuite_Add (suite, "/Collection/find/skip",
                  test_skip);
   TestSuite_Add (suite, "/Collection/find/batch_size",
                  test_batch_size);
   TestSuite_Add (suite, "/Collection/find/limit",
                  test_limit);
   TestSuite_Add (suite, "/Collection/find/flags",
                  test_query_flags);
}
