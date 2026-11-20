#include "bptree.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static long allocation_budget = -1;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size) {
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) allocation_budget--;
    return __real_calloc(n, size);
}
static void test_empty_edges(void) {
    bptree *t = NULL; uint64_t offset = 99; size_t visits = 99;
    assert(bpt_create(NULL) == BPT_INVALID);
    assert(bpt_create(&t) == BPT_OK);
    assert(bpt_find(t, 1, &offset, &visits) == BPT_NOT_FOUND && visits == 0 && offset == 99);
    assert(bpt_insert(t, 0, UINT64_MAX) == BPT_OK);
    assert(bpt_insert(t, UINT64_MAX, 0) == BPT_OK);
    assert(bpt_find(t, 0, &offset, &visits) == BPT_OK && offset == UINT64_MAX);
    assert(bpt_find(t, UINT64_MAX, &offset, &visits) == BPT_OK && offset == 0);
    assert(bpt_insert(t, 0, 123) == BPT_DUPLICATE);
    assert(bpt_find(t, 0, &offset, NULL) == BPT_OK && offset == UINT64_MAX);
    bpt_destroy(t); bpt_destroy(NULL);
}
static void test_splits(unsigned mode) {
    enum { N = 20000 }; uint64_t keys[N];
    for (size_t i = 0; i < N; ++i) keys[i] = i * 2 + 1;
    if (mode == 1) {
        for (size_t i = 0; i < N / 2; ++i) { uint64_t k = keys[i]; keys[i] = keys[N-1-i]; keys[N-1-i] = k; }
    } else if (mode == 2) {
        uint32_t random = 71;
        for (size_t i = N - 1; i; --i) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            size_t j = random % (i + 1); uint64_t k = keys[i]; keys[i] = keys[j]; keys[j] = k;
        }
    }
    bptree *t = NULL; assert(bpt_create(&t) == BPT_OK);
    for (size_t i = 0; i < N; ++i) assert(bpt_insert(t, keys[i], keys[i] * 92) == BPT_OK);
    assert(bpt_height(t) >= 3);
    for (size_t i = 0; i < N; ++i) {
        uint64_t offset; size_t visited;
        assert(bpt_find(t, keys[i], &offset, &visited) == BPT_OK && offset == keys[i] * 92);
        assert(visited == bpt_height(t));
        assert(bpt_find(t, keys[i] - 1, &offset, &visited) == BPT_NOT_FOUND);
    }
    bpt_destroy(t);
}
static void test_allocation_failure(void) {
    bptree *t = NULL;
    allocation_budget = 0; assert(bpt_create(&t) == BPT_NO_MEMORY && !t);
    allocation_budget = -1; assert(bpt_create(&t) == BPT_OK);
    for (uint64_t i = 0; i < 10000; ++i) {
        allocation_budget = 0;
        bpt_status status = bpt_insert(t, i, i + 32);
        if (status == BPT_NO_MEMORY) {
            uint64_t offset;
            assert(bpt_find(t, i, &offset, NULL) == BPT_NOT_FOUND);
            if (i) assert(bpt_find(t, i-1, &offset, NULL) == BPT_OK && offset == i+31);
            allocation_budget = -1; assert(bpt_prepare(t) == BPT_OK);
            allocation_budget = 0; assert(bpt_insert(t, i, i+32) == BPT_OK);
        } else assert(status == BPT_OK);
    }
    allocation_budget = -1;
    for (uint64_t i = 0; i < 10000; ++i) {
        uint64_t offset; assert(bpt_find(t, i, &offset, NULL) == BPT_OK && offset == i+32);
    }
    bpt_destroy(t);
}
int main(void) {
    test_empty_edges(); puts("PASS B+ empty, zero/max keys and duplicate rejection");
    test_splits(0); test_splits(1); test_splits(2); puts("PASS B+ ascending/descending/shuffled multi-level splits");
    test_allocation_failure(); puts("PASS B+ allocation failure and reserved insertion");
}
