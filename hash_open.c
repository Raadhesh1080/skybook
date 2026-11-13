/*
 * hash_open.c - Open-addressing hash table with linear probing.
 */
#include <stdlib.h>

#include "hash_open.h"

#define SLOT_EMPTY 0
#define SLOT_FULL  1
#define SLOT_TOMB  2

#define HO_MIN_CAP   16
#define HO_MAX_LOAD  0.95   /* at least one EMPTY slot must always exist */

static size_t round_pow2(size_t n)
{
    size_t c = HO_MIN_CAP;
    while (c < n) c <<= 1;
    return c;
}

HashOpen *ho_create(size_t initial_cap, double max_load)
{
    HashOpen *h;

    if (max_load < 0.1 || max_load > HO_MAX_LOAD) max_load = 0.7;

    h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cap = round_pow2(initial_cap);
    h->max_load = max_load;
    h->slots = calloc(h->cap, sizeof *h->slots);
    if (!h->slots) {
        free(h);
        return NULL;
    }
    return h;
}

void ho_free(HashOpen *h)
{
    if (!h) return;
    free(h->slots);
    free(h);
}

void *ho_find(HashOpen *h, int32_t key)
{
    size_t mask, i;

    if (!h) return NULL;
    mask = h->cap - 1;
    i = hash_int32(key) & mask;
    for (;;) {
        const HOSlot *s = &h->slots[i];
        h->probes++;
        if (s->state == SLOT_EMPTY) return NULL;          /* end of probe run */
        if (s->state == SLOT_FULL && s->key == key) return s->value;
        i = (i + 1) & mask;                                /* tombstone or other key */
    }
}

int ho_rehash(HashOpen *h, size_t new_cap)
{
    HOSlot *fresh;
    size_t mask, i;

    if (!h) return HT_INVALID;
    new_cap = round_pow2(new_cap);
    if ((double)(h->used + 1) > (double)new_cap * h->max_load) return HT_INVALID;

    fresh = calloc(new_cap, sizeof *fresh);
    if (!fresh) return HT_ALLOC_FAILED;

    mask = new_cap - 1;
    for (i = 0; i < h->cap; i++) {
        size_t j;
        if (h->slots[i].state != SLOT_FULL) continue;      /* tombstones dropped */
        j = hash_int32(h->slots[i].key) & mask;
        while (fresh[j].state == SLOT_FULL) j = (j + 1) & mask;
        fresh[j] = h->slots[i];
    }
    free(h->slots);
    h->slots = fresh;
    h->cap = new_cap;
    h->tombs = 0;
    return HT_SUCCESS;
}

int ho_insert(HashOpen *h, int32_t key, void *value)
{
    size_t mask, i, tomb = (size_t)-1;

    if (!h) return HT_INVALID;

    mask = h->cap - 1;
    i = hash_int32(key) & mask;
    for (;;) {
        HOSlot *s = &h->slots[i];
        if (s->state == SLOT_EMPTY) break;
        if (s->state == SLOT_TOMB) {
            if (tomb == (size_t)-1) tomb = i;               /* remember first reusable slot */
        } else if (s->key == key) {
            s->value = value;                               /* overwrite existing key */
            return HT_SUCCESS;
        }
        i = (i + 1) & mask;
    }

    if (tomb != (size_t)-1) {
        /* Reusing a tombstone doesn't raise occupancy. */
        i = tomb;
        h->tombs--;
    } else if ((double)(h->used + h->tombs + 1) > (double)h->cap * h->max_load) {
        /* Grow if live keys are filling the table; otherwise the pressure
         * comes from tombstones, and a same-size rehash clears them. */
        size_t new_cap = ((double)(h->used + 1) > (double)h->cap * h->max_load / 2)
                             ? h->cap * 2 : h->cap;
        int rc = ho_rehash(h, new_cap);
        if (rc != HT_SUCCESS) return rc;
        return ho_insert(h, key, value);
    }

    h->slots[i].key = key;
    h->slots[i].value = value;
    h->slots[i].state = SLOT_FULL;
    h->used++;
    return HT_SUCCESS;
}

int ho_remove(HashOpen *h, int32_t key)
{
    size_t mask, i;

    if (!h) return HT_INVALID;
    mask = h->cap - 1;
    i = hash_int32(key) & mask;
    for (;;) {
        HOSlot *s = &h->slots[i];
        if (s->state == SLOT_EMPTY) return HT_NOT_FOUND;
        if (s->state == SLOT_FULL && s->key == key) {
            s->state = SLOT_TOMB;
            s->value = NULL;
            h->used--;
            h->tombs++;
            return HT_SUCCESS;
        }
        i = (i + 1) & mask;
    }
}

double ho_load(const HashOpen *h)
{
    return h && h->cap ? (double)h->used / (double)h->cap : 0.0;
}

double ho_occupancy(const HashOpen *h)
{
    return h && h->cap ? (double)(h->used + h->tombs) / (double)h->cap : 0.0;
}
