#include "bptree.h"
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 31
typedef struct node {
    unsigned count;
    int leaf;
    uint64_t keys[MAX_KEYS + 1];
    union { uint64_t offsets[MAX_KEYS + 1]; struct node *children[MAX_KEYS + 2]; } data;
    struct node *next;
} node;
struct bptree { node *root, *spares; size_t height, spare_count; };

bpt_status bpt_create(bptree **out) {
    if (!out) return BPT_INVALID;
    *out = calloc(1, sizeof(bptree));
    return *out ? BPT_OK : BPT_NO_MEMORY;
}
static void free_node(node *n) {
    if (!n) return;
    if (!n->leaf) for (unsigned i = 0; i <= n->count; ++i) free_node(n->data.children[i]);
    free(n);
}
void bpt_destroy(bptree *tree) {
    if (!tree) return;
    free_node(tree->root);
    while (tree->spares) { node *n = tree->spares; tree->spares = n->next; free(n); }
    free(tree);
}
bpt_status bpt_prepare(bptree *tree) {
    if (!tree) return BPT_INVALID;
    /* One right sibling per level plus a possible new root; no allocation during mutation. */
    while (tree->spare_count < tree->height + 2) {
        node *n = calloc(1, sizeof(node));
        if (!n) return BPT_NO_MEMORY;
        n->next = tree->spares; tree->spares = n; tree->spare_count++;
    }
    return BPT_OK;
}
static node *take_node(bptree *tree, int leaf) {
    node *n = tree->spares;
    tree->spares = n->next; tree->spare_count--;
    memset(n, 0, sizeof(*n)); n->leaf = leaf;
    return n;
}
/* Upper bound: equality follows the right-hand child of an internal separator. */
static unsigned child_index(const node *n, uint64_t key) {
    unsigned low = 0, high = n->count;
    while (low < high) {
        unsigned mid = low + (high-low)/2;
        if (key >= n->keys[mid]) low = mid + 1; else high = mid;
    }
    return low;
}
bpt_status bpt_find(const bptree *tree, uint64_t key, uint64_t *offset, size_t *visited) {
    if (visited) *visited = 0;
    if (!tree || !offset) return BPT_INVALID;
    const node *n = tree->root;
    while (n) {
        if (visited) (*visited)++;
        if (!n->leaf) { n = n->data.children[child_index(n, key)]; continue; }
        unsigned low = 0, high = n->count;
        while (low < high) {
            unsigned mid = low + (high-low)/2;
            if (n->keys[mid] < key) low = mid + 1; else high = mid;
        }
        if (low == n->count || n->keys[low] != key) return BPT_NOT_FOUND;
        *offset = n->data.offsets[low]; return BPT_OK;
    }
    return BPT_NOT_FOUND;
}
/* Returns a new right sibling and the separator to insert in the parent, or NULL. */
static node *insert_node(bptree *tree, node *n, uint64_t key, uint64_t offset, uint64_t *separator) {
    unsigned i = 0;
    if (n->leaf) {
        while (i < n->count && n->keys[i] < key) i++;
        for (unsigned j = n->count; j > i; --j) {
            n->keys[j] = n->keys[j-1]; n->data.offsets[j] = n->data.offsets[j-1];
        }
        n->keys[i] = key; n->data.offsets[i] = offset; n->count++;
    } else {
        i = child_index(n, key);
        uint64_t promoted;
        node *right = insert_node(tree, n->data.children[i], key, offset, &promoted);
        if (!right) return NULL;
        for (unsigned j = n->count; j > i; --j) {
            n->keys[j] = n->keys[j-1]; n->data.children[j+1] = n->data.children[j];
        }
        n->keys[i] = promoted; n->data.children[i+1] = right; n->count++;
    }
    if (n->count <= MAX_KEYS) return NULL;
    node *right = take_node(tree, n->leaf);
    unsigned middle = n->count / 2;
    if (n->leaf) {
        right->count = n->count - middle;
        memcpy(right->keys, n->keys + middle, right->count * sizeof(uint64_t));
        memcpy(right->data.offsets, n->data.offsets + middle, right->count * sizeof(uint64_t));
        right->next = n->next; n->next = right;
        *separator = right->keys[0];
    } else {
        *separator = n->keys[middle];
        right->count = n->count - middle - 1;
        memcpy(right->keys, n->keys + middle + 1, right->count * sizeof(uint64_t));
        memcpy(right->data.children, n->data.children + middle + 1, (right->count + 1) * sizeof(node *));
    }
    n->count = middle;
    return right;
}
bpt_status bpt_insert(bptree *tree, uint64_t key, uint64_t offset) {
    if (!tree) return BPT_INVALID;
    uint64_t old;
    if (bpt_find(tree, key, &old, NULL) == BPT_OK) return BPT_DUPLICATE;
    bpt_status status = bpt_prepare(tree);
    if (status != BPT_OK) return status;
    if (!tree->root) { tree->root = take_node(tree, 1); tree->height = 1; }
    uint64_t separator;
    node *right = insert_node(tree, tree->root, key, offset, &separator);
    if (right) {
        node *root = take_node(tree, 0);
        root->count = 1; root->keys[0] = separator;
        root->data.children[0] = tree->root; root->data.children[1] = right;
        tree->root = root; tree->height++;
    }
    return BPT_OK;
}
size_t bpt_height(const bptree *tree) { return tree ? tree->height : 0; }
