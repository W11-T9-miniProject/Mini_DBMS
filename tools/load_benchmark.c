#include "db.h"
#include "platform.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define remove_directory _rmdir
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define remove_directory rmdir
#endif

/* Closed-loop load: each worker issues its next request after its previous one.
 * Every case starts from an identical copy of the same seed file. */
typedef enum { LOAD_POINT, LOAD_MIXED, LOAD_SCAN, LOAD_SLOW } Workload;
typedef enum { REQUEST_POINT, REQUEST_INSERT, REQUEST_SCAN, REQUEST_SLOW } RequestKind;

typedef struct {
    PlatformMutex lock;
    size_t ready;
    bool start;
#ifdef _WIN32
    CONDITION_VARIABLE changed;
#else
    pthread_cond_t changed;
#endif
} StartGate;

typedef struct {
    Db *db;
    StartGate *gate;
    size_t number, threads, operations;
    uint64_t seed_rows;
    Workload workload;
    double *latency, *wait, *hold;
    uint64_t *inserted_ids;
    size_t errors;
    char first_error[DB_ERROR_SIZE];
} Worker;

static void fail(const char *message) { fprintf(stderr, "%s\n", message); exit(1); }

static void sleep_milliseconds(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec delay = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
#endif
}

static void gate_initialize(StartGate *gate) {
    memset(gate, 0, sizeof(*gate));
    platform_mutex_init(&gate->lock);
#ifdef _WIN32
    InitializeConditionVariable(&gate->changed);
#else
    pthread_cond_init(&gate->changed, NULL);
#endif
}

static void gate_wait(StartGate *gate) {
    platform_mutex_lock(&gate->lock);
    ++gate->ready;
    while (!gate->start) {
#ifdef _WIN32
        SleepConditionVariableCS(&gate->changed, &gate->lock, INFINITE);
#else
        pthread_cond_wait(&gate->changed, &gate->lock);
#endif
    }
    platform_mutex_unlock(&gate->lock);
}

static double gate_release(StartGate *gate, size_t threads) {
    for (;;) {
        platform_mutex_lock(&gate->lock);
        if (gate->ready == threads) break;
        platform_mutex_unlock(&gate->lock);
        sleep_milliseconds(1);
    }
    double start = platform_now();
    gate->start = true;
#ifdef _WIN32
    WakeAllConditionVariable(&gate->changed);
#else
    pthread_cond_broadcast(&gate->changed);
#endif
    platform_mutex_unlock(&gate->lock);
    return start;
}

static RequestKind request_kind(Workload workload, size_t request) {
    if (workload == LOAD_SCAN && request % 250 == 0) return REQUEST_SCAN;
    if (workload == LOAD_SLOW && request % 250 == 0) return REQUEST_SLOW;
    if (workload != LOAD_POINT && request % 10 == 9) return REQUEST_INSERT;
    return REQUEST_POINT;
}

typedef struct { uint64_t expected_id, count; bool valid, slow; } Match;

static bool verify_point(const DbRow *row, const DbResult *result, void *context) {
    Match *match = context;
    char expected_name[DB_NAME_SIZE];
    (void)result;
    snprintf(expected_name, sizeof(expected_name), "seed_%llu", (unsigned long long)match->expected_id);
    match->valid = match->valid && row->id == match->expected_id && strcmp(row->name, expected_name) == 0;
    ++match->count;
    if (match->slow) sleep_milliseconds(50);
    return true;
}

static void execute_worker_requests(Worker *worker) {
    gate_wait(worker->gate);
    for (size_t request = worker->number; request < worker->operations; request += worker->threads) {
        RequestKind kind = request_kind(worker->workload, request);
        uint64_t id = ((uint64_t)request * 2654435761u + 17u) % worker->seed_rows + 1;
        char sql[192];
        if (kind == REQUEST_INSERT)
            snprintf(sql, sizeof(sql), "INSERT INTO users(name,age) VALUES('job_%zu',%u)", request, (unsigned)(request % 100));
        else if (kind == REQUEST_SCAN)
            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE name='seed_%llu'", (unsigned long long)id);
        else snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id=%llu", (unsigned long long)id);
        Match match = {id, 0, true, kind == REQUEST_SLOW};
        DbError error;
        DbResult result;
        double start = platform_now();
        DbStatus status = db_execute(worker->db, sql, kind == REQUEST_INSERT ? NULL : verify_point,
                                    &match, &result, &error);
        worker->latency[request] = (platform_now() - start) * 1000.0;
        worker->wait[request] = result.lock_wait_ms;
        worker->hold[request] = result.lock_hold_ms;
        bool valid = status == DB_OK && result.affected_rows == 1;
        if (kind == REQUEST_INSERT) {
            worker->inserted_ids[request] = result.first_insert_id;
            valid = valid && result.first_insert_id > worker->seed_rows;
        } else valid = valid && match.valid && match.count == 1;
        if (!valid) {
            ++worker->errors;
            if (!worker->first_error[0]) snprintf(worker->first_error, sizeof(worker->first_error),
                "request=%zu status=%s: %.160s", request, db_status_name(status), error.message);
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID context) { execute_worker_requests(context); return 0; }
typedef HANDLE Thread;
#else
static void *worker_entry(void *context) { execute_worker_requests(context); return NULL; }
typedef pthread_t Thread;
#endif

static void copy_seed_file(const char *root, const char *destination) {
    char source_path[1024], destination_path[1024];
    snprintf(source_path, sizeof(source_path), "%s/seed/users.db", root);
    snprintf(destination_path, sizeof(destination_path), "%s/users.db", destination);
    FILE *source = fopen(source_path, "rb");
    FILE *target = fopen(destination_path, "wb");
    if (!source || !target) fail("Cannot copy seed file");
    unsigned char bytes[65536];
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), source)) != 0)
        if (fwrite(bytes, 1, count, target) != count) fail("Seed copy failed");
    if (ferror(source)) fail("Seed read failed");
    fclose(source);
    if (fclose(target) != 0) fail("Seed close failed");
}

static Db *open_checked(const char *directory, bool sync) {
    DbError error;
    Db *db = db_open(directory, sync, &error);
    if (!db) fail(error.message);
    return db;
}

static void create_seed_database(const char *root, uint64_t rows) {
    char directory[1024];
    snprintf(directory, sizeof(directory), "%s/seed", root);
    /* Refuse to overwrite any previous benchmark's seed. */
    char path[1100]; snprintf(path, sizeof(path), "%s/users.db", directory);
    FILE *existing = fopen(path, "rb");
    if (existing) { fclose(existing); fail("Use a new load benchmark directory"); }
    Db *db = open_checked(directory, true);
    char *sql = malloc(128 * 1024);
    if (!sql) fail("Cannot allocate seed SQL");
    for (uint64_t first = 1; first <= rows; first += 1000) {
        size_t used = (size_t)snprintf(sql, 128 * 1024, "INSERT INTO users(name,age) VALUES ");
        uint64_t last = rows - first < 999 ? rows : first + 999;
        for (uint64_t id = first; id <= last; ++id) {
            int count = snprintf(sql + used, 128 * 1024 - used, "%s('seed_%llu',%u)",
                                 id == first ? "" : ",", (unsigned long long)id, (unsigned)(id % 100));
            if (count < 0 || (size_t)count >= 128 * 1024 - used) fail("Seed buffer overflow");
            used += (size_t)count;
        }
        DbError error;
        if (db_execute(db, sql, NULL, NULL, NULL, &error) != DB_OK) fail(error.message);
    }
    free(sql);
    db_close(db);
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static double percentile(double *values, size_t count, unsigned percent) {
    qsort(values, count, sizeof(*values), compare_double);
    return values[(count * percent + 99) / 100 - 1];
}

static double category_p95(const double *latency, size_t operations, Workload workload, RequestKind kind) {
    double *samples = malloc(operations * sizeof(double));
    if (!samples) fail("Cannot allocate category samples");
    size_t count = 0;
    for (size_t i = 0; i < operations; ++i)
        if (request_kind(workload, i) == kind) samples[count++] = latency[i];
    double value = count ? percentile(samples, count, 95) : 0;
    free(samples);
    return value;
}

static void remove_case_files(const char *directory) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/users.db", directory);
    if (remove(path) != 0) fail("Cannot remove verified temporary case data");
    snprintf(path, sizeof(path), "%s/users.lock", directory);
    if (remove(path) != 0 || remove_directory(directory) != 0) fail("Cannot remove temporary case lock/directory");
}

static const char *workload_name(Workload workload) {
    static const char *names[] = {"point", "mixed90_10", "scan_mix", "slow_callback"};
    return names[workload];
}

typedef struct { uint64_t seeds; const uint64_t *ids; size_t operations, count; bool valid; } WriteCheck;

static bool verify_inserted_row(const DbRow *row, const DbResult *result, void *context) {
    WriteCheck *check = context;
    size_t request;
    (void)result;
    if (row->id <= check->seeds || sscanf(row->name, "job_%zu", &request) != 1 ||
        request >= check->operations || check->ids[request] != row->id || row->age != (int32_t)(request % 100))
        check->valid = false;
    ++check->count;
    return true;
}

static void run_case(const char *root, Workload workload, size_t threads, size_t operations,
                     uint64_t seed_rows, bool sync, unsigned repeat) {
    char directory[1024];
    snprintf(directory, sizeof(directory), "%s/%s-%zu-%u", root, workload_name(workload), threads, repeat);
    if (!platform_create_directory(directory)) fail("Cannot create case directory");
    copy_seed_file(root, directory);
    Db *db = open_checked(directory, sync);
    double *latency = calloc(operations, sizeof(double)), *wait = calloc(operations, sizeof(double));
    double *hold = calloc(operations, sizeof(double));
    uint64_t *ids = calloc(operations, sizeof(uint64_t));
    Worker *workers = calloc(threads, sizeof(*workers));
    Thread *handles = calloc(threads, sizeof(*handles));
    if (!latency || !wait || !hold || !ids || !workers || !handles) fail("Cannot allocate load samples");
    StartGate gate;
    gate_initialize(&gate);
    /* Warm point and full-scan paths without including them in measurements. */
    DbError error;
    if (db_execute(db, "SELECT * FROM users WHERE name='seed_1'", NULL, NULL, NULL, &error) != DB_OK) fail(error.message);
    for (size_t i = 0; i < threads; ++i) {
        workers[i] = (Worker){db, &gate, i, threads, operations, seed_rows, workload, latency, wait, hold, ids, 0, {0}};
#ifdef _WIN32
        handles[i] = CreateThread(NULL, 0, worker_entry, &workers[i], 0, NULL);
        if (!handles[i]) fail("Cannot create load worker");
#else
        if (pthread_create(&handles[i], NULL, worker_entry, &workers[i]) != 0) fail("Cannot create load worker");
#endif
    }
    double start = gate_release(&gate, threads);
    size_t errors = 0;
    for (size_t i = 0; i < threads; ++i) {
#ifdef _WIN32
        if (WaitForSingleObject(handles[i], INFINITE) != WAIT_OBJECT_0) fail("Cannot join worker");
        CloseHandle(handles[i]);
#else
        if (pthread_join(handles[i], NULL) != 0) fail("Cannot join worker");
#endif
        errors += workers[i].errors;
        if (workers[i].errors) fprintf(stderr, "%s\n", workers[i].first_error);
    }
    double elapsed = platform_now() - start;
    size_t writes = 0;
    for (size_t i = 0; i < operations; ++i)
        if (request_kind(workload, i) == REQUEST_INSERT) ++writes;
    bool *seen = calloc(writes + 1, sizeof(bool));
    if (!seen) fail("Cannot allocate ID validation");
    for (size_t i = 0; i < operations; ++i) {
        if (request_kind(workload, i) != REQUEST_INSERT) continue;
        if (ids[i] <= seed_rows || ids[i] > seed_rows + writes || seen[ids[i] - seed_rows]) fail("Missing or duplicate inserted ID");
        seen[ids[i] - seed_rows] = true;
    }
    db_close(db);
    db = open_checked(directory, sync); /* validates every checksum and consecutive ID */
    DbStats stats;
    if (db_get_stats(db, &stats, &error) != DB_OK || stats.row_count != seed_rows + writes) fail("Reopened row count mismatch");
    char sql[160]; snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id > %llu", (unsigned long long)seed_rows);
    WriteCheck check = {seed_rows, ids, operations, 0, true};
    if (db_execute(db, sql, verify_inserted_row, &check, NULL, &error) != DB_OK || !check.valid || check.count != writes)
        fail("Reopened INSERT content verification failed");
    db_close(db);
    double point_p95 = category_p95(latency, operations, workload, REQUEST_POINT);
    double write_p95 = category_p95(latency, operations, workload, REQUEST_INSERT);
    double scan_p95 = category_p95(latency, operations, workload, REQUEST_SCAN);
    double slow_p95 = category_p95(latency, operations, workload, REQUEST_SLOW);
    double maximum = percentile(latency, operations, 100);
    printf("%s,%zu,%u,%zu,%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f\n",
           workload_name(workload), threads, repeat, operations, (double)operations / elapsed,
           percentile(latency, operations, 50), percentile(latency, operations, 95), percentile(latency, operations, 99),
           percentile(wait, operations, 95), percentile(hold, operations, 95), writes, errors,
           point_p95, write_p95, scan_p95, slow_p95, maximum);
    fflush(stdout);
    fprintf(stderr, "Completed %s threads=%zu repeat=%u\n", workload_name(workload), threads, repeat);
    free(seen); free(latency); free(wait); free(hold); free(ids); free(workers); free(handles);
#ifndef _WIN32
    pthread_cond_destroy(&gate.changed);
#endif
    platform_mutex_destroy(&gate.lock);
    if (errors) fail("Load correctness errors");
    remove_case_files(directory);
}

static uint64_t positive_argument(const char *text, uint64_t maximum) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !text[0] || text[0] == '-' || *end || value == 0 || value > maximum) fail("Invalid positive argument");
    return (uint64_t)value;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 7) {
        fputs("Usage: db_load NEW_DIRECTORY [rows=100000] [requests=2000] [max_threads=16] [repeats=3] [sync=1]\n", stderr);
        return 2;
    }
    if (strlen(argv[1]) > 800) fail("Benchmark path too long");
    uint64_t rows = argc > 2 ? positive_argument(argv[2], 10000000) : 100000;
    size_t requests = argc > 3 ? (size_t)positive_argument(argv[3], 10000000) : 2000;
    size_t max_threads = argc > 4 ? (size_t)positive_argument(argv[4], 64) : 16;
    unsigned repeats = argc > 5 ? (unsigned)positive_argument(argv[5], 100) : 3;
    bool sync = argc <= 6 || strcmp(argv[6], "1") == 0;
    if (argc > 6 && strcmp(argv[6], "0") != 0 && strcmp(argv[6], "1") != 0) fail("sync must be 0 or 1");
    if (max_threads > requests) fail("requests must be >= max_threads");
    /* A new root is mandatory: never overwrite a prior case or user data. */
#ifdef _WIN32
    if (!CreateDirectoryA(argv[1], NULL)) fail("Use a new load directory whose parent exists");
#else
    if (mkdir(argv[1], 0755) != 0) fail("Use a new load directory whose parent exists");
#endif
    create_seed_database(argv[1], rows);
    printf("# rows=%llu requests=%zu repeats=%u sync=%d seed_formula=request*2654435761+17\n",
           (unsigned long long)rows, requests, repeats, sync ? 1 : 0);
    puts("# closed_loop=true warm_cache=true; scan_mix=one full scan per 250 requests; slow_callback=50ms per 250 requests");
    puts("workload,threads,repeat,requests,ops_per_second,p50_ms,p95_ms,p99_ms,lock_wait_p95_ms,lock_hold_p95_ms,writes,errors,point_p95_ms,write_p95_ms,scan_p95_ms,slow_p95_ms,max_ms");
    for (unsigned repeat = 1; repeat <= repeats; ++repeat)
        for (size_t threads = 1; threads <= max_threads; threads *= 2)
            for (int workload = LOAD_POINT; workload <= LOAD_SLOW; ++workload)
                run_case(argv[1], (Workload)workload, threads, requests, rows, sync, repeat);
    return 0;
}
