#ifndef MINI_INTERNAL_H
#define MINI_INTERNAL_H

#include "db.h"
#include <stdarg.h>
#include <stdio.h>

DbStatus db_error_set(DbError *error, DbStatus code, const char *format, ...);

typedef enum { COLUMN_ID, COLUMN_NAME, COLUMN_AGE, COLUMN_EMAIL } Column;
typedef enum { COMPARE_EQ, COMPARE_NE, COMPARE_LT, COMPARE_LE,
               COMPARE_GT, COMPARE_GE, COMPARE_BETWEEN } Comparison;
typedef struct {
    bool present;
    Column column;
    Comparison comparison;
    int64_t number;
    int64_t upper;
    char string[DB_EMAIL_SIZE];
} Predicate;

typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementKind;
typedef struct {
    StatementKind kind;
    bool explain;
    Column columns[4];
    size_t column_count;
    Predicate predicate;
    uint64_t limit;
    DbRow *rows;
    size_t row_count;
} Statement;

DbStatus sql_parse(const char *sql, Statement *statement, DbError *error);
void sql_statement_destroy(Statement *statement);
bool sql_row_matches(const DbRow *row, const Predicate *predicate);
const char *sql_column_name(Column column);

#endif
