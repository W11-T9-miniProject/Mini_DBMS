#include "bptree.h"
#include <stdlib.h>
#include <string.h>

/* One extra key/child slot permits a temporary overflow before splitting. */
#define BPT_MAX_KEYS 31
#define BPT_OVERFLOW_KEYS (BPT_MAX_KEYS + 1)

struct BptNode {
    bool leaf;
    size_t count;
    uint64_t keys[BPT_OVERFLOW_KEYS];
    uint64_t values[BPT_OVERFLOW_KEYS];
    struct BptNode *children[BPT_OVERFLOW_KEYS + 1];
    struct BptNode *next;
};

typedef struct { bool occurred; uint64_t separator; BptNode *right; } Split;

static size_t find_key_position(const BptNode *node, uint64_t key, bool upper) {
    size_t left = 0, right = node->count;
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        if (node->keys[middle] < key || (upper && node->keys[middle] == key))
            left = middle + 1;
        else right = middle;
    }
    return left;
}

static BptNode *allocate_node(bool leaf) {
    BptNode *node = calloc(1, sizeof(*node));
    if (node) node->leaf = leaf;
    return node;
}

bool bptree_init(Bptree *tree) {
    memset(tree, 0, sizeof(*tree));
    tree->root = allocate_node(true);
    if (!tree->root) return false;
    tree->height = 1;
    tree->node_count = 1;
    return true;
}

static void destroy_node(BptNode *node) {
    if (!node) return;
    if (!node->leaf)
        for (size_t i = 0; i <= node->count; ++i) destroy_node(node->children[i]);
    free(node);
}

void bptree_destroy(Bptree *tree) {
    destroy_node(tree->root);
    while (tree->spares) {
        BptNode *next = tree->spares->next;
        free(tree->spares);
        tree->spares = next;
    }
    memset(tree, 0, sizeof(*tree));
}

bool bptree_find(const Bptree *tree, uint64_t key, uint64_t *value) {
    const BptNode *node = tree->root;
    if (!node) return false;
    while (!node->leaf) node = node->children[find_key_position(node, key, true)];
    size_t position = find_key_position(node, key, false);
    if (position == node->count || node->keys[position] != key) return false;
    if (value) *value = node->values[position];
    return true;
}

/* Reserve enough nodes for a split at every level plus a new root.
 * Reusable spare nodes avoid allocating on every insertion. */
static bool reserve_split_nodes(Bptree *tree) {
    while (tree->spare_count < tree->height + 1) {
        BptNode *node = allocate_node(false);
        if (!node) return false;
        node->next = tree->spares;
        tree->spares = node;
        ++tree->spare_count;
    }
    return true;
}

static BptNode *take_split_node(Bptree *tree, bool leaf) {
    BptNode *node = tree->spares;
    tree->spares = node->next;
    --tree->spare_count;
    ++tree->node_count;
    memset(node, 0, sizeof(*node));
    node->leaf = leaf;
    return node;
}

static Split split_leaf(Bptree *tree, BptNode *left) {
    BptNode *right = take_split_node(tree, true);
    size_t middle = left->count / 2;
    right->count = left->count - middle;
    memcpy(right->keys, left->keys + middle, right->count * sizeof(uint64_t));
    memcpy(right->values, left->values + middle, right->count * sizeof(uint64_t));
    left->count = middle;
    right->next = left->next;
    left->next = right;
    return (Split){true, right->keys[0], right};
}

static Split split_internal(Bptree *tree, BptNode *left) {
    BptNode *right = take_split_node(tree, false);
    size_t middle = left->count / 2;
    uint64_t separator = left->keys[middle];
    right->count = left->count - middle - 1;
    memcpy(right->keys, left->keys + middle + 1, right->count * sizeof(uint64_t));
    memcpy(right->children, left->children + middle + 1,
           (right->count + 1) * sizeof(BptNode *));
    left->count = middle;
    return (Split){true, separator, right};
}

static Split insert_into_node(Bptree *tree, BptNode *node, uint64_t key, uint64_t value) {
    size_t position = find_key_position(node, key, !node->leaf);
    if (node->leaf) {
        memmove(node->keys + position + 1, node->keys + position,
                (node->count - position) * sizeof(uint64_t));
        memmove(node->values + position + 1, node->values + position,
                (node->count - position) * sizeof(uint64_t));
        node->keys[position] = key;
        node->values[position] = value;
    } else {
        Split child = insert_into_node(tree, node->children[position], key, value);
        if (!child.occurred) return (Split){0};
        memmove(node->keys + position + 1, node->keys + position,
                (node->count - position) * sizeof(uint64_t));
        memmove(node->children + position + 2, node->children + position + 1,
                (node->count - position) * sizeof(BptNode *));
        node->keys[position] = child.separator;
        node->children[position + 1] = child.right;
    }
    ++node->count;
    if (node->count <= BPT_MAX_KEYS) return (Split){0};
    return node->leaf ? split_leaf(tree, node) : split_internal(tree, node);
}

bool bptree_insert(Bptree *tree, uint64_t key, uint64_t value) {
    if (!tree->root || bptree_find(tree, key, NULL) || !reserve_split_nodes(tree)) return false;
    Split split = insert_into_node(tree, tree->root, key, value);
    if (split.occurred) {
        BptNode *root = take_split_node(tree, false);
        root->keys[0] = split.separator;
        root->children[0] = tree->root;
        root->children[1] = split.right;
        root->count = 1;
        tree->root = root;
        ++tree->height;
    }
    ++tree->size;
    return true;
}

BptCursor bptree_lower_bound(const Bptree *tree, uint64_t key) {
    const BptNode *node = tree->root;
    if (!node) return (BptCursor){0};
    while (!node->leaf) node = node->children[find_key_position(node, key, true)];
    return (BptCursor){node, find_key_position(node, key, false)};
}

bool bptree_cursor_next(BptCursor *cursor, uint64_t *key, uint64_t *value) {
    while (cursor->leaf && cursor->position >= cursor->leaf->count) {
        cursor->leaf = cursor->leaf->next;
        cursor->position = 0;
    }
    if (!cursor->leaf) return false;
    *key = cursor->leaf->keys[cursor->position];
    *value = cursor->leaf->values[cursor->position++];
    return true;
}

typedef struct {
    bool valid;
    uint64_t minimum, maximum, entries;
    size_t nodes;
    const BptNode *last_leaf;
} Validation;

static Validation validate_node(const BptNode *node, size_t depth, size_t height, bool root) {
    Validation result = {true, 0, 0, 0, 1, NULL};
    if (!node || node->count > BPT_MAX_KEYS ||
        (!root && node->count < (node->leaf ? 16u : 15u)) ||
        (root && !node->leaf && node->count == 0)) {
        result.valid = false;
        return result;
    }
    for (size_t i = 1; i < node->count; ++i)
        if (node->keys[i - 1] >= node->keys[i]) result.valid = false;
    if (node->leaf) {
        result.valid = result.valid && depth == height;
        result.entries = node->count;
        if (node->count) {
            result.minimum = node->keys[0];
            result.maximum = node->keys[node->count - 1];
        }
        result.last_leaf = node;
        return result;
    }
    if (depth >= height) { result.valid = false; return result; }
    for (size_t i = 0; i <= node->count; ++i) {
        Validation child = validate_node(node->children[i], depth + 1, height, false);
        if (!child.valid || !child.entries) { result.valid = false; return result; }
        if (i == 0) result.minimum = child.minimum;
        else if (result.maximum >= child.minimum || node->keys[i - 1] != child.minimum ||
                 result.last_leaf->next != node->children[i] /* checked below for internal child */) {
            /* Internal children start at a subtree, not directly at a leaf. */
            const BptNode *first = node->children[i];
            while (!first->leaf) first = first->children[0];
            if (result.maximum >= child.minimum || node->keys[i - 1] != child.minimum ||
                result.last_leaf->next != first) result.valid = false;
        }
        result.maximum = child.maximum;
        result.entries += child.entries;
        result.nodes += child.nodes;
        result.last_leaf = child.last_leaf;
    }
    return result;
}

bool bptree_validate(const Bptree *tree) {
    if (!tree->root) return false;
    Validation result = validate_node(tree->root, 1, tree->height, true);
    return result.valid && result.entries == tree->size &&
           result.nodes == tree->node_count && !result.last_leaf->next;
}

size_t bptree_memory_bytes(const Bptree *tree) {
    return sizeof(*tree) + (tree->node_count + tree->spare_count) * sizeof(BptNode);
}
