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
    PlatformMutex writer_lock;
    uint64_t published_rows;
    bool failed;
};

static DbStatus rebuild_id_index(Db *db, DbError *error) {
    DbRow rows[STORAGE_READ_BATCH_ROWS];
    for (uint64_t first = 0; first < db->storage.row_count; first += STORAGE_READ_BATCH_ROWS) {
        uint64_t remaining = db->storage.row_count - first;
        size_t count = remaining < STORAGE_READ_BATCH_ROWS ? (size_t)remaining : STORAGE_READ_BATCH_ROWS;
        DbStatus status = storage_read_batch(&db->storage, storage_row_offset(first), rows, count, error);
        if (status != DB_OK) return status;
        for (size_t i = 0; i < count; ++i) {
            if (rows[i].id != first + i + 1)
                return db_error_set(error, DB_CORRUPT, "Unexpected record ID at row %llu", (unsigned long long)(first + i + 1));
            if (!bptree_insert(&db->index, rows[i].id, storage_row_offset(first + i)))
                return db_error_set(error, DB_MEMORY_ERROR, "Cannot rebuild ID index");
        }
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
    platform_mutex_init(&db->writer_lock);
    if (storage_open(&db->storage, directory, sync_writes, error) != DB_OK) {
        platform_rwlock_destroy(&db->table_lock);
        platform_mutex_destroy(&db->writer_lock);
        free(db);
        return NULL;
    }
    if (!bptree_init(&db->index)) {
        db_error_set(error, DB_MEMORY_ERROR, "Cannot allocate ID index");
        db_close(db);
        return NULL;
    }
    if (rebuild_id_index(db, error) != DB_OK) { db_close(db); return NULL; }
    db->published_rows = db->storage.row_count;
    return db;
}

void db_close(Db *db) {
    if (!db) return;
    bptree_destroy(&db->index);
    storage_close(&db->storage);
    platform_rwlock_destroy(&db->table_lock);
    platform_mutex_destroy(&db->writer_lock);
    free(db);
}

DbStatus db_get_stats(Db *db, DbStats *stats, DbError *error) {
    if (!db || !stats) return db_error_set(error, DB_IO_ERROR, "Database and stats output are required");
    platform_read_lock(&db->table_lock);
    stats->row_count = db->published_rows;
    stats->file_bytes = storage_row_offset(db->published_rows);
    stats->index_height = db->index.height;
    stats->index_nodes = db->index.node_count;
    stats->index_memory_bytes = bptree_memory_bytes(&db->index);
    platform_read_unlock(&db->table_lock);
    if (error) memset(error, 0, sizeof(*error));
    return DB_OK;
}

static DbPlan choose_select_plan(const Statement *statement, bool force_scan) {
    const Predicate *predicate = &statement->predicate;
    if (force_scan || !predicate->present || predicate->column != COLUMN_ID ||
        predicate->comparison == COMPARE_NE) return DB_PLAN_SCAN;
    return predicate->comparison == COMPARE_EQ ? DB_PLAN_INDEX_POINT : DB_PLAN_INDEX_RANGE;
}

static DbStatus execute_insert(Db *db, Statement *statement, DbResult *result, DbError *error) {
    /* writer_lock owns Storage's mutable append state. table_lock only protects
     * published_rows, failed, and the B+ tree; never hold it across disk sync. */
    double waiting = platform_now();
    platform_read_lock(&db->table_lock);
    result->lock_wait_ms += (platform_now() - waiting) * 1000.0;
    bool failed = db->failed;
    platform_read_unlock(&db->table_lock);
    if (failed) return db_error_set(error, DB_IO_ERROR, "Database needs to be closed and reopened");
    uint64_t first_row = db->storage.row_count;
    DbStatus status = storage_append(&db->storage, statement->rows, statement->row_count, error);
    if (status != DB_OK) {
        if (db->storage.failed) {
            platform_write_lock(&db->table_lock);
            db->failed = true;
            platform_write_unlock(&db->table_lock);
        }
        return status;
    }
    waiting = platform_now();
    platform_write_lock(&db->table_lock);
    result->lock_wait_ms += (platform_now() - waiting) * 1000.0;
    for (size_t i = 0; i < statement->row_count; ++i) {
        if (!bptree_insert(&db->index, statement->rows[i].id, storage_row_offset(first_row + i))) {
            /* Data is already durable. Prevent inconsistent reads until reopen rebuilds the index. */
            db->failed = true;
            platform_write_unlock(&db->table_lock);
            return db_error_set(error, DB_MEMORY_ERROR, "Rows saved but index allocation failed; reopen database before retrying");
        }
    }
    db->published_rows = db->storage.row_count;
    platform_write_unlock(&db->table_lock);
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

static DbStatus execute_table_scan(Db *db, uint64_t snapshot_rows, const Statement *statement, DbRowVisitor visitor,
                                    void *context, DbResult *result, DbError *error) {
    DbRow rows[STORAGE_READ_BATCH_ROWS];
    for (uint64_t first = 0; first < snapshot_rows && result->affected_rows < statement->limit; first += STORAGE_READ_BATCH_ROWS) {
        uint64_t remaining = snapshot_rows - first;
        size_t count = remaining < STORAGE_READ_BATCH_ROWS ? (size_t)remaining : STORAGE_READ_BATCH_ROWS;
        DbStatus status = storage_read_batch(&db->storage, storage_row_offset(first), rows, count, error);
        if (status != DB_OK) return status;
        for (size_t i = 0; i < count && result->affected_rows < statement->limit; ++i) {
            status = visit_matching_row(&rows[i], statement, visitor, context, result, error);
            if (status != DB_OK) return status;
        }
    }
    return DB_OK;
}

static DbStatus execute_index_lookup(Db *db, uint64_t offset, const Statement *statement, DbRowVisitor visitor,
                                     void *context, DbResult *result, DbError *error) {
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

static double acquire_select_lock(Db *db, DbResult *result) {
    double start = platform_now();
    platform_read_lock(&db->table_lock);
    double acquired = platform_now();
    result->lock_wait_ms += (acquired - start) * 1000.0;
    return acquired;
}

static void release_select_lock(Db *db, DbResult *result, double acquired) {
    result->lock_hold_ms += (platform_now() - acquired) * 1000.0;
    platform_read_unlock(&db->table_lock);
}

/* Copy offsets, never retain a cursor or node pointer across an unlock:
 * concurrent INSERT may split and move keys in that node. */
static DbStatus collect_range_offsets(Db *db, uint64_t *next_key, uint64_t upper,
                                     uint64_t *offsets, size_t capacity, size_t *count,
                                     DbResult *result, DbError *error) {
    double acquired = acquire_select_lock(db, result);
    *count = 0;
    DbStatus status = DB_OK;
    if (db->failed)
        status = db_error_set(error, DB_IO_ERROR, "Database needs to be closed and reopened");
    else {
        BptCursor cursor = bptree_lower_bound(&db->index, *next_key);
        uint64_t key, offset;
        while (*count < capacity && bptree_cursor_next(&cursor, &key, &offset)) {
            if (key > upper) break;
            offsets[(*count)++] = offset;
            *next_key = key + 1; /* Engine IDs are <= INT64_MAX. */
        }
    }
    release_select_lock(db, result, acquired);
    return status;
}

static DbStatus execute_index_range(Db *db, uint64_t snapshot_rows, const Statement *statement,
                                    DbRowVisitor visitor, void *context, DbResult *result, DbError *error) {
    uint64_t lower, upper;
    if (!determine_id_range(&statement->predicate, &lower, &upper)) return DB_OK;
    if (upper > snapshot_rows) upper = snapshot_rows;
    uint64_t offsets[STORAGE_READ_BATCH_ROWS];
    DbRow rows[STORAGE_READ_BATCH_ROWS];
    while (lower <= upper && result->affected_rows < statement->limit) {
        size_t count;
        uint64_t remaining_limit = statement->limit - result->affected_rows;
        size_t capacity = remaining_limit < STORAGE_READ_BATCH_ROWS ? (size_t)remaining_limit : STORAGE_READ_BATCH_ROWS;
        DbStatus status = collect_range_offsets(db, &lower, upper, offsets, capacity, &count, result, error);
        if (status != DB_OK) return status;
        if (!count) break;
        /* Coalesce adjacent file positions; nonadjacent positions still work. */
        for (size_t first = 0; first < count;) {
            size_t run = 1;
            while (first + run < count && offsets[first + run] == offsets[first] + run * STORAGE_RECORD_BYTES) ++run;
            status = storage_read_batch(&db->storage, offsets[first], rows, run, error);
            if (status != DB_OK) return status;
            for (size_t i = 0; i < run; ++i) {
                status = visit_matching_row(&rows[i], statement, visitor, context, result, error);
                if (status != DB_OK) return status;
            }
            first += run;
        }
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
    double acquired = acquire_select_lock(db, result);
    DbStatus status = DB_OK;
    uint64_t snapshot_rows = db->published_rows, point_offset = 0;
    bool point_found = false;
    if (db->failed)
        status = db_error_set(error, DB_IO_ERROR, "Database needs to be closed and reopened");
    else if (!statement->explain && statement->limit && result->plan == DB_PLAN_INDEX_POINT && statement->predicate.number > 0)
        point_found = bptree_find(&db->index, (uint64_t)statement->predicate.number, &point_offset);
    release_select_lock(db, result, acquired);
    if (status != DB_OK || statement->explain || statement->limit == 0) return status;
    /* Records are immutable after publication. Appends beyond snapshot_rows
     * cannot change this SELECT; disk reads and callbacks need no table lock. */
    switch (result->plan) {
        case DB_PLAN_INDEX_POINT: return point_found ? execute_index_lookup(db, point_offset, statement, visitor, context, result, error) : DB_OK;
        case DB_PLAN_INDEX_RANGE: return execute_index_range(db, snapshot_rows, statement, visitor, context, result, error);
        default: return execute_table_scan(db, snapshot_rows, statement, visitor, context, result, error);
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
    if (write) {
        double waiting_since = platform_now();
        platform_mutex_lock(&db->writer_lock);
        double acquired_at = platform_now();
        result->lock_wait_ms = (acquired_at - waiting_since) * 1000.0;
        status = execute_insert(db, &statement, result, error);
        result->lock_hold_ms = (platform_now() - acquired_at) * 1000.0;
        platform_mutex_unlock(&db->writer_lock);
    } else status = execute_select(db, &statement, force_scan, visitor, context, result, error);
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
