#include "db_script.h"
#include "internal.h"
#include <ctype.h>
#include <stdlib.h>

#define SCRIPT_MAX_STATEMENT (4u * 1024u * 1024u)
typedef enum { SCRIPT_NORMAL, SCRIPT_STRING, SCRIPT_LINE_COMMENT, SCRIPT_BLOCK_COMMENT } ScriptState;

static bool append_character(char **buffer, size_t *length, size_t *capacity, int character, DbError *error) {
    if (*length >= SCRIPT_MAX_STATEMENT) {
        db_error_set(error, DB_SQL_ERROR, "Statement exceeds 4 MiB");
        return false;
    }
    if (*length + 1 >= *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2 : 1024;
        char *grown = realloc(*buffer, new_capacity);
        if (!grown) { db_error_set(error, DB_MEMORY_ERROR, "Cannot allocate SQL buffer"); return false; }
        *buffer = grown;
        *capacity = new_capacity;
    }
    (*buffer)[(*length)++] = (char)character;
    (*buffer)[*length] = '\0';
    return true;
}

DbStatus db_run_script(FILE *input, DbStatementVisitor visitor, void *context, DbError *error) {
    DbError local_error = {0};
    if (!error) error = &local_error;
    if (!input || !visitor) return db_error_set(error, DB_IO_ERROR, "Input and statement visitor are required");
    char *buffer = NULL;
    size_t length = 0, capacity = 0;
    ScriptState state = SCRIPT_NORMAL;
    bool meaningful = false;
    DbStatus status = DB_OK;
    int current;
    while ((current = fgetc(input)) != EOF) {
        if (current == 0) { status = db_error_set(error, DB_SQL_ERROR, "NUL byte in SQL input"); break; }
        if (!append_character(&buffer, &length, &capacity, current, error)) { status = error->code; break; }
        if (state == SCRIPT_LINE_COMMENT) {
            if (current == '\n') state = SCRIPT_NORMAL;
        } else if (state == SCRIPT_BLOCK_COMMENT) {
            if (current == '*') {
                int next = fgetc(input);
                if (next == '/') {
                    if (!append_character(&buffer, &length, &capacity, next, error)) { status = error->code; break; }
                    state = SCRIPT_NORMAL;
                } else if (next != EOF) ungetc(next, input);
            }
        } else if (state == SCRIPT_STRING) {
            if (current == '\'') {
                int next = fgetc(input);
                if (next == '\'') {
                    if (!append_character(&buffer, &length, &capacity, next, error)) { status = error->code; break; }
                } else {
                    state = SCRIPT_NORMAL;
                    if (next != EOF) ungetc(next, input);
                }
            }
        } else if (current == '-' || current == '/') {
            int next = fgetc(input);
            if ((current == '-' && next == '-') || (current == '/' && next == '*')) {
                if (!append_character(&buffer, &length, &capacity, next, error)) { status = error->code; break; }
                state = current == '-' ? SCRIPT_LINE_COMMENT : SCRIPT_BLOCK_COMMENT;
            } else {
                meaningful = true;
                if (next != EOF) ungetc(next, input);
            }
        } else if (current == '\'') {
            meaningful = true;
            state = SCRIPT_STRING;
        } else if (current == ';') {
            if (meaningful) status = visitor(buffer, context, error);
            length = 0;
            meaningful = false;
            if (status != DB_OK) break;
        } else if (!isspace((unsigned char)current)) meaningful = true;
    }
    if (status == DB_OK && ferror(input)) status = db_error_set(error, DB_IO_ERROR, "Cannot read SQL file");
    if (status == DB_OK && (state == SCRIPT_STRING || state == SCRIPT_BLOCK_COMMENT))
        status = db_error_set(error, DB_SQL_ERROR, "Unterminated string or block comment at end of file");
    if (status == DB_OK && meaningful) status = visitor(buffer, context, error);
    free(buffer);
    return status;
}
