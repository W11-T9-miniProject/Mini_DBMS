#ifndef MINI_DB_H
#define MINI_DB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DB_MAX_COLUMNS 128
#define DB_MAX_TEXT (1024 * 1024)

typedef enum { DB_INTEGER, DB_TEXT } DbType;
typedef struct { char *name; DbType type; } DbColumn;
typedef struct {
    DbColumn columns[DB_MAX_COLUMNS];
    size_t count;
    char *data_path;
} DbTable;
typedef struct { char *values[DB_MAX_COLUMNS]; } DbRow;

/* All operations return 0 on success, -1 on failure; read returns 1 for a row.
 * Caller owns tables/rows and releases them with the matching free function. */
int db_table_open(const char *directory, const char *name, DbTable *table,
                  char *error, size_t error_size);
void db_table_free(DbTable *table);
void db_row_free(DbRow *row, size_t count);
/* On success db_append stores the byte offset of the new row in row_offset. */
int db_append(const DbTable *table, const DbRow *row, int64_t *row_offset,
              char *error, size_t size);
int db_truncate(const DbTable *table, int64_t offset);
int db_read(FILE *file, const DbTable *table, DbRow *row, char *error, size_t size);
int db_read_at(FILE *file, int64_t row_offset, const DbTable *table, DbRow *row,
               char *error, size_t size);
int db_execute(const char *directory, const char *sql, FILE *output,
               char *error, size_t error_size);
/* Server engine owns persistent indexes. Concurrent execute calls are supported.
 * Stop all callers before close. Schema/data must not be edited outside the engine. */
typedef struct DbEngine DbEngine;
DbEngine *db_engine_open(const char *directory, char *error, size_t error_size);
void db_engine_close(DbEngine *engine);
int db_engine_execute(DbEngine *engine, const char *sql, FILE *output,
                      char *error, size_t error_size);
#endif
