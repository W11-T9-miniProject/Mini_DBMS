#include "engine.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static db_engine *engine;
static sql_statement insert, select_all, select_id;
static pthread_barrier_t readers;
static atomic_int writer_started, writer_done;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int entered, released;
static sql_statement parse(const char *text) {
    sql_parser p; sql_statement s; sql_error e;
    sql_parser_init(&p, text, __builtin_strlen(text));
    assert(sql_next(&p, &s, &e) == SQL_OK); return s;
}
static int overlap(const play_record *r, void *unused) {
    (void)r; (void)unused;
    int result = pthread_barrier_wait(&readers);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    return 0;
}
static void *read_parallel(void *unused) {
    (void)unused; db_result r;
    assert(engine_execute(engine, &select_all, overlap, NULL, &r) == DB_OK && r.affected == 1);
    return NULL;
}
static int hold_read(const play_record *record, void *unused) {
    (void)record; (void)unused;
    pthread_mutex_lock(&gate); entered = 1; pthread_cond_broadcast(&ready);
    while (!released) pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate); return 0;
}
static void *held_reader(void *unused) {
    (void)unused; db_result r;
    assert(engine_execute(engine, &select_all, hold_read, NULL, &r) == DB_OK && r.affected == 1);
    return NULL;
}
static void *one_writer(void *unused) {
    (void)unused; db_result r; atomic_store(&writer_started, 1);
    assert(engine_execute_sync(engine, &insert, NULL, NULL, &r) == DB_OK && r.inserted.id == 2);
    atomic_store(&writer_done, 1); return NULL;
}
static void *mixed_worker(void *arg) {
    uintptr_t number = (uintptr_t)arg;
    for (unsigned i = 0; i < 40; ++i) {
        db_result r;
        if (number % 2) assert(engine_execute(engine, &insert, NULL, NULL, &r) == DB_OK);
        else {
            assert(engine_execute(engine, &select_all, NULL, NULL, &r) == DB_OK);
            assert(r.scanned == r.affected && r.affected >= 2);
        }
    }
    return NULL;
}
int main(void) {
    char directory[] = "/tmp/mini-dbms-concurrent-XXXXXX", path[256];
    assert(mkdtemp(directory)); snprintf(path, sizeof(path), "%s/history.db", directory);
    assert(engine_open(&engine, path) == DB_OK);
    insert = parse("INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P',71,108,'rock');");
    select_all = parse("SELECT * FROM play_history;");
    select_id = parse("SELECT * FROM play_history WHERE id=1;");
    db_result r; assert(engine_execute(engine, &insert, NULL, NULL, &r) == DB_OK);
    assert(pthread_barrier_init(&readers, NULL, 2) == 0);
    pthread_t a,b;
    assert(pthread_create(&a,NULL,read_parallel,NULL) == 0 && pthread_create(&b,NULL,read_parallel,NULL) == 0);
    pthread_join(a,NULL); pthread_join(b,NULL); pthread_barrier_destroy(&readers);
    puts("PASS concurrent SELECT callbacks overlap");
    assert(pthread_create(&a,NULL,held_reader,NULL) == 0);
    pthread_mutex_lock(&gate); while (!entered) pthread_cond_wait(&ready,&gate); pthread_mutex_unlock(&gate);
    assert(pthread_create(&b,NULL,one_writer,NULL) == 0);
    while (!atomic_load(&writer_started)) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
    struct timespec delay={0,30000000}; nanosleep(&delay,NULL);
    assert(!atomic_load(&writer_done));
    pthread_mutex_lock(&gate); released=1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&gate);
    pthread_join(a,NULL); pthread_join(b,NULL); assert(atomic_load(&writer_done));
    puts("PASS writer waits for reader snapshot and syncs before return");
    pthread_t workers[12];
    for (uintptr_t i=0;i<12;++i) assert(pthread_create(&workers[i],NULL,mixed_worker,(void *)i) == 0);
    for (unsigned i=0;i<12;++i) pthread_join(workers[i],NULL);
    assert(engine_close(engine) == DB_OK && engine_open(&engine,path) == DB_OK);
    assert(engine_execute(engine,&select_all,NULL,NULL,&r) == DB_OK && r.affected == 242);
    for (uint64_t id=1;id<=242;++id) {
        select_id.value.number=id;
        assert(engine_execute(engine,&select_id,NULL,NULL,&r) == DB_OK && r.affected == 1 && r.scanned == 1);
    }
    assert(engine_close(engine) == DB_OK); unlink(path); rmdir(directory);
    puts("PASS concurrent writes/reads, unique contiguous IDs and rebuilt index");
}
