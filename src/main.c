#include "engine.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SQL_FILE_LIMIT (256u * 1024u * 1024u)

static char *read_source(const char *path, size_t *length) {
    FILE *input = fopen(path, "rb");
    if (!input) { perror("Cannot open SQL file"); return NULL; }
    struct stat info;
    if (fstat(fileno(input), &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size > SQL_FILE_LIMIT) {
        fputs("SQL source must be a regular file of at most 256 MiB\n", stderr);
        fclose(input); return NULL;
    }
    size_t size = (size_t)info.st_size;
    char *text = malloc(size + 1);
    if (!text) { fputs("Cannot allocate SQL input buffer\n", stderr); fclose(input); return NULL; }
    size_t got = fread(text, 1, size, input);
    int failed = ferror(input);
    if (fclose(input) != 0) failed = 1;
    if (failed || got != size) { fputs("Cannot read complete SQL file\n", stderr); free(text); return NULL; }
    text[size] = 0; *length = size;
    return text;
}
static void json_string(const char *text) {
    putchar('"');
    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') putchar('\\');
        putchar(c);
    }
    putchar('"');
}
static int print_row(const play_record *record, void *context) {
    uint64_t statement = *(uint64_t *)context;
    printf("{\"type\":\"row\",\"statement\":%" PRIu64 ",\"record\":{\"id\":%" PRIu64 ",\"event_code\":", statement, record->id);
    json_string(record->event_code);
    printf(",\"user_id\":%" PRIu64 ",\"song_id\":%" PRIu64 ",\"genre\":", record->user_id, record->song_id);
    json_string(record->genre);
    puts("}}");
    return ferror(stdout) ? -1 : 0;
}

int main(int argc, char **argv) {
    const char *database = "data/play_history_v2.db", *source = NULL;
    int custom_path = 0;
    if (argc == 2) source = argv[1];
    else if (argc == 4 && strcmp(argv[1], "--db") == 0) {
        database = argv[2]; source = argv[3]; custom_path = 1;
    } else {
        fputs("Usage: mini-dbms [--db <database-file>] <sql-file>\n", stderr); return 2;
    }
    size_t length = 0;
    char *text = read_source(source, &length);
    if (!text) return 1;
    if (!custom_path && mkdir("data", 0700) < 0 && errno != EEXIST) {
        perror("Cannot create data directory"); free(text); return 1;
    }
    db_engine *engine = NULL;
    db_status status = engine_open(&engine, database);
    if (status != DB_OK) {
        fprintf(stderr, "Cannot open database: %s\n", db_status_text(status)); free(text); return 1;
    }
    sql_parser parser; sql_error error; sql_statement statement;
    sql_parser_init(&parser, text, length);
    uint64_t statements = 0, inserted = 0, selected = 0;
    int exit_code = 0;
    for (;;) {
        sql_status parsed = sql_next(&parser, &statement, &error);
        if (parsed == SQL_DONE) break;
        if (parsed == SQL_ERROR) {
            fprintf(stderr, "SQL error at line %zu, column %zu: %s\n", error.line, error.column, error.message);
            exit_code = 1; break;
        }
        statements++;
        db_result result;
        status = engine_execute(engine, &statement, print_row, &statements, &result);
        if (status != DB_OK) {
            fprintf(stderr, "Execution error in statement %" PRIu64 " at line %zu, column %zu: %s\n",
                    statements, statement.line, statement.column, db_status_text(status));
            exit_code = 1; break;
        }
        if (statement.kind == SQL_INSERT) inserted += result.affected;
        else {
            selected += result.affected;
            printf("{\"type\":\"select\",\"statement\":%" PRIu64 ",\"matched\":%" PRIu64 ",\"scanned\":%" PRIu64
                   ",\"access_path\":\"%s\",\"index_nodes\":%" PRIu64 "}\n",
                   statements, result.affected, result.scanned,
                   result.used_index ? "bplus_tree" : "full_scan", result.index_nodes);
        }
        if (ferror(stdout)) { fputs("Cannot write query output\n", stderr); exit_code = 1; break; }
    }
    status = engine_close(engine);
    free(text);
    if (status != DB_OK) {
        fprintf(stderr, "Database close/sync failed: %s\n", db_status_text(status)); return 1;
    }
    if (exit_code) return exit_code;
    printf("{\"type\":\"summary\",\"status\":\"ok\",\"statements\":%" PRIu64 ",\"inserted\":%" PRIu64 ",\"selected\":%" PRIu64 "}\n",
           statements, inserted, selected);
    return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
