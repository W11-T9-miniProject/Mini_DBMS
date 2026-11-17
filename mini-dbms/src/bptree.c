#include "bptree.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BPTREE_MAX_KEYS 31
#define BPTREE_MAX_HEIGHT 64

typedef struct BptreeNode {
    int leaf;
    size_t count;
    int64_t keys[BPTREE_MAX_KEYS];
    union {
        int64_t values[BPTREE_MAX_KEYS];
        struct BptreeNode *children[BPTREE_MAX_KEYS + 1];
    } entries;
    struct BptreeNode *next;
} BptreeNode;

struct Bptree { BptreeNode *root; };

typedef struct {
    int split;
    int64_t separator;
    BptreeNode *right;
} Split;

typedef struct {
    BptreeNode **nodes;
    size_t count, next;
} SplitPool;

#ifdef BPTREE_TESTING
static long fail_after = -1;

void bptree_test_fail_after(long successful_node_allocations) {
    fail_after = successful_node_allocations;
}
#endif

static BptreeNode *node_create(int leaf) {
#ifdef BPTREE_TESTING
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
#endif
    BptreeNode *node = calloc(1, sizeof(*node));
    if (node) node->leaf = leaf;
    return node;
}

static void node_destroy(BptreeNode *node) {
    if (!node) return;
    if (!node->leaf)
        for (size_t i = 0; i <= node->count; ++i) node_destroy(node->entries.children[i]);
    free(node);
}

Bptree *bptree_create(void) {
    Bptree *tree = calloc(1, sizeof(*tree));
    if (!tree) return NULL;
    tree->root = node_create(1);
    if (!tree->root) { free(tree); return NULL; }
    return tree;
}

void bptree_destroy(Bptree *tree) {
    if (!tree) return;
    node_destroy(tree->root);
    free(tree);
}

static size_t lower_bound(const BptreeNode *node, int64_t key) {
    size_t i = 0;
    while (i < node->count && node->keys[i] < key) ++i;
    return i;
}

static size_t child_index(const BptreeNode *node, int64_t key) {
    size_t index = lower_bound(node, key);
    while (index < node->count && key >= node->keys[index]) ++index;
    return index;
}

static void split_pool_free(SplitPool *pool) {
    for (size_t i = pool->next; i < pool->count; ++i) free(pool->nodes[i]);
    free(pool->nodes);
    memset(pool, 0, sizeof(*pool));
}

/* Allocate every node that a split cascade can require before mutating the tree. */
static int prepare_insert(const Bptree *tree, int64_t key, SplitPool *pool) {
    BptreeNode *path[BPTREE_MAX_HEIGHT];
    size_t count = 0;
    BptreeNode *node = tree->root;
    for (;;) {
        if (count == BPTREE_MAX_HEIGHT) return -1;
        path[count++] = node;
        if (node->leaf) break;
        node = node->entries.children[child_index(node, key)];
    }
    size_t leaf_index = lower_bound(node, key);
    if (leaf_index < node->count && node->keys[leaf_index] == key) return 1;

    int carries_split = node->count == BPTREE_MAX_KEYS;
    if (!carries_split) return 0;
    pool->nodes = calloc(count + 1, sizeof(*pool->nodes));
    if (!pool->nodes) return -1;
    if (carries_split) {
        pool->nodes[pool->count] = node_create(1);
        if (!pool->nodes[pool->count++]) goto out_of_memory;
    }
    for (size_t i = count - 1; i-- > 0 && carries_split;) {
        if (path[i]->count == BPTREE_MAX_KEYS) {
            pool->nodes[pool->count] = node_create(0);
            if (!pool->nodes[pool->count++]) goto out_of_memory;
        } else carries_split = 0;
    }
    if (carries_split) {
        pool->nodes[pool->count] = node_create(0);
        if (!pool->nodes[pool->count++]) goto out_of_memory;
    }
    return 0;
out_of_memory:
    pool->next = 0;
    split_pool_free(pool);
    return -1;
}

static BptreeNode *take_split_node(SplitPool *pool) {
    return pool->nodes[pool->next++];
}

static int insert(BptreeNode *node, int64_t key, int64_t value, Split *split, SplitPool *pool) {
    size_t index = lower_bound(node, key);
    split->split = 0;
    split->right = NULL;
    if (node->leaf) {
        if (index < node->count && node->keys[index] == key) return 1;
        if (node->count < BPTREE_MAX_KEYS) {
            for (size_t i = node->count; i > index; --i) {
                node->keys[i] = node->keys[i - 1];
                node->entries.values[i] = node->entries.values[i - 1];
            }
            node->keys[index] = key;
            node->entries.values[index] = value;
            ++node->count;
            return 0;
        }
        BptreeNode *right = take_split_node(pool);
        int64_t keys[BPTREE_MAX_KEYS + 1], values[BPTREE_MAX_KEYS + 1];
        for (size_t i = 0, source = 0; i < BPTREE_MAX_KEYS + 1; ++i) {
            if (i == index) { keys[i] = key; values[i] = value; }
            else { keys[i] = node->keys[source]; values[i] = node->entries.values[source++]; }
        }
        size_t left_count = (BPTREE_MAX_KEYS + 1) / 2;
        node->count = left_count;
        for (size_t i = 0; i < left_count; ++i) {
            node->keys[i] = keys[i]; node->entries.values[i] = values[i];
        }
        right->count = BPTREE_MAX_KEYS + 1 - left_count;
        for (size_t i = 0; i < right->count; ++i) {
            right->keys[i] = keys[left_count + i];
            right->entries.values[i] = values[left_count + i];
        }
        right->next = node->next;
        node->next = right;
        split->split = 1;
        split->separator = right->keys[0];
        split->right = right;
        return 0;
    }

    /* Equal separator keys belong to the child on their right. */
    index = child_index(node, key);
    Split child_split;
    int result = insert(node->entries.children[index], key, value, &child_split, pool);
    if (result || !child_split.split) return result;
    if (node->count < BPTREE_MAX_KEYS) {
        for (size_t i = node->count; i > index; --i) node->keys[i] = node->keys[i - 1];
        for (size_t i = node->count + 1; i > index + 1; --i)
            node->entries.children[i] = node->entries.children[i - 1];
        node->keys[index] = child_split.separator;
        node->entries.children[index + 1] = child_split.right;
        ++node->count;
        return 0;
    }
    BptreeNode *right = take_split_node(pool);
    int64_t keys[BPTREE_MAX_KEYS + 1];
    BptreeNode *children[BPTREE_MAX_KEYS + 2];
    for (size_t i = 0, source = 0; i < BPTREE_MAX_KEYS + 1; ++i) {
        if (i == index) keys[i] = child_split.separator;
        else keys[i] = node->keys[source++];
    }
    for (size_t i = 0, source = 0; i < BPTREE_MAX_KEYS + 2; ++i) {
        if (i == index + 1) children[i] = child_split.right;
        else children[i] = node->entries.children[source++];
    }
    size_t middle = (BPTREE_MAX_KEYS + 1) / 2;
    node->count = middle;
    for (size_t i = 0; i < middle; ++i) node->keys[i] = keys[i];
    for (size_t i = 0; i <= middle; ++i) node->entries.children[i] = children[i];
    right->count = BPTREE_MAX_KEYS - middle;
    for (size_t i = 0; i < right->count; ++i) right->keys[i] = keys[middle + 1 + i];
    for (size_t i = 0; i <= right->count; ++i)
        right->entries.children[i] = children[middle + 1 + i];
    split->split = 1;
    split->separator = keys[middle];
    split->right = right;
    return 0;
}

int bptree_insert(Bptree *tree, int64_t key, int64_t value) {
    if (!tree) return -1;
    SplitPool pool = {0};
    int prepared = prepare_insert(tree, key, &pool);
    if (prepared) return prepared;
    Split split;
    int result = insert(tree->root, key, value, &split, &pool);
    if (result || !split.split) { split_pool_free(&pool); return result; }
    BptreeNode *root = take_split_node(&pool);
    root->count = 1;
    root->keys[0] = split.separator;
    root->entries.children[0] = tree->root;
    root->entries.children[1] = split.right;
    tree->root = root;
    split_pool_free(&pool);
    return 0;
}

int bptree_lookup(const Bptree *tree, int64_t key, int64_t *value) {
    if (!tree || !tree->root) return 0;
    BptreeNode *node = tree->root;
    while (!node->leaf) {
        node = node->entries.children[child_index(node, key)];
    }
    size_t index = lower_bound(node, key);
    if (index == node->count || node->keys[index] != key) return 0;
    if (value) *value = node->entries.values[index];
    return 1;
}
