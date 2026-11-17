#ifndef MINI_DBMS_POOL_H
#define MINI_DBMS_POOL_H
#include <stddef.h>
typedef struct worker_pool worker_pool;
typedef void (*pool_handler)(int fd, unsigned worker_id, void *context);
int pool_create(worker_pool **out, unsigned workers, size_t capacity, pool_handler handler, void *context);
/* On success the pool owns fd; on failure ownership stays with caller. */
int pool_submit(worker_pool *pool, int fd);
void pool_snapshot(worker_pool *pool, unsigned *active, size_t *queued);
/* Stop accepting work, close queued sockets, finish active handlers and join. */
void pool_destroy(worker_pool *pool);
#endif
