/*
 * bst.h - Ordered index (int key -> void* data) built as an AVL tree.
 *
 * An AVL tree is a binary search tree that rebalances itself on insert: at
 * every node the heights of the two subtrees differ by at most 1, so the
 * height stays below ~1.44 log2(n) even when keys arrive in sorted order.
 *
 * All traversals are iterative (explicit stack, no recursion).
 */
#ifndef BST_H
#define BST_H

#include <stdint.h>

#define BST_SUCCESS      0
#define BST_DUP_KEY      1
#define BST_NULL         2
#define BST_ALLOC_FAILED 3

typedef struct BSTNode {
    int32_t key;
    int32_t height;         /* height of the subtree rooted here (leaf = 1) */
    void *data;
    struct BSTNode *left;
    struct BSTNode *right;
} BSTNode;

/* Visitor: return 0 to continue, non-zero to stop the traversal early. */
typedef int (*bst_visit_fn)(BSTNode *node, void *ctx);

/* Self-balancing (AVL) insert - what the engine uses. */
int      bst_add_node(BSTNode **root, int32_t key, void *data);

/* Plain, unbalanced BST insert. Benchmark baseline only: never mix it with
 * bst_add_node on the same tree (it does not maintain node heights). */
int      bst_add_node_unbalanced(BSTNode **root, int32_t key, void *data);

BSTNode *bst_search(BSTNode *root, int32_t key);
BSTNode *bst_max(BSTNode *root);

/* Traversals return 0 when complete, 1 when a visitor stopped them,
 * -1 when the traversal stack/queue could not be allocated. */
int bst_inorder(BSTNode *root, bst_visit_fn fn, void *ctx);
int bst_preorder(BSTNode *root, bst_visit_fn fn, void *ctx);
int bst_levelorder(BSTNode *root, bst_visit_fn fn, void *ctx);
int bst_range(BSTNode *root, int32_t lo, int32_t hi, bst_visit_fn fn, void *ctx);

void bst_free(BSTNode *root);            /* frees nodes only            */
void bst_free_with_data(BSTNode *root);  /* frees nodes and node->data  */

/* height = number of nodes on the longest root-to-leaf path (0 if empty) */
void bst_stats(BSTNode *root, long *count, int *height, double *avg_depth);

/* 1 if the tree is ordered, every stored height is correct and every node
 * is balanced (|left height - right height| <= 1). For tests. */
int bst_is_avl(BSTNode *root);

#endif
