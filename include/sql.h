#ifndef MINI_DBMS_SQL_H
#define MINI_DBMS_SQL_H

#include "storage.h"
#include <stddef.h>

typedef enum { SQL_OK, SQL_DONE, SQL_ERROR } sql_status;
typedef enum { SQL_INSERT, SQL_SELECT } sql_kind;
typedef enum { SQL_ID, SQL_EVENT_CODE, SQL_USER_ID, SQL_SONG_ID, SQL_GENRE } sql_field;
typedef enum { SQL_NUMBER, SQL_STRING } sql_value_kind;

typedef struct {
    sql_value_kind kind;
    uint64_t number;
    char text[32];
} sql_value;

typedef struct {
    sql_kind kind;
    size_t line, column;
    play_record record;
    int has_where;
    sql_field field;
    sql_value value;
} sql_statement;

typedef struct {
    const char *text;
    size_t length, position, line, column;
    int failed;
} sql_parser;

typedef struct {
    size_t line, column;
    char message[160];
} sql_error;

/* The source buffer must remain alive; successful statements own all values. */
void sql_parser_init(sql_parser *parser, const char *text, size_t length);
/* SQL_DONE for whitespace/comments only; SQL_ERROR leaves out unchanged. */
sql_status sql_next(sql_parser *parser, sql_statement *out, sql_error *error);

#endif
