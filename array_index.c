/*
 * array_index.c - Sorted array index with binary search.
 */
#include <stdlib.h>
#include <string.h>

#include "array_index.h"

ArrayIndex *ai_create(size_t initial_cap)
{
    ArrayIndex *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    if (initial_cap < 16) initial_cap = 16;
    a->items = malloc(initial_cap * sizeof *a->items);
    if (!a->items) {
        free(a);
        return NULL;
    }
    a->cap = initial_cap;
    return a;
}

void ai_free(ArrayIndex *a)
{
    if (!a) return;
    free(a->items);
    free(a);
}

static int grow(ArrayIndex *a)
{
    size_t new_cap;
    AIEntry *bigger;
    if (a->count < a->cap) return AI_SUCCESS;
    new_cap = a->cap * 2;
    bigger = realloc(a->items, new_cap * sizeof *bigger);
    if (!bigger) return AI_ALLOC_FAILED;
    a->items = bigger;
    a->cap = new_cap;
    return AI_SUCCESS;
}

/* Index of the first entry whose key is >= key (count if none). */
static size_t lower_bound(const ArrayIndex *a, int32_t key)
{
    size_t lo = 0, hi = a->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->items[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int ai_insert(ArrayIndex *a, int32_t key, void *value)
{
    size_t pos;
    if (!a) return AI_INVALID;
    pos = lower_bound(a, key);
    if (pos < a->count && a->items[pos].key == key) return AI_DUP_KEY;
    if (grow(a) != AI_SUCCESS) return AI_ALLOC_FAILED;
    /* shift everything after pos one slot to the right: the O(n) part */
    memmove(&a->items[pos + 1], &a->items[pos], (a->count - pos) * sizeof *a->items);
    a->items[pos].key = key;
    a->items[pos].value = value;
    a->count++;
    return AI_SUCCESS;
}

int ai_append(ArrayIndex *a, int32_t key, void *value)
{
    if (!a) return AI_INVALID;
    if (a->count && key <= a->items[a->count - 1].key) return AI_INVALID;
    if (grow(a) != AI_SUCCESS) return AI_ALLOC_FAILED;
    a->items[a->count].key = key;
    a->items[a->count].value = value;
    a->count++;
    return AI_SUCCESS;
}

int ai_remove(ArrayIndex *a, int32_t key)
{
    size_t pos;
    if (!a) return AI_INVALID;
    pos = lower_bound(a, key);
    if (pos >= a->count || a->items[pos].key != key) return AI_NOT_FOUND;
    memmove(&a->items[pos], &a->items[pos + 1], (a->count - pos - 1) * sizeof *a->items);
    a->count--;
    return AI_SUCCESS;
}

void *ai_find(ArrayIndex *a, int32_t key)
{
    size_t lo = 0, hi;
    if (!a) return NULL;
    hi = a->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int32_t k = a->items[mid].key;
        a->comparisons++;
        if (k == key) return a->items[mid].value;
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void *ai_find_linear(ArrayIndex *a, int32_t key)
{
    size_t i;
    if (!a) return NULL;
    for (i = 0; i < a->count; i++) {
        a->comparisons++;
        if (a->items[i].key == key) return a->items[i].value;
    }
    return NULL;
}
