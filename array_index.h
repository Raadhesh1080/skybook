/*
 * array_index.h - Array-based index (int key -> void* value).
 *
 * The simplest possible in-memory index: a flat array of { key, value }
 * entries. Two lookup strategies are provided so the benchmark can show
 * how much ordering the array buys:
 *   ai_find_linear - scan every entry, O(n)  (works on any order)
 *   ai_find        - binary search, O(log n) (array kept sorted by key)
 * Keeping the array sorted makes inserts O(n): every entry after the
 * insertion point has to be shifted along by one.
 */
#ifndef ARRAY_INDEX_H
#define ARRAY_INDEX_H

#include <stddef.h>
#include <stdint.h>

#define AI_SUCCESS      0
#define AI_DUP_KEY      1
#define AI_NOT_FOUND    2
#define AI_ALLOC_FAILED 3
#define AI_INVALID      4

typedef struct {
    int32_t key;
    void *value;
} AIEntry;

typedef struct {
    AIEntry *items;
    size_t count;
    size_t cap;
    uint64_t comparisons;   /* key comparisons made by lookups (instrumentation) */
} ArrayIndex;

ArrayIndex *ai_create(size_t initial_cap);
void        ai_free(ArrayIndex *a);

int   ai_insert(ArrayIndex *a, int32_t key, void *value);  /* keeps the array sorted */
int   ai_append(ArrayIndex *a, int32_t key, void *value);  /* bulk load: key must exceed the last key */
int   ai_remove(ArrayIndex *a, int32_t key);
void *ai_find(ArrayIndex *a, int32_t key);                 /* binary search */
void *ai_find_linear(ArrayIndex *a, int32_t key);          /* full scan     */

#endif
