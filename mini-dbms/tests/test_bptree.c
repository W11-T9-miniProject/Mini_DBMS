#include "bptree.h"
#include <stdint.h>
#include <stdio.h>

static int require(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "%s\n", message);
    return 1;
}

int main(void) {
    Bptree *tree = bptree_create();
    if (require(tree != NULL, "create")) return 1;

    /* More than one leaf's capacity proves lookup crosses split nodes. */
    for (int64_t key = 0; key < 1000; ++key)
        if (require(bptree_insert(tree, key, key * 17) == 0, "insert")) return 1;

    for (int64_t key = 0; key < 1000; ++key) {
        int64_t value = -1;
        if (require(bptree_lookup(tree, key, &value) == 1 && value == key * 17,
                    "lookup after split")) return 1;
    }
    if (require(bptree_lookup(tree, 1001, NULL) == 0, "missing lookup")) return 1;
    if (require(bptree_insert(tree, 500, 0) == 1, "duplicate key")) return 1;
    bptree_destroy(tree);

    tree = bptree_create();
    if (require(tree != NULL, "failure-injection create")) return 1;
    for (int64_t key = 0; key < 31; ++key)
        if (require(bptree_insert(tree, key, key) == 0, "failure-injection seed")) return 1;
    /* Splitting this full root needs a new leaf and a new root. */
    bptree_test_fail_after(1);
    if (require(bptree_insert(tree, 31, 31) == -1, "split allocation failure")) return 1;
    bptree_test_fail_after(-1);
    for (int64_t key = 0; key < 31; ++key) {
        int64_t value = -1;
        if (require(bptree_lookup(tree, key, &value) == 1 && value == key,
                    "tree survives failed split")) return 1;
    }
    if (require(bptree_lookup(tree, 31, NULL) == 0, "failed insert is absent")) return 1;
    if (require(bptree_insert(tree, 31, 31) == 0, "retry after failed split")) return 1;
    bptree_destroy(tree);

    tree = bptree_create();
    if (require(tree != NULL, "cascade failure-injection create")) return 1;
    /* At key 527, a full right leaf splits under a full root. */
    for (int64_t key = 0; key < 527; ++key)
        if (require(bptree_insert(tree, key, key * 3) == 0, "cascade failure-injection seed")) return 1;
    /* Leaf and internal-right allocation succeed; allocating the new root fails. */
    bptree_test_fail_after(2);
    if (require(bptree_insert(tree, 527, 527 * 3) == -1, "cascade split allocation failure")) return 1;
    bptree_test_fail_after(-1);
    for (int64_t key = 0; key < 527; ++key) {
        int64_t value = -1;
        if (require(bptree_lookup(tree, key, &value) == 1 && value == key * 3,
                    "tree survives failed cascade split")) return 1;
    }
    if (require(bptree_lookup(tree, 527, NULL) == 0, "failed cascade insert is absent")) return 1;
    if (require(bptree_insert(tree, 527, 527 * 3) == 0, "retry after failed cascade split")) return 1;
    bptree_destroy(tree);

    tree = bptree_create();
    if (require(tree != NULL, "second create")) return 1;
    for (int64_t i = 0; i < 1000; ++i) {
        int64_t key = (i * 37) % 1000;
        if (require(bptree_insert(tree, key, key + 99) == 0, "unordered insert")) return 1;
    }
    for (int64_t key = 0; key < 1000; ++key) {
        int64_t value = -1;
        if (require(bptree_lookup(tree, key, &value) == 1 && value == key + 99,
                    "unordered lookup")) return 1;
    }
    bptree_destroy(tree);
    return 0;
}
