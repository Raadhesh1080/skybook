/*
 * secindex.c - Secondary index (int64 key -> list of int32 primary keys).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "secindex.h"

#define SI_MIN_BUCKETS 64

static uint64_t hash_int64(int64_t key)
{
    uint64_t x = (uint64_t)key;        /* splitmix64 finalizer */
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

SecIndex *si_create(const char *name, si_key_fn key_fn)
{
    SecIndex *s;

    if (!name || !key_fn) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->buckets = calloc(SI_MIN_BUCKETS, sizeof *s->buckets);
    if (!s->buckets) {
        free(s);
        return NULL;
    }
    snprintf(s->name, sizeof s->name, "%s", name);
    s->key_fn = key_fn;
    s->nbuckets = SI_MIN_BUCKETS;
    return s;
}

void si_free(SecIndex *s)
{
    size_t i;
    if (!s) return;
    for (i = 0; i < s->nbuckets; i++) {
        SINode *n = s->buckets[i];
        while (n) {
            SINode *next = n->next;
            free(n->pks);
            free(n);
            n = next;
        }
    }
    free(s->buckets);
    free(s);
}

static SINode *find_node(const SecIndex *s, int64_t skey)
{
    SINode *n = s->buckets[hash_int64(skey) % s->nbuckets];
    while (n && n->skey != skey) n = n->next;
    return n;
}

/* Doubles the bucket array once there are more keys than buckets. */
static void maybe_grow(SecIndex *s)
{
    size_t new_n, i;
    SINode **fresh;

    if (s->nkeys <= s->nbuckets) return;
    new_n = s->nbuckets * 2;
    fresh = calloc(new_n, sizeof *fresh);
    if (!fresh) return;                 /* keep working with longer chains */
    for (i = 0; i < s->nbuckets; i++) {
        SINode *n = s->buckets[i];
        while (n) {
            SINode *next = n->next;
            size_t b = hash_int64(n->skey) % new_n;
            n->next = fresh[b];
            fresh[b] = n;
            n = next;
        }
    }
    free(s->buckets);
    s->buckets = fresh;
    s->nbuckets = new_n;
}

int si_add(SecIndex *s, int64_t skey, int32_t pk)
{
    SINode *n;

    if (!s) return SI_INVALID;
    n = find_node(s, skey);
    if (!n) {
        size_t b;
        n = calloc(1, sizeof *n);
        if (!n) return SI_ALLOC_FAILED;
        n->skey = skey;
        b = hash_int64(skey) % s->nbuckets;
        n->next = s->buckets[b];
        s->buckets[b] = n;
        s->nkeys++;
        maybe_grow(s);
    }
    if (n->count == n->cap) {
        size_t new_cap = n->cap ? n->cap * 2 : 4;
        int32_t *grown = realloc(n->pks, new_cap * sizeof *grown);
        if (!grown) return SI_ALLOC_FAILED;
        n->pks = grown;
        n->cap = new_cap;
    }
    n->pks[n->count++] = pk;
    s->nentries++;
    return SI_SUCCESS;
}

int si_remove(SecIndex *s, int64_t skey, int32_t pk)
{
    SINode **link;
    size_t i;

    if (!s) return SI_INVALID;
    for (link = &s->buckets[hash_int64(skey) % s->nbuckets]; *link; link = &(*link)->next) {
        SINode *n = *link;
        if (n->skey != skey) continue;
        for (i = 0; i < n->count; i++) {
            if (n->pks[i] != pk) continue;
            n->pks[i] = n->pks[--n->count];      /* order within a key doesn't matter */
            s->nentries--;
            if (n->count == 0) {                 /* last one: drop the key */
                *link = n->next;
                free(n->pks);
                free(n);
                s->nkeys--;
            }
            return SI_SUCCESS;
        }
        return SI_NOT_FOUND;
    }
    return SI_NOT_FOUND;
}

const int32_t *si_find(const SecIndex *s, int64_t skey, size_t *count)
{
    const SINode *n = s ? find_node(s, skey) : NULL;
    if (count) *count = n ? n->count : 0;
    return n ? n->pks : NULL;
}
