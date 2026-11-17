#ifndef MINI_DBMS_BPTREE_H
#define MINI_DBMS_BPTREE_H
#include <stddef.h>
#include <stdint.h>

typedef struct bptree bptree;
typedef enum { BPT_OK, BPT_NOT_FOUND, BPT_DUPLICATE, BPT_NO_MEMORY, BPT_INVALID } bpt_status;
bpt_status bpt_create(bptree **out);
void bpt_destroy(bptree *tree);
/* Reserve nodes before writing a row to disk; does not change key/value entries. */
bpt_status bpt_prepare(bptree *tree);
bpt_status bpt_insert(bptree *tree, uint64_t key, uint64_t offset);
bpt_status bpt_find(const bptree *tree, uint64_t key, uint64_t *offset, size_t *visited);
size_t bpt_height(const bptree *tree);
#endif
