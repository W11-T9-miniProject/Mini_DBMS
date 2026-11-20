#ifndef MINI_BPTREE_H
#define MINI_BPTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BptNode BptNode;
typedef struct {
    BptNode *root;
    BptNode *spares;
    size_t spare_count;
    size_t height;
    size_t node_count;
    uint64_t size;
} Bptree;

typedef struct { const BptNode *leaf; size_t position; } BptCursor;

bool bptree_init(Bptree *tree);
void bptree_destroy(Bptree *tree);
/* Unique keys only. Allocation failure leaves the existing tree unchanged. */
bool bptree_insert(Bptree *tree, uint64_t key, uint64_t value);
bool bptree_find(const Bptree *tree, uint64_t key, uint64_t *value);
BptCursor bptree_lower_bound(const Bptree *tree, uint64_t key);
bool bptree_cursor_next(BptCursor *cursor, uint64_t *key, uint64_t *value);
bool bptree_validate(const Bptree *tree);
size_t bptree_memory_bytes(const Bptree *tree);

#endif
