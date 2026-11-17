#include "pool.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static long allocation_budget=-1, thread_budget=-1;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size) {
    if (allocation_budget==0) return NULL;
    if (allocation_budget>0) allocation_budget--;
    return __real_calloc(n,size);
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*run)(void *),void *arg) {
    if (thread_budget==0) return EAGAIN;
    if (thread_budget>0) thread_budget--;
    return __real_pthread_create(thread,attr,run,arg);
}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static unsigned entered, id_mask;
static int released;
static void handler(int fd,unsigned id,void *context) {
    (void)fd; (void)context;
    pthread_mutex_lock(&mutex); entered++; id_mask|=1u<<id; pthread_cond_broadcast(&ready);
    while (!released) pthread_cond_wait(&ready,&mutex);
    pthread_mutex_unlock(&mutex);
}
static void *destroy_pool(void *p) { pool_destroy(p); return NULL; }
int main(void) {
    worker_pool *p=NULL;
    assert(pool_create(NULL,1,1,handler,NULL)==-1);
    assert(pool_create(&p,0,1,handler,NULL)==-1);
    for (long budget=0;budget<4;++budget) {
        allocation_budget=budget;
        assert(pool_create(&p,2,1,handler,NULL)==-1 && p==NULL);
    }
    allocation_budget=-1;
    puts("PASS pool allocation errors clean up without creating work");
    for (long budget=0;budget<2;++budget) {
        thread_budget=budget;
        assert(pool_create(&p,3,1,handler,NULL)==-1 && p==NULL);
    }
    thread_budget=-1;
    puts("PASS partially created worker pool joins started threads");
    assert(pool_create(&p,2,1,handler,NULL)==0);
    int a[2],b[2],c[2],d[2]; assert(pipe(a)==0 && pipe(b)==0 && pipe(c)==0 && pipe(d)==0);
    assert(pool_submit(p,a[0])==0);
    pthread_mutex_lock(&mutex); while (entered<1) pthread_cond_wait(&ready,&mutex); pthread_mutex_unlock(&mutex);
    assert(pool_submit(p,b[0])==0);
    pthread_mutex_lock(&mutex); while (entered<2) pthread_cond_wait(&ready,&mutex); pthread_mutex_unlock(&mutex);
    assert(pool_submit(p,c[0])==0 && pool_submit(p,d[0])==-1);
    assert(fcntl(d[0],F_GETFD)!=-1); close(d[0]);
    unsigned active; size_t queued; pool_snapshot(p,&active,&queued);
    assert(active==2 && queued==1);
    pthread_t stopper; assert(pthread_create(&stopper,NULL,destroy_pool,p)==0);
    for (unsigned i=0;i<1000 && fcntl(c[0],F_GETFD)!=-1;++i) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
    assert(fcntl(c[0],F_GETFD)==-1 && errno==EBADF);
    pthread_mutex_lock(&mutex); released=1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&mutex);
    pthread_join(stopper,NULL);
    assert(entered==2 && id_mask==6);
    assert(fcntl(a[0],F_GETFD)==-1 && fcntl(b[0],F_GETFD)==-1);
    close(a[1]);close(b[1]);close(c[1]);close(d[1]);pool_destroy(NULL);
    puts("PASS bounded queue, multiple workers, fd ownership and shutdown cleanup");
}
