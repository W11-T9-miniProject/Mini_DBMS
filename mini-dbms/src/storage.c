#include "db.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(char *error, size_t size, const char *message) {
    snprintf(error, size, "%s", message);
    return -1;
}

static char *path_for(const char *directory, const char *name, const char *suffix) {
    size_t size = strlen(directory) + strlen(name) + strlen(suffix) + 2;
    char *path = malloc(size);
    if (path) snprintf(path, size, "%s/%s%s", directory, name, suffix);
    return path;
}

static int identifier(const char *s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (++s; *s; ++s) if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

void db_table_free(DbTable *table) {
    for (size_t i = 0; i < table->count; ++i) free(table->columns[i].name);
    free(table->data_path);
    memset(table, 0, sizeof(*table));
}

int db_table_open(const char *directory, const char *name, DbTable *table,
                  char *error, size_t size) {
    memset(table, 0, sizeof(*table));
    if (!identifier(name)) return fail(error, size, "Invalid table name");
    char *path = path_for(directory, name, ".schema");
    if (!path) return fail(error, size, "Out of memory");
    FILE *file = fopen(path, "r");
    free(path);
    if (!file) return fail(error, size, "Cannot open table schema");
    char *line = NULL;
    size_t capacity = 0;
    int result = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char name_buffer[256], type[32], extra;
        int fields = sscanf(line, " %255s %31s %c", name_buffer, type, &extra);
        if (fields == EOF) continue;
        if (fields != 2 || !identifier(name_buffer) || table->count == DB_MAX_COLUMNS ||
            (strcmp(type, "INTEGER") && strcmp(type, "TEXT"))) {
            result = fail(error, size, "Invalid schema: expected column INTEGER or column TEXT");
            break;
        }
        for (size_t i = 0; i < table->count; ++i) {
            if (!strcmp(table->columns[i].name, name_buffer)) {
                result = fail(error, size, "Duplicate schema column"); break;
            }
        }
        if (result) break;
        DbColumn *column = &table->columns[table->count];
        column->name = strdup(name_buffer);
        if (!column->name) { result = fail(error, size, "Out of memory"); break; }
        column->type = !strcmp(type, "INTEGER") ? DB_INTEGER : DB_TEXT;
        ++table->count;
    }
    if (ferror(file)) result = fail(error, size, "Cannot read schema");
    free(line);
    fclose(file);
    if (!result && !table->count) result = fail(error, size, "Empty schema");
    if (!result) {
        table->data_path = path_for(directory, name, ".rows");
        if (!table->data_path) result = fail(error, size, "Out of memory");
    }
    if (result) db_table_free(table);
    return result;
}

void db_row_free(DbRow *row, size_t count) {
    for (size_t i = 0; i < count; ++i) { free(row->values[i]); row->values[i] = NULL; }
}

int db_append(const DbTable *table, const DbRow *row, int64_t *row_offset,
              char *error, size_t size) {
    /* Serialize before opening the file so allocation failures cannot append partial rows. */
    size_t length = 0;
    for (size_t i = 0; i < table->count; ++i) length += 4 + strlen(row->values[i]);
    unsigned char *buffer = malloc(length), *cursor = buffer;
    if (!buffer) return fail(error, size, "Out of memory");
    for (size_t i = 0; i < table->count; ++i) {
        uint32_t n = (uint32_t)strlen(row->values[i]);
        for (unsigned shift = 0; shift < 32; shift += 8) *cursor++ = (unsigned char)(n >> shift);
        memcpy(cursor, row->values[i], n);
        cursor += n;
    }
    FILE *file = fopen(table->data_path, "ab");
    if (!file) { free(buffer); return fail(error, size, "Cannot open table data for append"); }
    if (fseek(file, 0, SEEK_END)) {
        free(buffer); fclose(file); return fail(error, size, "Cannot locate table data");
    }
    long offset = ftell(file);
    if (offset < 0) { free(buffer); fclose(file); return fail(error, size, "Cannot locate table data"); }
    int failed = fwrite(buffer, 1, length, file) != length;
    free(buffer);
    if (fclose(file)) failed = 1;
    if (failed) {
        if (db_truncate(table, offset)) return fail(error, size, "Cannot roll back table data");
        return fail(error, size, "Cannot write table data");
    }
    if (row_offset) *row_offset = offset;
    return 0;
}

int db_read(FILE *file, const DbTable *table, DbRow *row, char *error, size_t size) {
    memset(row, 0, sizeof(*row));
    for (size_t i = 0; i < table->count; ++i) {
        unsigned char bytes[4];
        size_t got = fread(bytes, 1, 4, file);
        if (!got && !i && !ferror(file)) return 0;
        if (got != 4) goto corrupt;
        uint32_t length = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                          ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        if (length > DB_MAX_TEXT) goto corrupt;
        row->values[i] = malloc((size_t)length + 1);
        if (!row->values[i]) { db_row_free(row, table->count); return fail(error, size, "Out of memory"); }
        if (fread(row->values[i], 1, length, file) != length || memchr(row->values[i], 0, length)) goto corrupt;
        row->values[i][length] = 0;
        if (strpbrk(row->values[i], "\r\n")) goto corrupt;
        if (table->columns[i].type == DB_INTEGER) {
            char *end;
            errno = 0;
            strtoll(row->values[i], &end, 10);
            if (!length || *end || errno) goto corrupt;
        }
    }
    return 1;
corrupt:
    db_row_free(row, table->count);
    return fail(error, size, "Corrupt or unreadable table data");
}

int db_read_at(FILE *file, int64_t row_offset, const DbTable *table, DbRow *row,
               char *error, size_t size) {
    if (row_offset < 0 || row_offset > LONG_MAX || fseek(file, (long)row_offset, SEEK_SET))
        return fail(error, size, "Cannot seek table data");
    return db_read(file, table, row, error, size);
}

/* Only called under the engine write lock, to undo this statement's append. */
int db_truncate(const DbTable *table, int64_t offset) {
    if (offset < 0 || offset > LONG_MAX) return -1;
    return truncate(table->data_path, (off_t)offset);
}
