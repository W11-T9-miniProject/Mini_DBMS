#ifndef MINI_DBMS_ENGINE_H
#define MINI_DBMS_ENGINE_H

#include "sql.h"

typedef struct db_engine db_engine;
/* The record pointer is borrowed and valid only during the callback. */
typedef int (*db_row_fn)(const play_record *record, void *context);
typedef struct {
    uint64_t affected;
    uint64_t scanned;
    int used_index;
    uint64_t index_nodes;
    play_record inserted;
} db_result;

/* Concurrent execute/sync allowed. Open/close require exclusive lifetime ownership.
   Callbacks run under the engine lock and must not reenter this engine. */
db_status engine_open(db_engine **out, const char *path);
db_status engine_create_new(db_engine **out, const char *path);
/* NULL row callback counts matching rows without materializing a result list. */
db_status engine_execute(db_engine *engine, const sql_statement *statement,
                         db_row_fn row, void *context, db_result *result);
/* INSERT syncs the file under the same write lock before returning success. */
db_status engine_execute_sync(db_engine *engine, const sql_statement *statement,
                              db_row_fn row, void *context, db_result *result);
db_status engine_sync(db_engine *engine);
db_status engine_close(db_engine *engine);

#endif
