#ifndef MINI_DB_BPTREE_H
#define MINI_DB_BPTREE_H

#include <stdint.h>

/* A memory-resident B+ tree for unique signed 64-bit keys. */
typedef struct Bptree Bptree;

Bptree *bptree_create(void);
void bptree_destroy(Bptree *tree);

/* Returns 0 when inserted, 1 for an existing key, or -1 on allocation failure. */
int bptree_insert(Bptree *tree, int64_t key, int64_t value);

/* Returns 1 and writes value when found, 0 when absent. */
int bptree_lookup(const Bptree *tree, int64_t key, int64_t *value);

#ifdef BPTREE_TESTING
/* Test-only allocator fault injection for mutation atomicity checks. */
void bptree_test_fail_after(long successful_node_allocations);
#endif

#endif
