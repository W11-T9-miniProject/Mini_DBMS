#include "internal.h"
#include "bptree.h"
#include "storage.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct Db {
    Storage storage;
    Bptree index;
    PlatformRwLock table_lock;
    bool failed;
};

static DbStatus rebuild_id_index(Db *db, DbError *error) {
    for (uint64_t i = 0; i < db->storage.row_count; ++i) {
        DbRow row;
        uint64_t offset = storage_row_offset(i);
        DbStatus status = storage_read(&db->storage, offset, &row, error);
        if (status != DB_OK) return status;
        if (row.id != i + 1)
            return db_error_set(error, DB_CORRUPT, "Unexpected record ID at row %llu", (unsigned long long)(i + 1));
        if (!bptree_insert(&db->index, row.id, offset))
            return db_error_set(error, DB_MEMORY_ERROR, "Cannot rebuild ID index");
    }
    return DB_OK;
}

Db *db_open(const char *directory, bool sync_writes, DbError *error) {
    DbError local_error = {0};
    if (!error) error = &local_error;
    memset(error, 0, sizeof(*error));
    Db *db = calloc(1, sizeof(*db));
    if (!db) { db_error_set(error, DB_MEMORY_ERROR, "Cannot allocate DB engine"); return NULL; }
    platform_rwlock_init(&db->table_lock);
    if (storage_open(&db->storage, directory, sync_writes, error) != DB_OK) {
        platform_rwlock_destroy(&db->table_lock);
        free(db);
        return NULL;
    }
    if (!bptree_init(&db->index)) {
        db_error_set(error, DB_MEMORY_ERROR, "Cannot allocate ID index");
        db_close(db);
        return NULL;
    }
    if (rebuild_id_index(db, error) != DB_OK) { db_close(db); return NULL; }
    return db;
}

void db_close(Db *db) {
    if (!db) return;
    bptree_destroy(&db->index);
    storage_close(&db->storage);
    platform_rwlock_destroy(&db->table_lock);
    free(db);
}

static DbPlan choose_select_plan(const Statement *statement, bool force_scan) {
    const Predicate *predicate = &statement->predicate;
    if (force_scan || !predicate->present || predicate->column != COLUMN_ID ||
        predicate->comparison == COMPARE_NE) return DB_PLAN_SCAN;
    return predicate->comparison == COMPARE_EQ ? DB_PLAN_INDEX_POINT : DB_PLAN_INDEX_RANGE;
}

static DbStatus execute_insert(Db *db, Statement *statement, DbResult *result, DbError *error) {
    uint64_t first_row = db->storage.row_count;
    DbStatus status = storage_append(&db->storage, statement->rows, statement->row_count, error);
    if (status != DB_OK) return status;
    for (size_t i = 0; i < statement->row_count; ++i) {
        if (!bptree_insert(&db->index, statement->rows[i].id, storage_row_offset(first_row + i))) {
            /* Data is already durable. Prevent inconsistent reads until reopen rebuilds the index. */
            db->failed = true;
            return db_error_set(error, DB_MEMORY_ERROR, "Rows saved but index allocation failed; reopen database before retrying");
        }
    }
    result->plan = DB_PLAN_INSERT;
    result->affected_rows = statement->row_count;
    result->first_insert_id = first_row + 1;
    return DB_OK;
}

static DbStatus visit_matching_row(const DbRow *row, const Statement *statement,
                                   DbRowVisitor visitor, void *context,
                                   DbResult *result, DbError *error) {
    ++result->examined_rows;
    if (!sql_row_matches(row, &statement->predicate)) return DB_OK;
    ++result->affected_rows;
    if (visitor && !visitor(row, result, context))
        return db_error_set(error, DB_CALLBACK_ERROR, "Row visitor aborted query");
    return DB_OK;
}

static DbStatus execute_table_scan(Db *db, const Statement *statement, DbRowVisitor visitor,
                                    void *context, DbResult *result, DbError *error) {
    for (uint64_t i = 0; i < db->storage.row_count && result->affected_rows < statement->limit; ++i) {
        DbRow row;
        DbStatus status = storage_read(&db->storage, storage_row_offset(i), &row, error);
        if (status != DB_OK) return status;
        status = visit_matching_row(&row, statement, visitor, context, result, error);
        if (status != DB_OK) return status;
    }
    return DB_OK;
}

static DbStatus execute_index_lookup(Db *db, const Statement *statement, DbRowVisitor visitor,
                                     void *context, DbResult *result, DbError *error) {
    uint64_t offset;
    if (statement->predicate.number <= 0 ||
        !bptree_find(&db->index, (uint64_t)statement->predicate.number, &offset)) return DB_OK;
    DbRow row;
    DbStatus status = storage_read(&db->storage, offset, &row, error);
    if (status != DB_OK) return status;
    return visit_matching_row(&row, statement, visitor, context, result, error);
}

static bool determine_id_range(const Predicate *predicate, uint64_t *lower, uint64_t *upper) {
    *lower = 1;
    *upper = INT64_MAX;
    int64_t number = predicate->number;
    switch (predicate->comparison) {
        case COMPARE_LT:
            if (number <= 1) return false;
            *upper = (uint64_t)(number - 1);
            break;
        case COMPARE_LE:
            if (number < 1) return false;
            *upper = (uint64_t)number;
            break;
        case COMPARE_GT:
            if (number == INT64_MAX) return false;
            if (number >= 1) *lower = (uint64_t)number + 1;
            break;
        case COMPARE_GE:
            if (number > 1) *lower = (uint64_t)number;
            break;
        case COMPARE_BETWEEN:
            if (predicate->upper < 1 || number > predicate->upper) return false;
            if (number > 1) *lower = (uint64_t)number;
            *upper = (uint64_t)predicate->upper;
            break;
        default: return false;
    }
    return *lower <= *upper;
}

static DbStatus execute_index_range(Db *db, const Statement *statement, DbRowVisitor visitor,
                                    void *context, DbResult *result, DbError *error) {
    uint64_t lower, upper, key, offset;
    if (!determine_id_range(&statement->predicate, &lower, &upper)) return DB_OK;
    BptCursor cursor = bptree_lower_bound(&db->index, lower);
    while (result->affected_rows < statement->limit && bptree_cursor_next(&cursor, &key, &offset)) {
        if (key > upper) break;
        DbRow row;
        DbStatus status = storage_read(&db->storage, offset, &row, error);
        if (status != DB_OK) return status;
        status = visit_matching_row(&row, statement, visitor, context, result, error);
        if (status != DB_OK) return status;
    }
    return DB_OK;
}

static DbStatus execute_select(Db *db, const Statement *statement, bool force_scan,
                               DbRowVisitor visitor, void *context, DbResult *result, DbError *error) {
    result->plan = choose_select_plan(statement, force_scan);
    result->column_count = statement->column_count;
    for (size_t i = 0; i < statement->column_count; ++i)
        result->columns[i] = sql_column_name(statement->columns[i]);
    result->explain = statement->explain;
    if (statement->explain || statement->limit == 0) return DB_OK;
    switch (result->plan) {
        case DB_PLAN_INDEX_POINT: return execute_index_lookup(db, statement, visitor, context, result, error);
        case DB_PLAN_INDEX_RANGE: return execute_index_range(db, statement, visitor, context, result, error);
        default: return execute_table_scan(db, statement, visitor, context, result, error);
    }
}

static DbStatus execute_sql(Db *db, const char *sql, bool force_scan, DbRowVisitor visitor,
                             void *context, DbResult *result, DbError *error) {
    DbError local_error = {0};
    DbResult local_result;
    if (!error) error = &local_error;
    if (!result) result = &local_result;
    memset(error, 0, sizeof(*error));
    memset(result, 0, sizeof(*result));
    if (!db) return db_error_set(error, DB_IO_ERROR, "Database is not open");
    Statement statement;
    DbStatus status = sql_parse(sql, &statement, error);
    if (status != DB_OK) return status;
    bool write = statement.kind == STATEMENT_INSERT;
    if (write) platform_write_lock(&db->table_lock);
    else platform_read_lock(&db->table_lock);
    if (db->failed || db->storage.failed)
        status = db_error_set(error, DB_IO_ERROR, "Database needs to be closed and reopened");
    else if (write) status = execute_insert(db, &statement, result, error);
    else status = execute_select(db, &statement, force_scan, visitor, context, result, error);
    if (write) platform_write_unlock(&db->table_lock);
    else platform_read_unlock(&db->table_lock);
    sql_statement_destroy(&statement);
    return status;
}

DbStatus db_execute(Db *db, const char *sql, DbRowVisitor visitor,
                    void *context, DbResult *result, DbError *error) {
    return execute_sql(db, sql, false, visitor, context, result, error);
}

DbStatus db_execute_scan(Db *db, const char *sql, DbRowVisitor visitor,
                         void *context, DbResult *result, DbError *error) {
    return execute_sql(db, sql, true, visitor, context, result, error);
}
