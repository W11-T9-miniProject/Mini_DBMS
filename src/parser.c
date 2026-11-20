#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SQL_MAX_BYTES (4u * 1024u * 1024u)
#define INSERT_MAX_ROWS 4096u

typedef enum {
    TOKEN_END, TOKEN_WORD, TOKEN_STRING, TOKEN_NUMBER, TOKEN_STAR,
    TOKEN_COMMA, TOKEN_LEFT, TOKEN_RIGHT, TOKEN_SEMICOLON,
    TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_LE, TOKEN_GT, TOKEN_GE
} TokenKind;

typedef struct {
    const char *sql;
    size_t position;
    size_t token_position;
    TokenKind token;
    char text[DB_EMAIL_SIZE];
    int64_t number;
    DbError *error;
} Parser;

static bool reject(Parser *parser, const char *message) {
    db_error_set(parser->error, DB_SQL_ERROR, "At byte %zu: %s", parser->token_position, message);
    return false;
}

static bool skip_whitespace_and_comments(Parser *parser) {
    const char *sql = parser->sql;
    for (;;) {
        while (isspace((unsigned char)sql[parser->position])) ++parser->position;
        if (sql[parser->position] == '-' && sql[parser->position + 1] == '-') {
            while (sql[parser->position] && sql[parser->position] != '\n') ++parser->position;
        } else if (sql[parser->position] == '/' && sql[parser->position + 1] == '*') {
            parser->token_position = parser->position;
            parser->position += 2;
            while (sql[parser->position] &&
                   !(sql[parser->position] == '*' && sql[parser->position + 1] == '/'))
                ++parser->position;
            if (!sql[parser->position]) return reject(parser, "Unterminated block comment");
            parser->position += 2;
        } else return true;
    }
}

static bool read_string(Parser *parser) {
    size_t length = 0;
    ++parser->position;
    for (;;) {
        char current = parser->sql[parser->position++];
        if (!current) return reject(parser, "Unterminated string literal");
        if (current == '\'') {
            if (parser->sql[parser->position] != '\'') break;
            ++parser->position;
        }
        if (length + 1 >= sizeof(parser->text)) return reject(parser, "String exceeds 127 bytes");
        parser->text[length++] = current;
    }
    parser->text[length] = '\0';
    parser->token = TOKEN_STRING;
    return true;
}

static bool next_token(Parser *parser) {
    if (!skip_whitespace_and_comments(parser)) return false;
    parser->token_position = parser->position;
    const char *start = parser->sql + parser->position;
    unsigned char current = (unsigned char)*start;
    if (!current) { parser->token = TOKEN_END; return true; }
    if (current == '\'') return read_string(parser);
    if (isalpha(current) || current == '_') {
        size_t length = 0;
        while (isalnum((unsigned char)start[length]) || start[length] == '_') {
            if (length + 1 >= sizeof(parser->text)) return reject(parser, "Identifier is too long");
            parser->text[length] = (char)tolower((unsigned char)start[length]);
            ++length;
        }
        parser->text[length] = '\0';
        parser->position += length;
        parser->token = TOKEN_WORD;
        return true;
    }
    if (isdigit(current) || (current == '-' && isdigit((unsigned char)start[1]))) {
        char *end;
        errno = 0;
        long long number = strtoll(start, &end, 10);
        if (errno == ERANGE) return reject(parser, "Integer exceeds signed 64-bit range");
        parser->number = (int64_t)number;
        parser->position += (size_t)(end - start);
        parser->token = TOKEN_NUMBER;
        return true;
    }
    ++parser->position;
    switch (current) {
        case '*': parser->token = TOKEN_STAR; break;
        case ',': parser->token = TOKEN_COMMA; break;
        case '(': parser->token = TOKEN_LEFT; break;
        case ')': parser->token = TOKEN_RIGHT; break;
        case ';': parser->token = TOKEN_SEMICOLON; break;
        case '=': parser->token = TOKEN_EQ; break;
        case '<':
            if (*++start == '=') { ++parser->position; parser->token = TOKEN_LE; }
            else if (*start == '>') { ++parser->position; parser->token = TOKEN_NE; }
            else parser->token = TOKEN_LT;
            break;
        case '>':
            if (*++start == '=') { ++parser->position; parser->token = TOKEN_GE; }
            else parser->token = TOKEN_GT;
            break;
        case '!':
            if (*++start != '=') return reject(parser, "Expected !=");
            ++parser->position;
            parser->token = TOKEN_NE;
            break;
        default: return reject(parser, "Unexpected character");
    }
    return true;
}

static bool is_keyword(const Parser *parser, const char *word) {
    return parser->token == TOKEN_WORD && strcmp(parser->text, word) == 0;
}

static bool consume_keyword(Parser *parser, const char *word) {
    if (!is_keyword(parser, word)) {
        char message[160];
        snprintf(message, sizeof(message), "Expected keyword '%s'", word);
        return reject(parser, message);
    }
    return next_token(parser);
}

static bool consume_token(Parser *parser, TokenKind token, const char *message) {
    if (parser->token != token) return reject(parser, message);
    return next_token(parser);
}

const char *sql_column_name(Column column) {
    static const char *names[] = {"id", "name", "age", "email"};
    return names[column];
}

static bool parse_column(Parser *parser, Column *column) {
    if (parser->token != TOKEN_WORD) return reject(parser, "Expected a column name");
    for (int i = COLUMN_ID; i <= COLUMN_EMAIL; ++i) {
        if (strcmp(parser->text, sql_column_name((Column)i)) == 0) {
            *column = (Column)i;
            return next_token(parser);
        }
    }
    return reject(parser, "Unknown column (expected id, name, age or email)");
}

static bool parse_table(Parser *parser) {
    if (!is_keyword(parser, "users")) return reject(parser, "Unknown table (only 'users' exists)");
    return next_token(parser);
}

static bool parse_column_list(Parser *parser, Statement *statement) {
    bool seen[4] = {false};
    do {
        Column column;
        if (!parse_column(parser, &column)) return false;
        if (seen[column]) return reject(parser, "Duplicate column");
        seen[column] = true;
        statement->columns[statement->column_count++] = column;
        if (parser->token != TOKEN_COMMA) return true;
        if (!next_token(parser)) return false;
    } while (statement->column_count < 4);
    return reject(parser, "Too many columns");
}

static bool parse_insert_value(Parser *parser, Column column, DbRow *row) {
    if (column == COLUMN_ID) return reject(parser, "ID is assigned automatically");
    if (column == COLUMN_AGE) {
        if (parser->token != TOKEN_NUMBER || parser->number < INT32_MIN || parser->number > INT32_MAX)
            return reject(parser, "age requires a signed 32-bit integer");
        row->age = (int32_t)parser->number;
    } else {
        if (parser->token != TOKEN_STRING) return reject(parser, "Expected a quoted string");
        size_t length = strlen(parser->text);
        size_t capacity = column == COLUMN_NAME ? DB_NAME_SIZE : DB_EMAIL_SIZE;
        if (length >= capacity) return reject(parser, "name exceeds 63 UTF-8 bytes");
        memcpy(column == COLUMN_NAME ? row->name : row->email, parser->text, length + 1);
    }
    return next_token(parser);
}

static bool parse_insert_tuple(Parser *parser, const Statement *statement, DbRow *row) {
    if (!consume_token(parser, TOKEN_LEFT, "Expected '(' before VALUES tuple")) return false;
    memset(row, 0, sizeof(*row));
    for (size_t i = 0; i < statement->column_count; ++i) {
        if (i && !consume_token(parser, TOKEN_COMMA, "Expected ',' between values")) return false;
        if (!parse_insert_value(parser, statement->columns[i], row)) return false;
    }
    return consume_token(parser, TOKEN_RIGHT, "Expected ')' after values (check value count)");
}

static bool parse_insert(Parser *parser, Statement *statement) {
    statement->kind = STATEMENT_INSERT;
    if (!consume_keyword(parser, "insert") || !consume_keyword(parser, "into") || !parse_table(parser))
        return false;
    if (parser->token == TOKEN_LEFT) {
        if (!next_token(parser) || !parse_column_list(parser, statement) ||
            !consume_token(parser, TOKEN_RIGHT, "Expected ')' after columns")) return false;
    } else {
        statement->columns[0] = COLUMN_NAME;
        statement->columns[1] = COLUMN_AGE;
        statement->columns[2] = COLUMN_EMAIL;
        statement->column_count = 3;
    }
    bool has_name = false, has_age = false;
    for (size_t i = 0; i < statement->column_count; ++i) {
        if (statement->columns[i] == COLUMN_ID) return reject(parser, "ID is assigned automatically");
        has_name |= statement->columns[i] == COLUMN_NAME;
        has_age |= statement->columns[i] == COLUMN_AGE;
    }
    if (!has_name || !has_age) return reject(parser, "INSERT requires name and age; email defaults to ''");
    if (!consume_keyword(parser, "values")) return false;
    size_t capacity = 0;
    do {
        if (statement->row_count == INSERT_MAX_ROWS) return reject(parser, "Maximum 4096 tuples per INSERT");
        if (statement->row_count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            DbRow *rows = realloc(statement->rows, capacity * sizeof(*rows));
            if (!rows) {
                db_error_set(parser->error, DB_MEMORY_ERROR, "Cannot allocate INSERT rows");
                return false;
            }
            statement->rows = rows;
        }
        if (!parse_insert_tuple(parser, statement, &statement->rows[statement->row_count])) return false;
        ++statement->row_count;
        if (parser->token != TOKEN_COMMA) return true;
        if (!next_token(parser)) return false;
    } while (true);
}

static bool parse_predicate(Parser *parser, Predicate *predicate) {
    predicate->present = true;
    if (!consume_keyword(parser, "where") || !parse_column(parser, &predicate->column)) return false;
    bool between = is_keyword(parser, "between");
    if (between) predicate->comparison = COMPARE_BETWEEN;
    else {
        switch (parser->token) {
            case TOKEN_EQ: predicate->comparison = COMPARE_EQ; break;
            case TOKEN_NE: predicate->comparison = COMPARE_NE; break;
            case TOKEN_LT: predicate->comparison = COMPARE_LT; break;
            case TOKEN_LE: predicate->comparison = COMPARE_LE; break;
            case TOKEN_GT: predicate->comparison = COMPARE_GT; break;
            case TOKEN_GE: predicate->comparison = COMPARE_GE; break;
            default: return reject(parser, "Expected =, !=, <>, <, <=, >, >= or BETWEEN");
        }
    }
    if (!next_token(parser)) return false;
    if (predicate->column == COLUMN_ID || predicate->column == COLUMN_AGE) {
        if (parser->token != TOKEN_NUMBER) return reject(parser, "Numeric column requires integer condition");
        predicate->number = parser->number;
    } else {
        if (between) return reject(parser, "BETWEEN supports numeric columns only");
        if (parser->token != TOKEN_STRING) return reject(parser, "String column requires quoted condition");
        memcpy(predicate->string, parser->text, strlen(parser->text) + 1);
    }
    if (!next_token(parser)) return false;
    if (between) {
        if (!consume_keyword(parser, "and")) return false;
        if (parser->token != TOKEN_NUMBER) return reject(parser, "Expected upper bound integer");
        predicate->upper = parser->number;
        if (!next_token(parser)) return false;
    }
    return true;
}

static bool parse_select(Parser *parser, Statement *statement) {
    statement->kind = STATEMENT_SELECT;
    if (!consume_keyword(parser, "select")) return false;
    if (parser->token == TOKEN_STAR) {
        for (int i = 0; i < 4; ++i) statement->columns[statement->column_count++] = (Column)i;
        if (!next_token(parser)) return false;
    } else if (!parse_column_list(parser, statement)) return false;
    if (!consume_keyword(parser, "from") || !parse_table(parser)) return false;
    if (is_keyword(parser, "where") && !parse_predicate(parser, &statement->predicate)) return false;
    if (is_keyword(parser, "limit")) {
        if (!next_token(parser)) return false;
        if (parser->token != TOKEN_NUMBER || parser->number < 0) return reject(parser, "LIMIT requires nonnegative integer");
        statement->limit = (uint64_t)parser->number;
        if (!next_token(parser)) return false;
    }
    return true;
}

DbStatus sql_parse(const char *sql, Statement *statement, DbError *error) {
    DbError local_error = {0};
    if (!error) error = &local_error;
    memset(error, 0, sizeof(*error));
    memset(statement, 0, sizeof(*statement));
    statement->limit = UINT64_MAX;
    if (!sql || strlen(sql) > SQL_MAX_BYTES)
        return db_error_set(error, DB_SQL_ERROR, "SQL must be non-null and at most 4 MiB");
    Parser parser = {0};
    parser.sql = sql;
    parser.error = error;
    bool success = next_token(&parser);
    if (success && is_keyword(&parser, "explain")) {
        statement->explain = true;
        success = next_token(&parser);
        if (success && !is_keyword(&parser, "select")) success = reject(&parser, "EXPLAIN requires SELECT");
    }
    if (success) {
        if (is_keyword(&parser, "insert")) success = parse_insert(&parser, statement);
        else if (is_keyword(&parser, "select")) success = parse_select(&parser, statement);
        else success = reject(&parser, "Expected INSERT, SELECT or EXPLAIN SELECT");
    }
    if (success && parser.token == TOKEN_SEMICOLON) success = next_token(&parser);
    if (success && parser.token != TOKEN_END) success = reject(&parser, "Unexpected trailing SQL; execute one statement at a time");
    if (!success) {
        sql_statement_destroy(statement);
        return error->code;
    }
    return DB_OK;
}

void sql_statement_destroy(Statement *statement) {
    free(statement->rows);
    statement->rows = NULL;
    statement->row_count = 0;
}

static bool compare_order(int order, Comparison comparison) {
    switch (comparison) {
        case COMPARE_EQ: return order == 0;
        case COMPARE_NE: return order != 0;
        case COMPARE_LT: return order < 0;
        case COMPARE_LE: return order <= 0;
        case COMPARE_GT: return order > 0;
        case COMPARE_GE: return order >= 0;
        case COMPARE_BETWEEN: return false;
    }
    return false;
}

bool sql_row_matches(const DbRow *row, const Predicate *predicate) {
    if (!predicate->present) return true;
    if (predicate->column == COLUMN_NAME || predicate->column == COLUMN_EMAIL) {
        const char *value = predicate->column == COLUMN_NAME ? row->name : row->email;
        return compare_order(strcmp(value, predicate->string), predicate->comparison);
    }
    int64_t value = predicate->column == COLUMN_ID ? (int64_t)row->id : row->age;
    if (predicate->comparison == COMPARE_BETWEEN)
        return value >= predicate->number && value <= predicate->upper;
    return compare_order((value > predicate->number) - (value < predicate->number), predicate->comparison);
}
