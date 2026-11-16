#include "db.h"
#include "platform.h"
#include "storage.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BATCH_SIZE 1000u
#define SQL_CAPACITY (256u * 1024u)

static void fail(const char *message) { fprintf(stderr, "%s\n", message); exit(1); }

static uint64_t parse_positive(const char *value, uint64_t maximum) {
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || !value[0] || value[0] == '-' || *end || parsed == 0 || parsed > maximum)
        fail("Invalid benchmark argument");
    return (uint64_t)parsed;
}

static DbResult execute_checked(Db *db, const char *sql) {
    DbResult result;
    DbError error;
    if (db_execute(db, sql, NULL, NULL, &result, &error) != DB_OK) fail(error.message);
    return result;
}

static void insert_generated_rows(Db *db, uint64_t total) {
    char *sql = malloc(SQL_CAPACITY);
    if (!sql) fail("Cannot allocate SQL batch");
    for (uint64_t start = 1; start <= total; start += BATCH_SIZE) {
        size_t used = (size_t)snprintf(sql, SQL_CAPACITY, "INSERT INTO users(name,age,email) VALUES ");
        size_t batch = total - start + 1 < BATCH_SIZE ? (size_t)(total - start + 1) : BATCH_SIZE;
        for (size_t i = 0; i < batch; ++i) {
            unsigned long long id = (unsigned long long)(start + i);
            int written = snprintf(sql + used, SQL_CAPACITY - used, "%s('user_%llu',%u,'user_%llu@example.com')",
                                   i ? "," : "", id, (unsigned)((start + i) % 100), id);
            if (written < 0 || (size_t)written >= SQL_CAPACITY - used) fail("SQL batch buffer overflow");
            used += (size_t)written;
        }
        DbResult result = execute_checked(db, sql);
        if (result.affected_rows != batch || result.first_insert_id != start) fail("Generated INSERT verification failed");
        if ((start - 1) % 100000 == 0) fprintf(stderr, "Inserted %llu / %llu\n", (unsigned long long)(start + batch - 1), (unsigned long long)total);
    }
    free(sql);
}

typedef struct { uint64_t id; uint64_t count; } Match;
static bool capture_match(const DbRow *row, const DbResult *result, void *context) {
    Match *match = context;
    (void)result;
    match->id = row->id;
    ++match->count;
    return true;
}

static double measure_query(Db *db, const char *sql, bool force_scan, uint64_t expected_id, uint64_t expected_examined) {
    Match match = {0};
    DbResult result;
    DbError error;
    double start = platform_now();
    DbStatus status = force_scan
        ? db_execute_scan(db, sql, capture_match, &match, &result, &error)
        : db_execute(db, sql, capture_match, &match, &result, &error);
    double elapsed = (platform_now() - start) * 1000.0;
    if (status != DB_OK) fail(error.message);
    if (match.count != 1 || match.id != expected_id || result.affected_rows != 1 || result.examined_rows != expected_examined)
        fail("Query correctness or examined-row verification failed");
    return elapsed;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static void report_latencies(const char *label, double *samples, size_t count) {
    qsort(samples, count, sizeof(*samples), compare_double);
    double median = count % 2 ? samples[count / 2] : (samples[count / 2 - 1] + samples[count / 2]) / 2;
    size_t p95 = (count * 95 + 99) / 100 - 1;
    printf("%s,%.6f,%.6f\n", label, median, samples[p95]);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fputs("Usage: db_bench EMPTY_DIRECTORY [records=1000000] [queries=30]\n", stderr);
        return 2;
    }
    uint64_t total = argc >= 3 ? parse_positive(argv[2], 100000000) : 1000000;
    size_t queries = argc >= 4 ? (size_t)parse_positive(argv[3], 10000) : 30;
    DbError error;
    Db *db = db_open(argv[1], true, &error);
    if (!db) fail(error.message);
    if (execute_checked(db, "SELECT * FROM users LIMIT 1").affected_rows != 0) {
        db_close(db);
        fail("Benchmark requires an empty database directory; existing data was not changed");
    }
    double start = platform_now();
    insert_generated_rows(db, total);
    double insert_seconds = platform_now() - start;
    db_close(db);
    start = platform_now();
    db = db_open(argv[1], true, &error);
    if (!db) fail(error.message);
    double rebuild_seconds = platform_now() - start;
    DbStats stats;
    if (db_get_stats(db, &stats, &error) != DB_OK) fail(error.message);
    DbResult count_result = execute_checked(db, "SELECT * FROM users");
    if (count_result.affected_rows != total) fail("Reopened row count mismatch");
    double *indexed = calloc(queries, sizeof(double));
    double *forced = calloc(queries, sizeof(double));
    double *names = calloc(queries, sizeof(double));
    if (!indexed || !forced || !names) fail("Cannot allocate timing samples");
    uint32_t seed = 20261116u;
    /* Warm page cache and each execution path before collecting timings. */
    for (size_t iteration = 0; iteration < queries + 3; ++iteration) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        uint64_t id = (uint64_t)seed % total + 1;
        char id_sql[128], name_sql[160];
        snprintf(id_sql, sizeof(id_sql), "SELECT * FROM users WHERE id=%llu", (unsigned long long)id);
        snprintf(name_sql, sizeof(name_sql), "SELECT * FROM users WHERE name='user_%llu'", (unsigned long long)id);
        double a, b, c;
        /* Alternate measurement order to reduce systematic cache/order bias. */
        if (iteration % 2) {
            c = measure_query(db, name_sql, false, id, total);
            b = measure_query(db, id_sql, true, id, total);
            a = measure_query(db, id_sql, false, id, 1);
        } else {
            a = measure_query(db, id_sql, false, id, 1);
            b = measure_query(db, id_sql, true, id, total);
            c = measure_query(db, name_sql, false, id, total);
        }
        if (iteration >= 3) { indexed[iteration - 3] = a; forced[iteration - 3] = b; names[iteration - 3] = c; }
    }
    printf("records=%llu\nqueries=%zu\nseed=20261116\nbatch_size=%u\nsync_writes=true\n",
           (unsigned long long)total, queries, BATCH_SIZE);
    printf("insert_seconds=%.6f\nrebuild_seconds=%.6f\nfile_bytes=%llu\n",
           insert_seconds, rebuild_seconds, (unsigned long long)storage_row_offset(total));
    printf("index_height=%zu\nindex_nodes=%zu\nindex_memory_bytes=%zu\n",
           stats.index_height, stats.index_nodes, stats.index_memory_bytes);
    puts("cache=warm (OS cache is not dropped)\nquery,median_ms,p95_ms");
    report_latencies("id_index", indexed, queries);
    report_latencies("same_id_forced_scan", forced, queries);
    report_latencies("unique_name_scan", names, queries);
    puts("correctness=PASS (every query returned exactly the expected ID; examined counts verified)");
    free(indexed); free(forced); free(names);
    db_close(db);
    return 0;
}
