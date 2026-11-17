#define _GNU_SOURCE
#include "engine.h"
#include "bptree.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct db_engine { db_file *file; bptree *index; int failed; pthread_rwlock_t lock; };

static db_status index_status(bpt_status status) {
    return status == BPT_OK ? DB_OK : status == BPT_NO_MEMORY ? DB_NO_MEMORY : DB_CORRUPT_FILE;
}

static db_status open_engine(db_engine **out, const char *path, int exclusive_create) {
    if (!out) return DB_INVALID_ARGUMENT;
    *out = NULL;
    db_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) return DB_NO_MEMORY;
    pthread_rwlockattr_t attr;
    if (pthread_rwlockattr_init(&attr)) { free(engine); return DB_IO_ERROR; }
    /* Linux target: waiting writers must not be starved by arriving readers. */
    int configured = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    int initialized = configured ? configured : pthread_rwlock_init(&engine->lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    if (initialized) { free(engine); return DB_IO_ERROR; }
    db_status status = exclusive_create ? db_create_new(&engine->file, path) : db_open(&engine->file, path);
    if (status != DB_OK) { pthread_rwlock_destroy(&engine->lock); free(engine); return status; }
    status = index_status(bpt_create(&engine->index));
    for (uint64_t i = 0; status == DB_OK && i < db_count(engine->file); ++i) {
        uint64_t offset = DB_HEADER_SIZE + i * DB_RECORD_SIZE;
        play_record record;
        status = db_read_at(engine->file, offset, &record);
        if (status == DB_OK) status = index_status(bpt_insert(engine->index, record.id, offset));
    }
    if (status != DB_OK) {
        bpt_destroy(engine->index); db_close(engine->file); pthread_rwlock_destroy(&engine->lock); free(engine); return status;
    }
    *out = engine;
    return DB_OK;
}

db_status engine_open(db_engine **out, const char *path) { return open_engine(out, path, 0); }
db_status engine_create_new(db_engine **out, const char *path) { return open_engine(out, path, 1); }

static int valid_condition(const sql_statement *s) {
    if (s->has_where != 0 && s->has_where != 1) return 0;
    if (!s->has_where) return 1;
    if (s->field < SQL_ID || s->field > SQL_GENRE) return 0;
    if (s->field == SQL_EVENT_CODE || s->field == SQL_GENRE) {
        size_t capacity = 32;
        return s->value.kind == SQL_STRING && strnlen(s->value.text, 32) < capacity;
    }
    return s->value.kind == SQL_NUMBER;
}
static int matches(const play_record *record, const sql_statement *s) {
    if (!s->has_where) return 1;
    switch (s->field) {
        case SQL_ID: return record->id == s->value.number;
        case SQL_EVENT_CODE: return strcmp(record->event_code, s->value.text) == 0;
        case SQL_USER_ID: return record->user_id == s->value.number;
        case SQL_SONG_ID: return record->song_id == s->value.number;
        case SQL_GENRE: return strcmp(record->genre, s->value.text) == 0;
    }
    return 0;
}

static db_status execute_locked(db_engine *engine, const sql_statement *statement,
                         db_row_fn row, void *context, db_result *result) {
    if (!engine || !statement || !result) return DB_INVALID_ARGUMENT;
    if (engine->failed) return DB_IO_ERROR;
    db_result current = {0};
    if (statement->kind == SQL_INSERT) {
        db_status status = index_status(bpt_prepare(engine->index));
        if (status != DB_OK) return status;
        uint64_t offset;
        status = db_append(engine->file, &statement->record, &current.inserted, &offset);
        if (status != DB_OK) return status;
        /* prepare reserved all split nodes before touching the file. */
        status = index_status(bpt_insert(engine->index, current.inserted.id, offset));
        if (status != DB_OK) { engine->failed = 1; return status; }
        current.affected = 1;
    } else if (statement->kind == SQL_SELECT) {
        if (!valid_condition(statement)) return DB_INVALID_ARGUMENT;
        if (statement->has_where && statement->field == SQL_ID) {
            current.used_index = 1;
            uint64_t offset; size_t visited;
            bpt_status found = bpt_find(engine->index, statement->value.number, &offset, &visited);
            current.index_nodes = visited;
            if (found == BPT_OK) {
                play_record record;
                db_status status = db_read_at(engine->file, offset, &record);
                if (status != DB_OK) return status;
                if (record.id != statement->value.number) return DB_CORRUPT_FILE;
                current.scanned = 1; current.affected = 1;
                if (row && row(&record, context) != 0) return DB_IO_ERROR;
            } else if (found != BPT_NOT_FOUND) return index_status(found);
            *result = current;
            return DB_OK;
        }
        uint64_t count = db_count(engine->file);
        for (uint64_t i = 0; i < count; ++i) {
            play_record record;
            db_status status = db_read_at(engine->file, DB_HEADER_SIZE + i * DB_RECORD_SIZE, &record);
            if (status != DB_OK) return status;
            current.scanned++;
            if (!matches(&record, statement)) continue;
            current.affected++;
            if (row && row(&record, context) != 0) return DB_IO_ERROR;
        }
    } else return DB_INVALID_ARGUMENT;
    *result = current;
    return DB_OK;
}

static db_status execute_mode(db_engine *engine, const sql_statement *s,
                              db_row_fn row, void *context, db_result *result, int sync) {
    if (!engine || !s || !result || (s->kind != SQL_INSERT && s->kind != SQL_SELECT)) return DB_INVALID_ARGUMENT;
    int locked = s->kind == SQL_INSERT ? pthread_rwlock_wrlock(&engine->lock) : pthread_rwlock_rdlock(&engine->lock);
    if (locked) return DB_IO_ERROR;
    db_result pending;
    db_status status = execute_locked(engine, s, row, context, &pending);
    if (status == DB_OK && sync && s->kind == SQL_INSERT) status = db_sync(engine->file);
    if (status == DB_OK) *result = pending;
    pthread_rwlock_unlock(&engine->lock);
    return status;
}
db_status engine_execute(db_engine *engine, const sql_statement *s,
                         db_row_fn row, void *context, db_result *result) {
    return execute_mode(engine, s, row, context, result, 0);
}
db_status engine_execute_sync(db_engine *engine, const sql_statement *s,
                              db_row_fn row, void *context, db_result *result) {
    return execute_mode(engine, s, row, context, result, 1);
}
db_status engine_sync(db_engine *engine) {
    if (!engine) return DB_INVALID_ARGUMENT;
    if (pthread_rwlock_wrlock(&engine->lock)) return DB_IO_ERROR;
    db_status status = db_sync(engine->file);
    pthread_rwlock_unlock(&engine->lock); return status;
}
db_status engine_close(db_engine *engine) {
    if (!engine) return DB_INVALID_ARGUMENT;
    db_status status = db_close(engine->file);
    bpt_destroy(engine->index);
    pthread_rwlock_destroy(&engine->lock);
    free(engine);
    return status;
}
