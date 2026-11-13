/*
 * bst.c - AVL tree (self-balancing BST), iterative.
 */
#include <stdlib.h>

#include "bst.h"

/* An AVL tree of n nodes has height < 1.4405 log2(n + 2), so 64 levels is
 * far more than any int32-keyed tree can ever reach. */
#define AVL_MAX_HEIGHT 64

/* ---- Explicit traversal stack ------------------------------------------ */

typedef struct {
    BSTNode *node;
    int depth;
} Frame;

typedef struct {
    Frame *items;
    size_t count;
    size_t cap;
} Stack;

static int push(Stack *s, BSTNode *node, int depth)
{
    if (s->count == s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 64;
        Frame *grown = realloc(s->items, new_cap * sizeof *grown);
        if (!grown) return -1;
        s->items = grown;
        s->cap = new_cap;
    }
    s->items[s->count].node = node;
    s->items[s->count].depth = depth;
    s->count++;
    return 0;
}

static Frame pop(Stack *s)
{
    return s->items[--s->count];
}

/* ---- AVL balancing ------------------------------------------------------ */

static int32_t height_of(const BSTNode *n)
{
    return n ? n->height : 0;
}

static void update_height(BSTNode *n)
{
    int32_t l = height_of(n->left), r = height_of(n->right);
    n->height = (l > r ? l : r) + 1;
}

/*      y            x
 *     / \          / \
 *    x   C   ->   A   y
 *   / \              / \
 *  A   B            B   C      */
static BSTNode *rotate_right(BSTNode *y)
{
    BSTNode *x = y->left;
    y->left = x->right;
    x->right = y;
    update_height(y);
    update_height(x);
    return x;
}

static BSTNode *rotate_left(BSTNode *x)
{
    BSTNode *y = x->right;
    x->right = y->left;
    y->left = x;
    update_height(x);
    update_height(y);
    return y;
}

/* Restores the AVL property at n (whose subtrees are already balanced) and
 * returns the new root of this subtree. Covers the four classic cases:
 * left-left, left-right, right-right, right-left. */
static BSTNode *rebalance(BSTNode *n)
{
    int32_t balance;

    update_height(n);
    balance = height_of(n->left) - height_of(n->right);
    if (balance > 1) {
        if (height_of(n->left->left) < height_of(n->left->right))
            n->left = rotate_left(n->left);            /* left-right -> left-left */
        return rotate_right(n);
    }
    if (balance < -1) {
        if (height_of(n->right->right) < height_of(n->right->left))
            n->right = rotate_right(n->right);         /* right-left -> right-right */
        return rotate_left(n);
    }
    return n;
}

/* ---- Insert / search ---------------------------------------------------- */

static BSTNode *new_node(int32_t key, void *data)
{
    BSTNode *node = malloc(sizeof *node);
    if (!node) return NULL;
    node->key = key;
    node->height = 1;
    node->data = data;
    node->left = node->right = NULL;
    return node;
}

int bst_add_node(BSTNode **root, int32_t key, void *data)
{
    BSTNode **path[AVL_MAX_HEIGHT];   /* links walked from the root down */
    BSTNode **link;
    int depth = 0;

    if (!root) return BST_NULL;

    link = root;
    while (*link) {
        if (key == (*link)->key) return BST_DUP_KEY;
        if (depth == AVL_MAX_HEIGHT) return BST_NULL;   /* not an AVL tree */
        path[depth++] = link;
        link = key < (*link)->key ? &(*link)->left : &(*link)->right;
    }

    *link = new_node(key, data);
    if (!*link) return BST_ALLOC_FAILED;

    /* Walk back up, fixing heights and rotating where a node became
     * unbalanced. Once a subtree's height is unchanged, nothing above it
     * can change either, so stop early. */
    while (depth > 0) {
        BSTNode **up = path[--depth];
        int32_t before = (*up)->height;
        *up = rebalance(*up);
        if ((*up)->height == before) break;
    }
    return BST_SUCCESS;
}

int bst_add_node_unbalanced(BSTNode **root, int32_t key, void *data)
{
    BSTNode **link;

    if (!root) return BST_NULL;
    link = root;
    while (*link) {
        if (key == (*link)->key) return BST_DUP_KEY;
        link = key < (*link)->key ? &(*link)->left : &(*link)->right;
    }
    *link = new_node(key, data);
    return *link ? BST_SUCCESS : BST_ALLOC_FAILED;
}

BSTNode *bst_search(BSTNode *root, int32_t key)
{
    while (root) {
        if (key == root->key) return root;
        root = key < root->key ? root->left : root->right;
    }
    return NULL;
}

BSTNode *bst_max(BSTNode *root)
{
    if (!root) return NULL;
    while (root->right) root = root->right;
    return root;
}

/* ---- Traversals --------------------------------------------------------- */

int bst_inorder(BSTNode *root, bst_visit_fn fn, void *ctx)
{
    Stack s = {0};
    BSTNode *cur = root;
    int rc = 0;

    while (cur || s.count) {
        while (cur) {
            if (push(&s, cur, 0)) { rc = -1; goto done; }
            cur = cur->left;
        }
        cur = pop(&s).node;
        if (fn(cur, ctx)) { rc = 1; goto done; }
        cur = cur->right;
    }
done:
    free(s.items);
    return rc;
}

int bst_preorder(BSTNode *root, bst_visit_fn fn, void *ctx)
{
    Stack s = {0};
    int rc = 0;

    if (root && push(&s, root, 0)) return -1;
    while (s.count) {
        BSTNode *n = pop(&s).node;
        if (fn(n, ctx)) { rc = 1; break; }
        /* push right first so the left subtree is visited first */
        if ((n->right && push(&s, n->right, 0)) || (n->left && push(&s, n->left, 0))) {
            rc = -1;
            break;
        }
    }
    free(s.items);
    return rc;
}

/* Breadth-first, top level first. The engine saves .ndx in this order:
 * re-inserting an AVL tree's nodes level by level never triggers a
 * rotation (every partially rebuilt tree is itself balanced), so the next
 * open rebuilds a tree of exactly the same shape. */
int bst_levelorder(BSTNode *root, bst_visit_fn fn, void *ctx)
{
    Stack q = {0};          /* used as a FIFO queue: items[head..count) */
    size_t head = 0;
    int rc = 0;

    if (root && push(&q, root, 0)) return -1;
    while (head < q.count) {
        BSTNode *n = q.items[head++].node;
        if (fn(n, ctx)) { rc = 1; break; }
        if ((n->left && push(&q, n->left, 0)) || (n->right && push(&q, n->right, 0))) {
            rc = -1;
            break;
        }
    }
    free(q.items);
    return rc;
}

/* In-order walk restricted to [lo, hi]. Subtrees that lie entirely outside
 * the range are never entered, so the cost is O(h + k) for k results. */
int bst_range(BSTNode *root, int32_t lo, int32_t hi, bst_visit_fn fn, void *ctx)
{
    Stack s = {0};
    BSTNode *cur = root;
    int rc = 0;

    while (cur || s.count) {
        while (cur) {
            if (cur->key < lo) {
                cur = cur->right;          /* whole left subtree is < lo */
            } else {
                if (push(&s, cur, 0)) { rc = -1; goto done; }
                cur = cur->left;
            }
        }
        if (!s.count) break;
        cur = pop(&s).node;
        if (cur->key > hi) break;          /* everything after is > hi too */
        if (fn(cur, ctx)) { rc = 1; goto done; }
        cur = cur->right;
    }
done:
    free(s.items);
    return rc;
}

/* ---- Freeing ------------------------------------------------------------ */

static void free_tree(BSTNode *root, int with_data)
{
    Stack s = {0};

    if (!root) return;
    if (push(&s, root, 0)) {
        /* Out of memory for the stack: fall back to freeing via rotation,
         * which needs no extra memory. */
        while (root) {
            if (root->left) {
                BSTNode *l = root->left;
                root->left = l->right;
                l->right = root;
                root = l;
            } else {
                BSTNode *next = root->right;
                if (with_data) free(root->data);
                free(root);
                root = next;
            }
        }
        return;
    }
    while (s.count) {
        BSTNode *n = pop(&s).node;
        if (n->left)  push(&s, n->left, 0);
        if (n->right) push(&s, n->right, 0);
        if (with_data) free(n->data);
        free(n);
    }
    free(s.items);
}

void bst_free(BSTNode *root)           { free_tree(root, 0); }
void bst_free_with_data(BSTNode *root) { free_tree(root, 1); }

/* ---- Shape statistics --------------------------------------------------- */

void bst_stats(BSTNode *root, long *count, int *height, double *avg_depth)
{
    Stack s = {0};
    long n = 0;
    int h = 0;
    double depth_sum = 0.0;

    if (root && push(&s, root, 1) == 0) {
        while (s.count) {
            Frame f = pop(&s);
            n++;
            depth_sum += f.depth;
            if (f.depth > h) h = f.depth;
            if ((f.node->left && push(&s, f.node->left, f.depth + 1)) ||
                (f.node->right && push(&s, f.node->right, f.depth + 1))) {
                break;
            }
        }
    }
    free(s.items);
    if (count) *count = n;
    if (height) *height = h;
    if (avg_depth) *avg_depth = n ? depth_sum / n : 0.0;
}

/* ---- Invariant check (tests) -------------------------------------------- */

/* Returns the subtree height, or -1 if any AVL invariant fails. Recursion
 * depth is bounded: a subtree deeper than AVL_MAX_HEIGHT fails at once. */
static int check_avl(const BSTNode *n, int64_t lo, int64_t hi, int depth)
{
    int l, r;
    if (!n) return 0;
    if (depth > AVL_MAX_HEIGHT || n->key <= lo || n->key >= hi) return -1;
    l = check_avl(n->left, lo, n->key, depth + 1);
    r = check_avl(n->right, n->key, hi, depth + 1);
    if (l < 0 || r < 0 || l - r > 1 || r - l > 1) return -1;
    if (n->height != (l > r ? l : r) + 1) return -1;
    return n->height;
}

int bst_is_avl(BSTNode *root)
{
    return check_avl(root, (int64_t)INT32_MIN - 1, (int64_t)INT32_MAX + 1, 1) >= 0;
}
