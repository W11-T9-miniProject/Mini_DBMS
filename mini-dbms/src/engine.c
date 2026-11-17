#include "db.h"
#include "bptree.h"
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum { END, IDENT, NUMBER, STRING, SYMBOL } Kind;
typedef struct {
    char *data_path;
    Bptree *tree;
    int id_column;
    int64_t next_id;
} IndexEntry;
typedef struct { IndexEntry *entries; size_t count, capacity; } IndexRegistry;
typedef struct {
    const char *directory, *sql, *cursor;
    char *token;
    Kind kind;
    FILE *output;
    char *error;
    size_t error_size;
    int failed;
    IndexRegistry *indexes;
    int indexes_sealed;
    size_t output_limit;
} Parser;

struct DbEngine {
    char *directory;
    IndexRegistry indexes;
    pthread_rwlock_t lock;
    FILE *lock_file;
    int poisoned;
};

static int error(Parser *p, const char *format, ...) {
    if (!p->failed) {
        size_t used = (size_t)snprintf(p->error, p->error_size, "At byte %zu: ", (size_t)(p->cursor - p->sql));
        if (used < p->error_size) {
            va_list args;
            va_start(args, format);
            vsnprintf(p->error + used, p->error_size - used, format, args);
            va_end(args);
        }
    }
    p->failed = 1;
    return -1;
}

static void next(Parser *p) {
    free(p->token);
    p->token = NULL;
    if (p->failed) return;
    while (isspace((unsigned char)*p->cursor)) ++p->cursor;
    const char *start = p->cursor;
    char c = *p->cursor;
    if (!c) { p->kind = END; return; }
    if (isalpha((unsigned char)c) || c == '_') {
        p->kind = IDENT;
        do { ++p->cursor; } while (isalnum((unsigned char)*p->cursor) || *p->cursor == '_');
    } else if (isdigit((unsigned char)c) || ((c == '-' || c == '+') && isdigit((unsigned char)p->cursor[1]))) {
        p->kind = NUMBER;
        ++p->cursor;
        while (isdigit((unsigned char)*p->cursor)) ++p->cursor;
    } else if (c == '\'') {
        p->kind = STRING;
        ++p->cursor;
        size_t capacity = 64, length = 0;
        p->token = malloc(capacity);
        if (!p->token) { error(p, "Out of memory"); return; }
        while (*p->cursor) {
            if (*p->cursor == '\'') {
                ++p->cursor;
                if (*p->cursor != '\'') { p->token[length] = 0; return; }
            }
            if (length + 1 >= capacity) {
                if (capacity == DB_MAX_TEXT + 1) { error(p, "String exceeds maximum length"); return; }
                size_t next_capacity = capacity * 2;
                if (next_capacity > DB_MAX_TEXT + 1) next_capacity = DB_MAX_TEXT + 1;
                char *expanded = realloc(p->token, next_capacity);
                if (!expanded) { error(p, "Out of memory"); return; }
                p->token = expanded;
                capacity = next_capacity;
            }
            p->token[length++] = *p->cursor++;
            if (length > DB_MAX_TEXT) { error(p, "String exceeds maximum length"); return; }
        }
        error(p, "Unterminated string literal");
        return;
    } else {
        p->kind = SYMBOL;
        ++p->cursor;
        if (strchr("!<>", c) && *p->cursor == '=') ++p->cursor;
    }
    p->token = strndup(start, (size_t)(p->cursor - start));
    if (!p->token) error(p, "Out of memory");
}

static int is(Parser *p, const char *word) {
    return !p->failed && p->token && p->kind != STRING && !strcasecmp(p->token, word);
}
static int take(Parser *p, const char *word) {
    if (!is(p, word)) return 0;
    next(p);
    return 1;
}
static int expect(Parser *p, const char *word) {
    if (take(p, word)) return 0;
    return error(p, "Expected %s", word);
}
static char *identifier(Parser *p) {
    if (p->failed) return NULL;
    if (p->kind != IDENT) { error(p, "Expected identifier"); return NULL; }
    char *name = strdup(p->token);
    if (!name) { error(p, "Out of memory"); return NULL; }
    next(p);
    return name;
}
static int open_table(Parser *p, DbTable *table) {
    char *name = identifier(p);
    if (!name) return -1;
    char message[256];
    int result = db_table_open(p->directory, name, table, message, sizeof(message));
    free(name);
    if (result) return error(p, "%s", message);
    return 0;
}
static int column(Parser *p, const DbTable *table, const char *name) {
    for (size_t i = 0; i < table->count; ++i)
        if (!strcmp(table->columns[i].name, name)) return (int)i;
    return error(p, "Unknown column: %s", name);
}

static int id_column(const DbTable *table) {
    for (size_t i = 0; i < table->count; ++i)
        if (!strcmp(table->columns[i].name, "id") && table->columns[i].type == DB_INTEGER)
            return (int)i;
    return -1;
}

static void indexes_free(IndexRegistry *registry) {
    for (size_t i = 0; i < registry->count; ++i) {
        free(registry->entries[i].data_path);
        bptree_destroy(registry->entries[i].tree);
    }
    free(registry->entries);
    memset(registry, 0, sizeof(*registry));
}

static IndexEntry *find_index(IndexRegistry *registry, const char *data_path) {
    for (size_t i = 0; i < registry->count; ++i)
        if (!strcmp(registry->entries[i].data_path, data_path)) return &registry->entries[i];
    return NULL;
}

static int open_index(Parser *p, const DbTable *table, IndexEntry **result) {
    int key_column = id_column(table);
    if (key_column < 0) { *result = NULL; return 0; }
    IndexEntry *entry = find_index(p->indexes, table->data_path);
    if (entry) { *result = entry; return 0; }
    if (p->indexes_sealed) return error(p, "Table was not loaded at server startup");
    if (p->indexes->count == p->indexes->capacity) {
        size_t capacity = p->indexes->capacity ? p->indexes->capacity * 2 : 4;
        IndexEntry *entries = realloc(p->indexes->entries, capacity * sizeof(*entries));
        if (!entries) return error(p, "Out of memory");
        p->indexes->entries = entries;
        p->indexes->capacity = capacity;
    }
    entry = &p->indexes->entries[p->indexes->count];
    memset(entry, 0, sizeof(*entry));
    entry->data_path = strdup(table->data_path);
    entry->tree = bptree_create();
    entry->id_column = key_column;
    entry->next_id = 1;
    if (!entry->data_path || !entry->tree) {
        free(entry->data_path);
        bptree_destroy(entry->tree);
        memset(entry, 0, sizeof(*entry));
        return error(p, "Out of memory");
    }
    FILE *file = fopen(table->data_path, "rb");
    if (!file && errno != ENOENT) {
        free(entry->data_path); bptree_destroy(entry->tree); memset(entry, 0, sizeof(*entry));
        return error(p, "Cannot open table data");
    }
    if (file) {
        DbRow row;
        char message[256];
        int read_result;
        while (!p->failed) {
            long offset = ftell(file);
            if (offset < 0) { error(p, "Cannot locate table data"); break; }
            read_result = db_read(file, table, &row, message, sizeof(message));
            if (read_result <= 0) {
                if (read_result < 0) error(p, "%s", message);
                break;
            }
            long long parsed = strtoll(row.values[key_column], NULL, 10);
            int inserted = bptree_insert(entry->tree, parsed, offset);
            db_row_free(&row, table->count);
            if (inserted < 0) { error(p, "Out of memory"); break; }
            if (inserted > 0) { error(p, "Duplicate id in table data"); break; }
            if (entry->next_id && parsed >= entry->next_id) {
                entry->next_id = parsed == LLONG_MAX ? 0 : parsed + 1;
            }
        }
        fclose(file);
    }
    if (p->failed) {
        free(entry->data_path); bptree_destroy(entry->tree); memset(entry, 0, sizeof(*entry));
        return -1;
    }
    ++p->indexes->count;
    *result = entry;
    return 0;
}

static char *literal(Parser *p, DbType type) {
    if (p->failed) return NULL;
    if (p->kind != (type == DB_INTEGER ? NUMBER : STRING)) {
        error(p, "Expected %s literal", type == DB_INTEGER ? "INTEGER" : "TEXT");
        return NULL;
    }
    if (type == DB_TEXT && strpbrk(p->token, "\r\n")) {
        error(p, "TEXT literals cannot contain CR or LF");
        return NULL;
    }
    char *value;
    if (type == DB_INTEGER) {
        errno = 0;
        char *end;
        long long number = strtoll(p->token, &end, 10);
        if (errno || *end) { error(p, "INTEGER out of range"); return NULL; }
        char canonical[32];
        snprintf(canonical, sizeof(canonical), "%lld", number);
        value = strdup(canonical);
    } else value = strdup(p->token);
    if (!value) { error(p, "Out of memory"); return NULL; }
    next(p);
    return value;
}
/* Keep the terminator current until the statement has executed. Lexical errors
 * in the next statement must not prevent this statement from taking effect. */
static int finish(Parser *p) {
    return is(p, ";") ? 0 : error(p, "Expected ;");
}

static int insert(Parser *p) {
    DbTable table = {0};
    DbRow row = {0};
    if (expect(p, "INTO") || open_table(p, &table)) return -1;
    size_t order[DB_MAX_COLUMNS], count = 0;
    int generated_id = 0;
    int key_column = id_column(&table);
    if (take(p, "(")) {
        do {
            char *name = identifier(p);
            if (!name) break;
            int index = column(p, &table, name);
            free(name);
            if (index < 0) break;
            if (count == DB_MAX_COLUMNS) { error(p, "Too many columns"); break; }
            for (size_t i = 0; i < count; ++i)
                if (order[i] == (size_t)index) error(p, "Duplicate INSERT column");
            order[count++] = (size_t)index;
        } while (take(p, ","));
        expect(p, ")");
        if (!p->failed && count != table.count) {
            int contains_id = 0;
            for (size_t i = 0; i < count; ++i) if ((int)order[i] == key_column) contains_id = 1;
            if (key_column >= 0 && !contains_id && count + 1 == table.count) generated_id = 1;
            else error(p, "INSERT must provide every column");
        }
    } else {
        count = table.count;
        for (size_t i = 0; i < count; ++i) order[i] = i;
    }
    if (!p->failed && !expect(p, "VALUES") && !expect(p, "(")) {
        for (size_t i = 0; i < count && !p->failed; ++i) {
            if (i) expect(p, ",");
            row.values[order[i]] = literal(p, table.columns[order[i]].type);
        }
        if (!p->failed) expect(p, ")");
        if (!p->failed) finish(p);
        if (!p->failed && generated_id) {
            IndexEntry *entry;
            if (!open_index(p, &table, &entry) && (!entry || entry->next_id <= 0))
                error(p, "Cannot generate id: INTEGER range exhausted");
            if (!p->failed) {
                char generated[32];
                snprintf(generated, sizeof(generated), "%lld", (long long)entry->next_id);
                row.values[key_column] = strdup(generated);
                if (!row.values[key_column]) error(p, "Out of memory");
            }
        }
        if (!p->failed) {
            IndexEntry *entry = NULL;
            if (key_column >= 0) open_index(p, &table, &entry);
            long long key = 0;
            if (!p->failed && entry) {
                key = strtoll(row.values[key_column], NULL, 10);
                if (bptree_lookup(entry->tree, key, NULL)) error(p, "Duplicate id");
            }
            int64_t row_offset = -1;
            char message[256];
            if (!p->failed && db_append(&table, &row, &row_offset, message, sizeof(message)))
                error(p, "%s", message);
            if (!p->failed && entry) {
                if (bptree_insert(entry->tree, key, row_offset) < 0) {
                    if (db_truncate(&table, row_offset)) error(p, "Cannot roll back table data");
                    else error(p, "Out of memory");
                }
                else if (entry->next_id && key >= entry->next_id)
                    entry->next_id = key == LLONG_MAX ? 0 : key + 1;
            }
        }
    }
    db_row_free(&row, table.count);
    db_table_free(&table);
    return p->failed ? -1 : 0;
}

#define MAX_EXPRESSIONS 256
typedef enum { COMPARE, CONJUNCTION, DISJUNCTION } ExprKind;
typedef struct {
    ExprKind kind;
    int left, right, column, operation;
    char *value;
} Expr;
typedef struct { Expr nodes[MAX_EXPRESSIONS]; size_t count; } Expressions;

static int add_expr(Parser *p, Expressions *expressions, Expr node) {
    if (expressions->count == MAX_EXPRESSIONS) {
        free(node.value);
        return error(p, "WHERE expression is too complex");
    }
    int index = (int)expressions->count++;
    expressions->nodes[index] = node;
    return index;
}
static int parse_or(Parser *p, const DbTable *table, Expressions *expressions, unsigned depth);
static int parse_atom(Parser *p, const DbTable *table, Expressions *expressions, unsigned depth) {
    if (depth > 64) return error(p, "WHERE parentheses are nested too deeply");
    if (take(p, "(")) {
        int result = parse_or(p, table, expressions, depth + 1);
        expect(p, ")");
        return result;
    }
    char *name = identifier(p);
    if (!name) return -1;
    int index = column(p, table, name);
    free(name);
    if (index < 0) return -1;
    const char *operators[] = {"=", "!=", "<", "<=", ">", ">="};
    int operation = -1;
    for (int i = 0; i < 6; ++i) if (is(p, operators[i])) { operation = i; break; }
    if (operation < 0) return error(p, "Expected comparison operator");
    next(p);
    char *value = literal(p, table->columns[index].type);
    if (!value) return -1;
    return add_expr(p, expressions, (Expr){.kind = COMPARE, .column = index,
                                          .operation = operation, .value = value});
}
static int parse_and(Parser *p, const DbTable *table, Expressions *expressions, unsigned depth) {
    int left = parse_atom(p, table, expressions, depth);
    while (!p->failed && take(p, "AND")) {
        int right = parse_atom(p, table, expressions, depth);
        if (p->failed) return -1;
        left = add_expr(p, expressions, (Expr){.kind = CONJUNCTION, .left = left, .right = right});
    }
    return left;
}
static int parse_or(Parser *p, const DbTable *table, Expressions *expressions, unsigned depth) {
    int left = parse_and(p, table, expressions, depth);
    while (!p->failed && take(p, "OR")) {
        int right = parse_and(p, table, expressions, depth);
        if (p->failed) return -1;
        left = add_expr(p, expressions, (Expr){.kind = DISJUNCTION, .left = left, .right = right});
    }
    return left;
}
static int matches(const Expressions *expressions, int index, const DbTable *table, const DbRow *row) {
    if (index < 0) return 1;
    const Expr *node = &expressions->nodes[index];
    if (node->kind == CONJUNCTION)
        return matches(expressions, node->left, table, row) && matches(expressions, node->right, table, row);
    if (node->kind == DISJUNCTION)
        return matches(expressions, node->left, table, row) || matches(expressions, node->right, table, row);
    int comparison;
    if (table->columns[node->column].type == DB_INTEGER) {
        long long left = strtoll(row->values[node->column], NULL, 10);
        long long right = strtoll(node->value, NULL, 10);
        comparison = (left > right) - (left < right);
    } else comparison = strcmp(row->values[node->column], node->value);
    switch (node->operation) {
        case 0: return comparison == 0;
        case 1: return comparison != 0;
        case 2: return comparison < 0;
        case 3: return comparison <= 0;
        case 4: return comparison > 0;
        default: return comparison >= 0;
    }
}

static int write_field(FILE *output, const char *value) {
    int quoted = strpbrk(value, ",\"") != NULL;
    if (quoted && fputc('"', output) == EOF) return -1;
    for (const char *c = value; *c; ++c) {
        if (*c == '"' && fputc('"', output) == EOF) return -1;
        if (fputc((unsigned char)*c, output) == EOF) return -1;
    }
    if (quoted && fputc('"', output) == EOF) return -1;
    return 0;
}

static int write_row(Parser *p, const DbRow *row, const size_t *projection, size_t count) {
    if (p->output_limit) {
        long position = ftell(p->output);
        size_t estimate = count; /* commas between fields and the final newline */
        for (size_t i = 0; i < count; ++i) {
            const char *value = row->values[projection[i]];
            estimate += strlen(value);
            if (strpbrk(value, ",\"")) estimate += 2;
            for (const char *c = value; *c; ++c) if (*c == '\"') ++estimate;
        }
        if (position < 0 || (size_t)position > p->output_limit || estimate > p->output_limit - (size_t)position)
            return error(p, "Query result exceeds output limit");
    }
    for (size_t i = 0; i < count && !p->failed; ++i) {
        if ((i && fputc(',', p->output) == EOF) || write_field(p->output, row->values[projection[i]]))
            return error(p, "Cannot write output");
    }
    if (fputc('\n', p->output) == EOF) return error(p, "Cannot write output");
    return 0;
}

static int select_rows(Parser *p) {
    DbTable table = {0};
    char *names[DB_MAX_COLUMNS] = {0};
    size_t projection[DB_MAX_COLUMNS], count = 0;
    int star = take(p, "*");
    if (!star) {
        do {
            if (count == DB_MAX_COLUMNS) { error(p, "Too many projected columns"); break; }
            names[count] = identifier(p);
            if (!names[count]) break;
            ++count;
        } while (take(p, ","));
    }
    if (!p->failed) expect(p, "FROM");
    if (!p->failed) open_table(p, &table);
    if (!p->failed) {
        if (star) {
            count = table.count;
            for (size_t i = 0; i < count; ++i) projection[i] = i;
        } else for (size_t i = 0; i < count && !p->failed; ++i) {
            int index = column(p, &table, names[i]);
            if (index >= 0) projection[i] = (size_t)index;
        }
    }
    for (size_t i = 0; i < DB_MAX_COLUMNS; ++i) free(names[i]);
    Expressions expressions = {0};
    int predicate = -1;
    if (!p->failed && take(p, "WHERE")) predicate = parse_or(p, &table, &expressions, 0);
    if (!p->failed && !finish(p)) {
        int key_column = id_column(&table);
        int indexed_predicate = predicate >= 0 && expressions.nodes[predicate].kind == COMPARE &&
                                expressions.nodes[predicate].operation == 0 &&
                                expressions.nodes[predicate].column == key_column;
        IndexEntry *entry = NULL;
        if (indexed_predicate) open_index(p, &table, &entry);
        FILE *file = fopen(table.data_path, "rb");
        if (!file && errno != ENOENT) error(p, "Cannot open table data");
        if (file) {
            DbRow row;
            char message[256];
            int result;
            if (!p->failed && indexed_predicate && entry) {
                long long key = strtoll(expressions.nodes[predicate].value, NULL, 10);
                int64_t row_offset;
                if (bptree_lookup(entry->tree, key, &row_offset)) {
                    result = db_read_at(file, row_offset, &table, &row, message, sizeof(message));
                    if (result < 0) error(p, "%s", message);
                    else if (result > 0 && matches(&expressions, predicate, &table, &row))
                        write_row(p, &row, projection, count);
                    if (result > 0) db_row_free(&row, table.count);
                }
            } else while (!p->failed && (result = db_read(file, &table, &row, message, sizeof(message))) > 0) {
                if (matches(&expressions, predicate, &table, &row)) {
                    write_row(p, &row, projection, count);
                }
                db_row_free(&row, table.count);
            }
            if (!p->failed && !indexed_predicate && result < 0) error(p, "%s", message);
            fclose(file);
        }
    }
    for (size_t i = 0; i < expressions.count; ++i) free(expressions.nodes[i].value);
    db_table_free(&table);
    return p->failed ? -1 : 0;
}

static FILE *directory_lock(const char *directory, char *message, size_t size) {
    size_t length = strlen(directory) + 20;
    char *path = malloc(length);
    if (!path) { snprintf(message, size, "Out of memory"); return NULL; }
    snprintf(path, length, "%s/.engine.lock", directory);
    FILE *file = fopen(path, "a+"); free(path);
    if (!file || fcntl(fileno(file), F_SETLK, &(struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET})) {
        if (file) fclose(file);
        snprintf(message, size, "Database is in use or cannot be locked"); return NULL;
    }
    return file;
}

int db_execute(const char *directory, const char *sql, FILE *output, char *message, size_t size) {
    FILE *lock_file = directory_lock(directory, message, size);
    if (!lock_file) return -1;
    IndexRegistry indexes = {0};
    Parser p = {.indexes = &indexes, .directory = directory, .sql = sql, .cursor = sql,
                .output = output, .error = message, .error_size = size};
    if (size) message[0] = 0;
    next(&p);
    while (!p.failed && p.kind != END) {
        if (take(&p, "INSERT")) insert(&p);
        else if (take(&p, "SELECT")) select_rows(&p);
        else error(&p, "Expected INSERT or SELECT");
        if (!p.failed) next(&p);
    }
    free(p.token);
    indexes_free(&indexes);
    fclose(lock_file);
    return p.failed ? -1 : 0;
}


DbEngine *db_engine_open(const char *directory, char *message, size_t size) {
    DbEngine *engine = calloc(1, sizeof(*engine));
    if (!engine) { snprintf(message, size, "Out of memory"); return NULL; }
    engine->directory = strdup(directory);
    if (!engine->directory || pthread_rwlock_init(&engine->lock, NULL)) {
        free(engine->directory); free(engine); snprintf(message, size, "Cannot initialize engine"); return NULL;
    }
    engine->lock_file = directory_lock(directory, message, size);
    if (!engine->lock_file) { db_engine_close(engine); return NULL; }
    DIR *dir = opendir(directory);
    if (!dir) { snprintf(message, size, "Cannot open database directory"); db_engine_close(engine); return NULL; }
    Parser p = {.directory = directory, .sql = "", .cursor = "", .error = message,
                .error_size = size, .indexes = &engine->indexes};
    struct dirent *item;
    while (!p.failed && (item = readdir(dir))) {
        size_t length = strlen(item->d_name);
        if (length <= 7 || strcmp(item->d_name + length - 7, ".schema")) continue;
        char *name = strndup(item->d_name, length - 7);
        DbTable table = {0};
        if (!name) error(&p, "Out of memory");
        else if (db_table_open(directory, name, &table, message, size)) p.failed = 1;
        else { IndexEntry *entry; open_index(&p, &table, &entry); }
        db_table_free(&table); free(name);
    }
    closedir(dir);
    if (p.failed) { db_engine_close(engine); return NULL; }
    return engine;
}

void db_engine_close(DbEngine *engine) {
    if (!engine) return;
    indexes_free(&engine->indexes);
    if (engine->lock_file) fclose(engine->lock_file);
    pthread_rwlock_destroy(&engine->lock);
    free(engine->directory); free(engine);
}

/* Validate the whole request before executing: a rejected batch must not write. */
int db_engine_execute(DbEngine *engine, const char *sql, FILE *output,
                      char *message, size_t size) {
    Parser check = {.directory = engine->directory, .sql = sql, .cursor = sql,
                    .error = message, .error_size = size};
    if (size) message[0] = 0;
    next(&check);
    int writing = is(&check, "INSERT");
    if (!writing && !is(&check, "SELECT")) error(&check, "Expected INSERT or SELECT");
    while (!check.failed && check.kind != END && !is(&check, ";")) next(&check);
    if (!check.failed && !is(&check, ";")) error(&check, "Expected ;");
    if (!check.failed) next(&check);
    if (!check.failed && check.kind != END) error(&check, "Exactly one SQL statement is allowed");
    free(check.token);
    if (check.failed) return -1;
    int locked = writing ? pthread_rwlock_wrlock(&engine->lock) : pthread_rwlock_rdlock(&engine->lock);
    if (locked) { snprintf(message, size, "Cannot lock engine"); return -1; }
    Parser p = {.directory = engine->directory, .sql = sql, .cursor = sql,
                .output = output, .error = message, .error_size = size,
                .indexes = &engine->indexes, .indexes_sealed = 1, .output_limit = 4 * 1024 * 1024};
    if (engine->poisoned) error(&p, "Engine requires restart after a storage or memory failure");
    next(&p);
    if (!p.failed) {
        if (take(&p, "INSERT")) insert(&p);
        else if (take(&p, "SELECT")) select_rows(&p);
    }
    /* An allocation/storage failure after append may need a rebuild. Fail closed. */
    if (writing && p.failed && (strstr(message, "Out of memory") || strstr(message, "write table data") || strstr(message, "roll back table data")))
        engine->poisoned = 1;
    free(p.token);
    pthread_rwlock_unlock(&engine->lock);
    return p.failed ? -1 : 0;
}
