#include "sql.h"

#include <stdio.h>
#include <string.h>

typedef enum { T_END, T_WORD, T_NUMBER, T_STRING, T_STAR, T_COMMA,
               T_LEFT, T_RIGHT, T_EQUAL, T_SEMICOLON, T_BAD } token_kind;
typedef struct {
    token_kind kind;
    size_t line, column;
    uint64_t number;
    char text[64];
} token;
typedef struct { sql_parser *source; sql_error *error; token current; } reader;

static int fail(reader *r, const char *message) {
    r->source->failed = 1;
    if (r->error) {
        r->error->line = r->current.line;
        r->error->column = r->current.column;
        snprintf(r->error->message, sizeof(r->error->message), "%s", message);
    }
    r->current.kind = T_BAD;
    return 0;
}
static unsigned char peek(const sql_parser *p, size_t ahead) {
    if (p->position >= p->length || ahead >= p->length - p->position) return 0;
    return (unsigned char)p->text[p->position + ahead];
}
static void advance(sql_parser *p) {
    if (p->text[p->position++] == '\n') { p->line++; p->column = 1; }
    else p->column++;
}
static int space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static int alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }

static int next(reader *r) {
    sql_parser *p = r->source;
    memset(&r->current, 0, sizeof(r->current));
    for (;;) {
        while (p->position < p->length && space(peek(p, 0))) advance(p);
        if (peek(p, 0) != '-' || peek(p, 1) != '-') break;
        while (p->position < p->length && peek(p, 0) != '\n') {
            if (!peek(p, 0)) {
                r->current.line = p->line; r->current.column = p->column;
                return fail(r, "NUL byte is not allowed in SQL");
            }
            advance(p);
        }
    }
    token *t = &r->current;
    t->line = p->line; t->column = p->column;
    if (p->position == p->length) { t->kind = T_END; return 1; }
    unsigned char c = peek(p, 0);
    if (alpha(c)) {
        size_t size = 0;
        do {
            if (size == sizeof(t->text) - 1) return fail(r, "SQL identifier is too long");
            t->text[size++] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
            advance(p); c = peek(p, 0);
        } while (alpha(c) || digit(c));
        t->kind = T_WORD; return 1;
    }
    if (digit(c)) {
        uint64_t value = 0;
        do {
            unsigned n = c - '0';
            if (value > (UINT64_MAX - n) / 10) return fail(r, "integer exceeds uint64 range");
            value = value * 10 + n;
            advance(p); c = peek(p, 0);
        } while (digit(c));
        t->kind = T_NUMBER; t->number = value; return 1;
    }
    if (c == '\'') {
        size_t size = 0;
        advance(p);
        while (p->position < p->length) {
            c = peek(p, 0); advance(p);
            if (c == '\'') {
                if (peek(p, 0) == '\'' && p->position < p->length) advance(p);
                else { t->kind = T_STRING; return 1; }
            }
            if (c < 32 || c > 126) return fail(r, "string values must contain printable ASCII characters");
            if (size == 31) return fail(r, "string value exceeds 31 bytes");
            t->text[size++] = (char)c;
        }
        return fail(r, "unterminated string literal");
    }
    advance(p);
    switch (c) {
        case '*': t->kind = T_STAR; break;
        case ',': t->kind = T_COMMA; break;
        case '(': t->kind = T_LEFT; break;
        case ')': t->kind = T_RIGHT; break;
        case '=': t->kind = T_EQUAL; break;
        case ';': t->kind = T_SEMICOLON; break;
        default: return fail(r, c == 0 ? "NUL byte is not allowed in SQL" : "unsupported character in SQL");
    }
    return 1;
}

static int is_word(const reader *r, const char *word) {
    return r->current.kind == T_WORD && strcmp(r->current.text, word) == 0;
}
static int word(reader *r, const char *expected, const char *message) {
    return is_word(r, expected) ? next(r) : fail(r, message);
}
static int symbol(reader *r, token_kind expected, const char *message) {
    return r->current.kind == expected ? next(r) : fail(r, message);
}
static int field(reader *r, sql_field *out) {
    static const char *names[] = {"id", "event_code", "user_id", "song_id", "genre"};
    for (unsigned i = 0; i < 5; ++i) {
        if (is_word(r, names[i])) { *out = (sql_field)i; return next(r); }
    }
    return fail(r, "unknown column; expected id, event_code, user_id, song_id or genre");
}
static int value(reader *r, sql_field column, sql_value *out) {
    memset(out, 0, sizeof(*out));
    int numeric = column == SQL_ID || column == SQL_USER_ID || column == SQL_SONG_ID;
    if (numeric) {
        if (r->current.kind != T_NUMBER) return fail(r, "numeric column requires an unsigned integer literal");
        out->kind = SQL_NUMBER; out->number = r->current.number;
    } else {
        if (r->current.kind != T_STRING) return fail(r, "text column requires a single-quoted string literal");
        size_t length = strlen(r->current.text);
        size_t capacity = 32;
        if (length >= capacity) return fail(r, "string is too long for this column");
        out->kind = SQL_STRING;
        memcpy(out->text, r->current.text, length + 1);
    }
    return next(r);
}

static int insert(reader *r, sql_statement *statement) {
    statement->kind = SQL_INSERT;
    if (!word(r, "insert", "expected INSERT") || !word(r, "into", "expected INTO") ||
        !word(r, "play_history", "only table play_history exists") ||
        !symbol(r, T_LEFT, "INSERT requires an explicit column list")) return 0;
    sql_field columns[4]; unsigned seen = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!field(r, &columns[i])) return 0;
        if (columns[i] == SQL_ID) return fail(r, "id is generated automatically; do not insert id");
        unsigned bit = 1u << columns[i];
        if (seen & bit) return fail(r, "duplicate INSERT column");
        seen |= bit;
        if (!symbol(r, i == 3 ? T_RIGHT : T_COMMA, "INSERT requires exactly four distinct input columns")) return 0;
    }
    if (!word(r, "values", "expected VALUES") || !symbol(r, T_LEFT, "expected '(' before values")) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        sql_value v;
        if (!value(r, columns[i], &v)) return 0;
        switch (columns[i]) {
            case SQL_EVENT_CODE: memcpy(statement->record.event_code, v.text, sizeof(statement->record.event_code)); break;
            case SQL_USER_ID: statement->record.user_id = v.number; break;
            case SQL_SONG_ID: statement->record.song_id = v.number; break;
            case SQL_GENRE: memcpy(statement->record.genre, v.text, sizeof(statement->record.genre)); break;
            case SQL_ID: return fail(r, "id is generated automatically");
        }
        if (!symbol(r, i == 3 ? T_RIGHT : T_COMMA, "INSERT requires exactly four values")) return 0;
    }
    return 1;
}
static int select_rows(reader *r, sql_statement *statement) {
    statement->kind = SQL_SELECT;
    if (!word(r, "select", "expected SELECT") || !symbol(r, T_STAR, "only SELECT * is supported") ||
        !word(r, "from", "expected FROM") || !word(r, "play_history", "only table play_history exists")) return 0;
    if (is_word(r, "where")) {
        statement->has_where = 1;
        if (!next(r) || !field(r, &statement->field) || !symbol(r, T_EQUAL, "only WHERE column = value is supported") ||
            !value(r, statement->field, &statement->value)) return 0;
    }
    return 1;
}

void sql_parser_init(sql_parser *parser, const char *text, size_t length) {
    if (!parser) return;
    *parser = (sql_parser){ .text = text, .length = length, .line = 1, .column = 1 };
}

sql_status sql_next(sql_parser *parser, sql_statement *out, sql_error *error) {
    if (error) memset(error, 0, sizeof(*error));
    if (!parser || !out || (!parser->text && parser->length) || parser->failed) {
        if (error) { error->line = 1; error->column = 1; snprintf(error->message, sizeof(error->message), "invalid or stopped SQL parser"); }
        return SQL_ERROR;
    }
    reader r = { .source = parser, .error = error };
    if (!next(&r)) return SQL_ERROR;
    while (r.current.kind == T_SEMICOLON) if (!next(&r)) return SQL_ERROR;
    if (r.current.kind == T_END) return SQL_DONE;
    sql_statement statement = { .line = r.current.line, .column = r.current.column };
    int ok;
    if (is_word(&r, "insert")) ok = insert(&r, &statement);
    else if (is_word(&r, "select")) ok = select_rows(&r, &statement);
    else ok = fail(&r, "only INSERT and SELECT are supported");
    if (!ok) return SQL_ERROR;
    if (r.current.kind != T_END && r.current.kind != T_SEMICOLON) {
        fail(&r, "expected ';' between statements or end of input"); return SQL_ERROR;
    }
    *out = statement;
    return SQL_OK;
}
