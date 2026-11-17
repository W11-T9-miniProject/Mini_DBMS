#include "engine.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAMPLES 101
typedef struct { uint64_t id, count; play_record record; } capture;
typedef struct { uint64_t id, indexed_ns, linear_ns, nodes; } sample;
static void die(const char *message) { fprintf(stderr, "%s\n", message); exit(1); }
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock failure");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void check(db_status status) { if (status != DB_OK) die(db_status_text(status)); }
static sql_statement parse(const char *text) {
    sql_parser parser; sql_statement statement; sql_error error;
    sql_parser_init(&parser, text, strlen(text));
    if (sql_next(&parser, &statement, &error) != SQL_OK) die(error.message);
    return statement;
}
static int capture_row(const play_record *record, void *context) {
    capture *c = context;
    c->count++; c->record = *record;
    return record->id == c->id ? 0 : -1;
}
static uint64_t query(db_engine *engine, sql_statement *statement, capture *c, db_result *result) {
    c->count = 0;
    uint64_t start = now_ns();
    db_status status = engine_execute(engine, statement, capture_row, c, result);
    uint64_t duration = now_ns() - start;
    check(status);
    if (c->count != 1 || result->affected != 1) die("query did not return exactly the expected row");
    return duration;
}
static int compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) die("Usage: benchmark <generated.sql> <new-db-file> [odd-samples:1..101]");
    char *end = NULL; errno = 0;
    unsigned long parsed_samples = argc == 4 ? strtoul(argv[3], &end, 10) : 9;
    if (errno || (argc == 4 && (!argv[3][0] || *end)) || !parsed_samples || parsed_samples > MAX_SAMPLES || parsed_samples % 2 == 0)
        die("samples must be an odd integer between 1 and 101");
    unsigned samples = (unsigned)parsed_samples;
    struct stat info;
    /* Never append a benchmark dataset to an existing user database. */
    if (lstat(argv[2], &info) == 0 || errno != ENOENT) die("benchmark database path must not exist");
    FILE *source = fopen(argv[1], "rb");
    if (!source) die("cannot open SQL input");
    if (fstat(fileno(source), &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (uint64_t)info.st_size > UINT64_C(256)*1024*1024) die("invalid SQL size (limit 256 MiB)");
    size_t length = (size_t)info.st_size;
    char *text = malloc(length+1);
    if (!text || fread(text, 1, length, source) != length) die("cannot read SQL input");
    if (fclose(source)) die("cannot close SQL input");
    text[length] = 0;
    db_engine *engine = NULL;
    check(engine_create_new(&engine, argv[2]));
    sql_parser parser; sql_statement statement; sql_error error; db_result result;
    sql_parser_init(&parser, text, length);
    uint64_t count = 0, start = now_ns();
    for (;;) {
        sql_status status = sql_next(&parser, &statement, &error);
        if (status == SQL_DONE) break;
        if (status != SQL_OK) die(error.message);
        if (statement.kind != SQL_INSERT) die("benchmark input must contain INSERTs only");
        check(engine_execute(engine, &statement, NULL, NULL, &result)); count++;
    }
    if (!count) die("empty dataset");
    check(engine_close(engine));
    uint64_t load_ns = now_ns() - start;
    free(text);
    start = now_ns(); check(engine_open(&engine, argv[2]));
    uint64_t rebuild_ns = now_ns() - start;
    sample measurements[MAX_SAMPLES]; uint64_t indexed[MAX_SAMPLES], linear[MAX_SAMPLES];
    /* Two warmups run both paths before collecting paired measurements. */
    for (unsigned i = 0; i < samples + 2; ++i) {
        uint64_t id = 1 + ((count-1) * (i+1) / (samples+3));
        char id_sql[128], code_sql[160];
        snprintf(id_sql, sizeof(id_sql), "SELECT * FROM play_history WHERE id=%" PRIu64 ";", id);
        snprintf(code_sql, sizeof(code_sql), "SELECT * FROM play_history WHERE event_code='PLAY%09" PRIu64 "';", id);
        sql_statement by_id = parse(id_sql), by_code = parse(code_sql);
        capture a = {.id=id}, b = {.id=id}; db_result indexed_result, linear_result;
        uint64_t a_ns, b_ns;
        if (i % 2) {
            b_ns = query(engine, &by_code, &b, &linear_result);
            a_ns = query(engine, &by_id, &a, &indexed_result);
        } else {
            a_ns = query(engine, &by_id, &a, &indexed_result);
            b_ns = query(engine, &by_code, &b, &linear_result);
        }
        if (!indexed_result.used_index || indexed_result.scanned != 1 || linear_result.used_index || linear_result.scanned != count ||
            a.record.id != b.record.id || a.record.user_id != b.record.user_id || a.record.song_id != b.record.song_id ||
            strcmp(a.record.event_code, b.record.event_code) || strcmp(a.record.genre, b.record.genre)) die("lookup path/result mismatch");
        if (i >= 2) {
            unsigned j = i-2;
            indexed[j] = a_ns; linear[j] = b_ns;
            measurements[j] = (sample){id, a_ns, b_ns, indexed_result.index_nodes};
        }
    }
    check(engine_close(engine));
    if (stat(argv[2], &info)) die("cannot stat database");
    qsort(indexed, samples, sizeof(uint64_t), compare); qsort(linear, samples, sizeof(uint64_t), compare);
    printf("{\"records\":%" PRIu64 ",\"file_bytes\":%" PRIu64 ",\"samples\":%u,\"load_ns\":%" PRIu64
           ",\"rebuild_ns\":%" PRIu64 ",\"indexed_median_ns\":%" PRIu64 ",\"linear_median_ns\":%" PRIu64
           ",\"indexed_min_ns\":%" PRIu64 ",\"indexed_max_ns\":%" PRIu64 ",\"linear_min_ns\":%" PRIu64 ",\"linear_max_ns\":%" PRIu64 ",\"queries\":[",
           count, (uint64_t)info.st_size, samples, load_ns, rebuild_ns, indexed[samples/2], linear[samples/2],
           indexed[0], indexed[samples-1], linear[0], linear[samples-1]);
    for (unsigned i = 0; i < samples; ++i) {
        sample s = measurements[i];
        printf("%s{\"id\":%" PRIu64 ",\"indexed_ns\":%" PRIu64 ",\"linear_ns\":%" PRIu64
               ",\"indexed_scanned\":1,\"linear_scanned\":%" PRIu64 ",\"index_nodes\":%" PRIu64 ",\"matched\":1}",
               i ? "," : "", s.id, s.indexed_ns, s.linear_ns, count, s.nodes);
    }
    puts("]}");
    return fflush(stdout) ? 1 : 0;
}
