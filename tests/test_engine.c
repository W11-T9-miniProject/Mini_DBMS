#include "engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char path[256];
static long allocation_budget = -1;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size) {
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) allocation_budget--;
    return __real_calloc(n, size);
}
static sql_statement statement(const char *text) {
    sql_parser parser; sql_statement s; sql_error error;
    sql_parser_init(&parser, text, strlen(text));
    assert(sql_next(&parser, &s, &error) == SQL_OK);
    return s;
}
static db_result execute(db_engine *engine, const char *sql) {
    sql_statement s = statement(sql); db_result result;
    assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_OK);
    return result;
}
static void insert(db_engine *engine) {
    db_result result = execute(engine, "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('PLAY1',71,305,'rock');");
    assert(result.affected == 1 && result.inserted.id == 1);
    result = execute(engine, "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('PLAY2',32,108,'pop');");
    assert(result.inserted.id == 2);
}
static int count_row(const play_record *record, void *context) {
    assert(record->id >= 1 && record->event_code[0]);
    (*(unsigned *)context)++;
    return 0;
}
static int stop_row(const play_record *record, void *context) {
    (void)record; (void)context; return 1;
}
static void test_insert_select_and_reopen(void) {
    unlink(path); db_engine *engine = NULL;
    assert(engine_open(&engine, path) == DB_OK);
    db_result result = execute(engine, "SELECT * FROM play_history;");
    assert(result.affected == 0 && result.scanned == 0);
    insert(engine);
    sql_statement s = statement("SELECT * FROM play_history;");
    unsigned rows = 0;
    assert(engine_execute(engine, &s, count_row, &rows, &result) == DB_OK);
    assert(rows == 2 && result.affected == 2 && result.scanned == 2);
    assert(engine_close(engine) == DB_OK);
    assert(engine_open(&engine, path) == DB_OK);
    result = execute(engine, "SELECT * FROM play_history;");
    assert(result.affected == 2 && result.scanned == 2);
    result = execute(engine, "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('PLAY3',71,305,'jazz');");
    assert(result.inserted.id == 3);
    assert(engine_close(engine) == DB_OK);
}
static void test_filters(void) {
    unlink(path); db_engine *engine = NULL;
    assert(engine_open(&engine, path) == DB_OK); insert(engine);
    const char *queries[] = {"SELECT * FROM play_history WHERE id=2;",
        "SELECT * FROM play_history WHERE event_code='PLAY1';", "SELECT * FROM play_history WHERE user_id=32;",
        "SELECT * FROM play_history WHERE song_id=305;", "SELECT * FROM play_history WHERE genre='pop';"};
    for (size_t i = 0; i < sizeof(queries)/sizeof(*queries); ++i) {
        db_result result = execute(engine, queries[i]);
        assert(result.affected == 1 && result.scanned == (i == 0 ? 1 : 2));
    }
    db_result result = execute(engine, "SELECT * FROM play_history WHERE id=999;");
    assert(result.affected == 0 && result.scanned == 0);
    result = execute(engine, "SELECT * FROM play_history WHERE event_code='not-found';");
    assert(result.affected == 0);
    assert(engine_close(engine) == DB_OK);
}
static void test_invalid_records_and_callback_failure(void) {
    unlink(path); db_engine *engine = NULL;
    assert(engine_open(&engine, path) == DB_OK); insert(engine);
    sql_statement s = statement("INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('BAD',0,1,'rock');");
    db_result result;
    assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_INVALID_ARGUMENT);
    s = statement("INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('BAD',1,1,'');");
    assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_INVALID_ARGUMENT);
    s = statement("SELECT * FROM play_history;");
    assert(engine_execute(engine, &s, stop_row, NULL, &result) == DB_IO_ERROR);
    result = execute(engine, "SELECT * FROM play_history;");
    assert(result.affected == 2);
    s.has_where = 1; s.field = SQL_ID; s.value.kind = SQL_STRING;
    assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_INVALID_ARGUMENT);
    assert(engine_close(engine) == DB_OK);
}
static int verify_row(const play_record *record, void *context) {
    uint64_t expected = *(uint64_t *)context;
    assert(record->id == expected && record->song_id == expected + 100);
    return 0;
}
static void test_index_rebuild_and_failures(void) {
    unlink(path); db_engine *engine = NULL; db_result result;
    assert(engine_open(&engine, path) == DB_OK);
    sql_statement s = statement("INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P',71,101,'rock');");
    allocation_budget = 0;
    assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_NO_MEMORY);
    allocation_budget = -1;
    assert(execute(engine, "SELECT * FROM play_history;").affected == 0);
    for (uint64_t i = 1; i <= 5000; ++i) {
        s.record.song_id = i + 100;
        assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_OK && result.inserted.id == i);
    }
    assert(engine_close(engine) == DB_OK);
    for (long budget = 0; budget < 12; ++budget) {
        allocation_budget = budget;
        db_status status = engine_open(&engine, path);
        allocation_budget = -1;
        if (status == DB_OK) assert(engine_close(engine) == DB_OK);
        else assert(status == DB_NO_MEMORY && engine == NULL);
    }
    assert(engine_open(&engine, path) == DB_OK);
    s = statement("SELECT * FROM play_history WHERE id=1;");
    for (uint64_t id = 1; id <= 5000; id += 17) {
        s.value.number = id;
        assert(engine_execute(engine, &s, verify_row, &id, &result) == DB_OK);
        assert(result.affected == 1 && result.scanned == 1 && result.used_index && result.index_nodes >= 3);
    }
    s.value.number = UINT64_MAX;
    assert(engine_execute(engine, &s, NULL, NULL, &result) == DB_OK && result.scanned == 0 && result.affected == 0);
    assert(engine_close(engine) == DB_OK);
}
int main(void) {
    char directory[] = "/tmp/mini-dbms-engine-XXXXXX";
    assert(mkdtemp(directory)); snprintf(path, sizeof(path), "%s/play_history.db", directory);
    test_insert_select_and_reopen(); puts("PASS engine INSERT/SELECT, callback rows and reopen");
    test_filters(); puts("PASS linear scan and all five WHERE columns");
    test_invalid_records_and_callback_failure(); puts("PASS semantic errors, callback failure and retained data");
    test_index_rebuild_and_failures(); puts("PASS indexed lookup, rebuild and allocation failure before append");
    unlink(path); assert(rmdir(directory) == 0);
    return 0;
}
