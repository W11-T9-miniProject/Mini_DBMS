#include "pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct { worker_pool *pool; unsigned id; } worker_arg;
struct worker_pool {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t *threads;
    worker_arg *args;
    unsigned workers, active;
    int *fds, stopping;
    size_t capacity, head, count;
    pool_handler handler;
    void *context;
};
static void *worker(void *arg) {
    worker_arg *a = arg; worker_pool *p = a->pool;
    for (;;) {
        pthread_mutex_lock(&p->mutex);
        while (!p->count && !p->stopping) pthread_cond_wait(&p->ready, &p->mutex);
        if (p->stopping) { pthread_mutex_unlock(&p->mutex); return NULL; }
        int fd = p->fds[p->head];
        p->head = (p->head + 1) % p->capacity; p->count--; p->active++;
        pthread_mutex_unlock(&p->mutex);
        p->handler(fd, a->id, p->context); close(fd);
        pthread_mutex_lock(&p->mutex); p->active--; pthread_mutex_unlock(&p->mutex);
    }
}
void pool_destroy(worker_pool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mutex); p->stopping = 1;
    while (p->count) {
        close(p->fds[p->head]); p->head = (p->head + 1) % p->capacity; p->count--;
    }
    pthread_cond_broadcast(&p->ready); pthread_mutex_unlock(&p->mutex);
    for (unsigned i = 0; i < p->workers; ++i) pthread_join(p->threads[i], NULL);
    pthread_cond_destroy(&p->ready); pthread_mutex_destroy(&p->mutex);
    free(p->fds); free(p->args); free(p->threads); free(p);
}
int pool_create(worker_pool **out, unsigned workers, size_t capacity, pool_handler handler, void *context) {
    if (!out || !workers || workers > 32 || !capacity || capacity > 1024 || !handler) return -1;
    *out = NULL;
    worker_pool *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->threads = calloc(workers, sizeof(pthread_t)); p->args = calloc(workers, sizeof(worker_arg));
    p->fds = calloc(capacity, sizeof(int));
    if (!p->threads || !p->args || !p->fds) { free(p->threads); free(p->args); free(p->fds); free(p); return -1; }
    if (pthread_mutex_init(&p->mutex, NULL)) { free(p->threads); free(p->args); free(p->fds); free(p); return -1; }
    if (pthread_cond_init(&p->ready, NULL)) {
        pthread_mutex_destroy(&p->mutex); free(p->threads); free(p->args); free(p->fds); free(p); return -1;
    }
    p->capacity = capacity; p->handler = handler; p->context = context;
    for (unsigned i = 0; i < workers; ++i) {
        p->args[i] = (worker_arg){p, i+1};
        if (pthread_create(&p->threads[i], NULL, worker, &p->args[i])) { pool_destroy(p); return -1; }
        p->workers++;
    }
    *out = p; return 0;
}
int pool_submit(worker_pool *p, int fd) {
    if (!p || fd < 0) return -1;
    pthread_mutex_lock(&p->mutex);
    if (p->stopping || p->count == p->capacity) { pthread_mutex_unlock(&p->mutex); return -1; }
    p->fds[(p->head + p->count) % p->capacity] = fd; p->count++;
    pthread_cond_signal(&p->ready); pthread_mutex_unlock(&p->mutex); return 0;
}
void pool_snapshot(worker_pool *p, unsigned *active, size_t *queued) {
    pthread_mutex_lock(&p->mutex); *active = p->active; *queued = p->count; pthread_mutex_unlock(&p->mutex);
}
