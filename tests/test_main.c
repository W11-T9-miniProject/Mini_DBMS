#include "db.h"
#include "db_script.h"
#include "bptree.h"
#include "internal.h"
#include "platform.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define process_id _getpid
#define remove_directory _rmdir
#else
#include <unistd.h>
#define process_id getpid
#define remove_directory rmdir
#endif

static unsigned assertions;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static char test_root[256];

static void make_test_path(char *path, size_t capacity, const char *name) {
    snprintf(path, capacity, "%s/%s", test_root, name);
}

static void remove_test_database(const char *directory) {
    char file[512];
    snprintf(file, sizeof(file), "%s/users.db", directory);
    CHECK(remove(file) == 0);
    snprintf(file, sizeof(file), "%s/users.lock", directory);
    CHECK(remove(file) == 0);
    CHECK(remove_directory(directory) == 0);
}

typedef struct { uint64_t count, id_sum, last_id; DbRow last_row; } Collected;

static bool collect_row(const DbRow *row, const DbResult *result, void *context) {
    Collected *collected = context;
    (void)result;
    ++collected->count;
    collected->id_sum += row->id;
    collected->last_id = row->id;
    collected->last_row = *row;
    return true;
}

static bool reject_row(const DbRow *row, const DbResult *result, void *context) {
    (void)row; (void)result; (void)context;
    return false;
}

static DbResult run_sql(Db *db, const char *sql, Collected *collected) {
    DbResult result;
    DbError error;
    DbStatus status = db_execute(db, sql, collected ? collect_row : NULL, collected, &result, &error);
    if (status != DB_OK) fprintf(stderr, "%s: %s\n", sql, error.message);
    CHECK(status == DB_OK);
    return result;
}

static void test_parser(void) {
    const char *valid[] = {
        "insert into users(name,age) values('a',1)",
        "INSERT INTO USERS VALUES ('a',-2147483648,'');",
        "INSERT INTO users(age,name,email) VALUES(2147483647,'O''Brien','x,y;z'),(0,'','')",
        "/* comment */ SELECT name,id FROM users WHERE id BETWEEN -1 AND 9223372036854775807 LIMIT 0;--end",
        "SELECT * FROM users WHERE age != -10",
        "SELECT * FROM users WHERE name <> 'x'",
        "EXPLAIN SELECT * FROM users WHERE id > 3",
        "SELECT * FROM users WHERE email <= 'z'"
    };
    const char *invalid[] = {
        "", "-- comment only", "CREATE TABLE users", "SELECT * FROM missing",
        "SELECT nope FROM users", "SELECT id,id FROM users", "SELECT * FROM users WHERE id='1'",
        "SELECT * FROM users WHERE name=1", "SELECT * FROM users LIMIT -1",
        "SELECT * FROM users WHERE id=9223372036854775808", "SELECT * FROM users WHERE age=-9223372036854775809",
        "SELECT * FROM users WHERE id = 1 AND age=2", "SELECT * FROM users; SELECT * FROM users",
        "SELECT * FROM users WHERE id=1.0", "SELECT * FROM users WHERE name BETWEEN 'a' AND 'z'",
        "INSERT INTO users(id,name,age) VALUES(1,'a',1)", "INSERT INTO users(name) VALUES('a')",
        "INSERT INTO users(name,age) VALUES('a','1')", "INSERT INTO users(name,age) VALUES('a',2147483648)",
        "INSERT INTO users(name,age) VALUES('a',-2147483649)", "INSERT INTO users(name,age) VALUES('a',1,2)",
        "INSERT INTO users(name,name,age) VALUES('a','b',1)", "INSERT INTO users(name,age) VALUES('a',1),",
        "INSERT INTO users(name,age) VALUES('unterminated,1)", "SELECT * FROM users /* unclosed",
        "EXPLAIN INSERT INTO users(name,age) VALUES('a',1)", "SELECT * FROM users WHERE id ! 1"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        Statement statement;
        DbError error;
        if (sql_parse(valid[i], &statement, &error) != DB_OK) {
            fprintf(stderr, "Valid SQL rejected: %s (%s)\n", valid[i], error.message);
            CHECK(false);
        }
        sql_statement_destroy(&statement);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        Statement statement;
        DbError error;
        CHECK(sql_parse(invalid[i], &statement, &error) == DB_SQL_ERROR);
        CHECK(error.message[0] != 0);
    }
    char sql[512], name[65], email[129];
    memset(name, 'n', 63); name[63] = 0;
    memset(email, 'e', 127); email[127] = 0;
    snprintf(sql, sizeof(sql), "INSERT INTO users VALUES('%s',0,'%s')", name, email);
    Statement statement;
    DbError error;
    CHECK(sql_parse(sql, &statement, &error) == DB_OK);
    CHECK(strlen(statement.rows[0].name) == 63 && strlen(statement.rows[0].email) == 127);
    sql_statement_destroy(&statement);
    name[63] = 'n'; name[64] = 0;
    snprintf(sql, sizeof(sql), "INSERT INTO users VALUES('%s',0,'')", name);
    CHECK(sql_parse(sql, &statement, &error) == DB_SQL_ERROR);
    email[127] = 'e'; email[128] = 0;
    snprintf(sql, sizeof(sql), "INSERT INTO users VALUES('',0,'%s')", email);
    CHECK(sql_parse(sql, &statement, &error) == DB_SQL_ERROR);
    CHECK(sql_parse(NULL, &statement, &error) == DB_SQL_ERROR);
    puts("PASS parser: grammar, type checks, limits, malformed SQL");
}

static uint32_t random_next(uint32_t *seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

static void test_bptree_order(unsigned order) {
    const size_t count = 50000;
    size_t *keys = malloc(count * sizeof(*keys));
    CHECK(keys != NULL);
    for (size_t i = 0; i < count; ++i) keys[i] = order == 1 ? count - 1 - i : i;
    if (order == 2) {
        uint32_t seed = 123456789;
        for (size_t i = count - 1; i; --i) {
            size_t j = random_next(&seed) % (i + 1);
            size_t temporary = keys[i]; keys[i] = keys[j]; keys[j] = temporary;
        }
    }
    Bptree tree;
    CHECK(bptree_init(&tree));
    CHECK(bptree_validate(&tree));
    uint64_t value;
    CHECK(!bptree_find(&tree, 0, &value));
    for (size_t i = 0; i < count; ++i) {
        CHECK(bptree_insert(&tree, keys[i] * 2, keys[i] * 17));
        if (i % 4096 == 0) CHECK(bptree_validate(&tree));
    }
    CHECK(bptree_validate(&tree));
    CHECK(tree.size == count && tree.height >= 3);
    CHECK(!bptree_insert(&tree, 0, 999));
    CHECK(bptree_validate(&tree));
    for (size_t i = 0; i < count; ++i) {
        CHECK(bptree_find(&tree, i * 2, &value));
        CHECK(value == i * 17);
        CHECK(!bptree_find(&tree, i * 2 + 1, &value));
    }
    BptCursor cursor = bptree_lower_bound(&tree, 19);
    uint64_t key;
    CHECK(bptree_cursor_next(&cursor, &key, &value) && key == 20 && value == 170);
    cursor = bptree_lower_bound(&tree, 0);
    for (size_t i = 0; i < count; ++i)
        CHECK(bptree_cursor_next(&cursor, &key, &value) && key == i * 2 && value == i * 17);
    CHECK(!bptree_cursor_next(&cursor, &key, &value));
    CHECK(bptree_insert(&tree, UINT64_MAX, 42));
    CHECK(bptree_find(&tree, UINT64_MAX, &value) && value == 42);
    CHECK(bptree_validate(&tree));
    bptree_destroy(&tree);
    free(keys);
}

static void test_engine(void) {
    char path[320]; make_test_path(path, sizeof(path), "engine");
    DbError error;
    Db *db = db_open(path, true, &error);
    CHECK(db != NULL);
    Db *second = db_open(path, true, &error);
    CHECK(second == NULL && error.code == DB_BUSY);
    CHECK(run_sql(db, "SELECT * FROM users", NULL).affected_rows == 0);
    DbResult result = run_sql(db, "INSERT INTO users(name,age,email) VALUES('Alice',25,'a@x'),('Bob',30,'b@x'),('O''Brien; Kim',-1,'')", NULL);
    CHECK(result.affected_rows == 3 && result.first_insert_id == 1);
    result = run_sql(db, "INSERT INTO users(name,age) VALUES('Default',0)", NULL);
    CHECK(result.first_insert_id == 4);
    Collected rows = {0};
    result = run_sql(db, "SELECT email,name FROM users WHERE id=4", &rows);
    CHECK(result.plan == DB_PLAN_INDEX_POINT && result.examined_rows == 1);
    CHECK(rows.count == 1 && rows.last_row.email[0] == 0 && strcmp(rows.last_row.name, "Default") == 0);
    CHECK(result.column_count == 2 && strcmp(result.columns[0], "email") == 0);
    rows = (Collected){0};
    result = run_sql(db, "SELECT * FROM users WHERE name='O''Brien; Kim'", &rows);
    CHECK(result.plan == DB_PLAN_SCAN && result.examined_rows == 4 && rows.last_id == 3);
    result = run_sql(db, "SELECT * FROM users WHERE id BETWEEN 2 AND 3", NULL);
    CHECK(result.plan == DB_PLAN_INDEX_RANGE && result.affected_rows == 2 && result.examined_rows == 2);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id=99", NULL).examined_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id=-1", NULL).affected_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id<1", NULL).affected_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id<=0", NULL).affected_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id>9223372036854775807", NULL).affected_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id>=-9223372036854775808", NULL).affected_rows == 4);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id BETWEEN -5 AND -1", NULL).affected_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id BETWEEN 3 AND 1", NULL).affected_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users LIMIT 0", NULL).examined_rows == 0);
    CHECK(run_sql(db, "SELECT * FROM users LIMIT 2", NULL).affected_rows == 2);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id>=1 LIMIT 1", NULL).examined_rows == 1);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id<>2", NULL).affected_rows == 3);
    result = run_sql(db, "EXPLAIN SELECT * FROM users WHERE id=1", NULL);
    CHECK(result.explain && result.examined_rows == 0 && result.plan == DB_PLAN_INDEX_POINT);
    CHECK(run_sql(db, "EXPLAIN SELECT * FROM users WHERE age=25", NULL).plan == DB_PLAN_SCAN);
    CHECK(db_execute(db, "INSERT INTO users(name,age) VALUES('ok',1),('bad','x')", NULL, NULL, &result, &error) == DB_SQL_ERROR);
    CHECK(run_sql(db, "SELECT * FROM users", NULL).affected_rows == 4);
    CHECK(db_execute(db, "SELECT * FROM users", reject_row, NULL, &result, &error) == DB_CALLBACK_ERROR);
    CHECK(run_sql(db, "SELECT * FROM users", NULL).affected_rows == 4);
    Collected indexed = {0}, scanned = {0};
    run_sql(db, "SELECT * FROM users WHERE id=2", &indexed);
    CHECK(db_execute_scan(db, "SELECT * FROM users WHERE id=2", collect_row, &scanned, &result, &error) == DB_OK);
    CHECK(scanned.count == indexed.count && scanned.id_sum == indexed.id_sum && result.examined_rows == 4);
    db_close(db);
    db = db_open(path, true, &error);
    CHECK(db != NULL);
    rows = (Collected){0};
    run_sql(db, "SELECT * FROM users", &rows);
    CHECK(rows.count == 4 && rows.id_sum == 10);
    result = run_sql(db, "INSERT INTO users VALUES('Restart',-2147483648,'')", NULL);
    CHECK(result.first_insert_id == 5);
    rows = (Collected){0};
    run_sql(db, "SELECT * FROM users WHERE id=5", &rows);
    CHECK(rows.last_row.age == INT32_MIN);
    db_close(db);
    db = db_open(path, true, &error);
    CHECK(db != NULL);
    rows = (Collected){0};
    run_sql(db, "SELECT * FROM users WHERE id=5", &rows);
    CHECK(rows.last_row.age == INT32_MIN);
    db_close(db);
    remove_test_database(path);
    puts("PASS engine: persistence, auto ID, index plans, limits, rollback of parse errors, reopen");
}

static void test_corruption(void) {
    char path[320], file_path[512];
    for (unsigned mode = 0; mode < 3; ++mode) {
        char name[32]; snprintf(name, sizeof(name), "corrupt%u", mode);
        make_test_path(path, sizeof(path), name);
        DbError error;
        Db *db = db_open(path, false, &error);
        CHECK(db != NULL);
        run_sql(db, "INSERT INTO users(name,age) VALUES('safe',1)", NULL);
        db_close(db);
        snprintf(file_path, sizeof(file_path), "%s/users.db", path);
        FILE *file = fopen(file_path, "r+b");
        CHECK(file != NULL);
        if (mode == 0) { CHECK(fseek(file, 30, SEEK_SET) == 0); CHECK(fputc('X', file) != EOF); }
        if (mode == 1) { CHECK(fseek(file, 0, SEEK_END) == 0); CHECK(fputc('X', file) != EOF); }
        if (mode == 2) { CHECK(fseek(file, 0, SEEK_SET) == 0); CHECK(fputc('X', file) != EOF); }
        CHECK(fclose(file) == 0);
        db = db_open(path, false, &error);
        CHECK(db == NULL && error.code == DB_CORRUPT);
        remove_test_database(path);
    }
    puts("PASS storage: checksum, partial record, invalid header rejected");
}

typedef struct { Db *db; size_t statements; uint64_t selected; } ScriptContext;

static DbStatus execute_test_script(const char *sql, void *context, DbError *error) {
    ScriptContext *script = context;
    ++script->statements;
    DbResult result;
    DbStatus status = db_execute(script->db, sql, NULL, NULL, &result, error);
    if (status == DB_OK && result.plan != DB_PLAN_INSERT) script->selected += result.affected_rows;
    return status;
}

static void test_script(void) {
    char path[320]; make_test_path(path, sizeof(path), "script");
    DbError error;
    Db *db = db_open(path, false, &error);
    CHECK(db != NULL);
    FILE *file = tmpfile(); CHECK(file != NULL);
    fputs("-- ;\n; /* ; */ INSERT INTO users(name,age) VALUES('a; b,''c',1);\n"
          "/* ; */ SELECT * FROM users; SELECT name FROM users -- ;\n WHERE id=1; -- last comment", file);
    rewind(file);
    ScriptContext script = {db, 0, 0};
    CHECK(db_run_script(file, execute_test_script, &script, &error) == DB_OK);
    CHECK(script.statements == 3 && script.selected == 2);
    fclose(file);
    file = tmpfile(); CHECK(file != NULL);
    fputs("SELECT * FROM users; BAD SQL; INSERT INTO users(name,age) VALUES('never',1);", file);
    rewind(file);
    script.statements = 0;
    CHECK(db_run_script(file, execute_test_script, &script, &error) == DB_SQL_ERROR);
    CHECK(script.statements == 2 && run_sql(db, "SELECT * FROM users", NULL).affected_rows == 1);
    fclose(file);
    const char *bad[] = {"/* unterminated", "SELECT * FROM users WHERE name='unterminated"};
    for (size_t i = 0; i < 2; ++i) {
        file = tmpfile(); CHECK(file != NULL);
        fputs(bad[i], file); rewind(file);
        CHECK(db_run_script(file, execute_test_script, &script, &error) == DB_SQL_ERROR);
        fclose(file);
    }
    db_close(db);
    remove_test_database(path);
    puts("PASS SQL files: quotes, comments, multiple statements, stop on error");
}

typedef struct { Db *db; unsigned number; bool reader; bool failed; } ThreadContext;

static void run_worker(ThreadContext *worker) {
    for (unsigned i = 0; i < 200; ++i) {
        char sql[160];
        if (worker->reader) snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id >= 1 LIMIT 20");
        else snprintf(sql, sizeof(sql), "INSERT INTO users(name,age) VALUES('thread_%u_%u',%u)", worker->number, i, i);
        DbError error;
        DbResult result;
        if (db_execute(worker->db, sql, NULL, NULL, &result, &error) != DB_OK) {
            worker->failed = true;
            return;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID context) { run_worker(context); return 0; }
#else
static void *worker_entry(void *context) { run_worker(context); return NULL; }
#endif

static void test_concurrency(void) {
    char path[320]; make_test_path(path, sizeof(path), "concurrent");
    DbError error;
    Db *db = db_open(path, false, &error);
    CHECK(db != NULL);
    ThreadContext workers[6];
#ifdef _WIN32
    HANDLE threads[6];
#else
    pthread_t threads[6];
#endif
    for (unsigned i = 0; i < 6; ++i) {
        workers[i] = (ThreadContext){db, i, i >= 4, false};
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, worker_entry, &workers[i], 0, NULL);
        CHECK(threads[i] != NULL);
#else
        CHECK(pthread_create(&threads[i], NULL, worker_entry, &workers[i]) == 0);
#endif
    }
    for (size_t i = 0; i < 6; ++i) {
#ifdef _WIN32
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CloseHandle(threads[i]);
#else
        CHECK(pthread_join(threads[i], NULL) == 0);
#endif
        CHECK(!workers[i].failed);
    }
    Collected rows = {0};
    run_sql(db, "SELECT * FROM users", &rows);
    CHECK(rows.count == 800 && rows.id_sum == 800u * 801u / 2 && rows.last_id == 800);
    db_close(db);
    db = db_open(path, false, &error); CHECK(db != NULL);
    CHECK(run_sql(db, "SELECT * FROM users WHERE id BETWEEN 1 AND 800", NULL).affected_rows == 800);
    db_close(db);
    remove_test_database(path);
    puts("PASS concurrency: 4 writers + 2 readers, 800 unique consecutive IDs, reopen");
}

int main(void) {
    CHECK(platform_create_directory("build"));
    snprintf(test_root, sizeof(test_root), "build/test-%ld", (long)process_id());
    CHECK(platform_create_directory(test_root));
    test_parser();
    test_bptree_order(0);
    test_bptree_order(1);
    test_bptree_order(2);
    puts("PASS B+ tree: 150,000 inserts, sorted/reverse/random, split invariants, cursor, duplicate keys");
    test_engine();
    test_corruption();
    test_script();
    test_concurrency();
    CHECK(remove_directory(test_root) == 0);
    printf("ALL TESTS PASSED (%u assertions)\n", assertions);
    return 0;
}
