#ifndef MINI_DB_H
#define MINI_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DB_NAME_SIZE 64
#define DB_EMAIL_SIZE 128
#define DB_ERROR_SIZE 256
#define DB_ABI_VERSION 1u

#if defined(_WIN32) && defined(MINIDB_SHARED)
#define DB_API __declspec(dllexport)
#else
#define DB_API
#endif

typedef struct Db Db;

typedef struct {
    uint64_t id;
    int32_t age;
    char name[DB_NAME_SIZE];
    char email[DB_EMAIL_SIZE];
} DbRow;

typedef enum { DB_OK, DB_SQL_ERROR, DB_IO_ERROR, DB_MEMORY_ERROR,
               DB_CORRUPT, DB_BUSY, DB_CALLBACK_ERROR } DbStatus;

typedef struct {
    DbStatus code;
    char message[DB_ERROR_SIZE];
} DbError;

typedef enum { DB_PLAN_INSERT, DB_PLAN_SCAN, DB_PLAN_INDEX_POINT,
               DB_PLAN_INDEX_RANGE } DbPlan;

typedef struct {
    const char *columns[4];
    size_t column_count;
    uint64_t affected_rows;
    uint64_t examined_rows;
    uint64_t first_insert_id;
    DbPlan plan;
    bool explain;
    /* Diagnostic timings, in milliseconds. Included in total query latency. */
    double lock_wait_ms;
    double lock_hold_ms;
} DbResult;

/* Row memory is borrowed and valid only during the callback.
 * Called without table locks. INSERT during a callback is permitted; this
 * SELECT keeps its original row-count snapshot. Do not close this Db here.
 * Return false to abort the query with DB_CALLBACK_ERROR. */
typedef bool (*DbRowVisitor)(const DbRow *row, const DbResult *result, void *context);

typedef struct {
    uint64_t row_count;
    uint64_t file_bytes;
    size_t index_height;
    size_t index_nodes;
    size_t index_memory_bytes;
} DbStats;

/* Opens directory/users.db. Only one process may open a directory at a time.
 * Individual execute calls are safe across threads. close requires joined callers.
 * sync_writes=true uses fsync/_commit before acknowledging an INSERT. */
DB_API Db *db_open(const char *directory, bool sync_writes, DbError *error);
DB_API void db_close(Db *db);
DB_API DbStatus db_get_stats(Db *db, DbStats *stats, DbError *error);

/* Executes exactly one statement. INSERT accepts multiple VALUES tuples.
 * SELECT streams matching rows, so it does not allocate a result-sized array. */
DB_API DbStatus db_execute(Db *db, const char *sql, DbRowVisitor visitor,
                    void *context, DbResult *result, DbError *error);

/* Benchmark/test hook: identical query and parser, with index lookup disabled. */
DB_API DbStatus db_execute_scan(Db *db, const char *sql, DbRowVisitor visitor,
                         void *context, DbResult *result, DbError *error);

DB_API const char *db_plan_name(DbPlan plan);
DB_API const char *db_status_name(DbStatus status);

/* FFI validates version and struct sizes before passing memory across the ABI.
 * kind: 1=DbRow, 2=DbResult, 3=DbError, 4=DbStats. Unknown kind returns 0. */
DB_API uint32_t db_abi_version(void);
DB_API size_t db_abi_sizeof(unsigned kind);

#endif
