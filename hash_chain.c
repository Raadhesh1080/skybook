/*
 * hash_chain.c - Separate-chaining hash table (fixed bucket count).
 */
#include <stdlib.h>

#include "hash_chain.h"

HashChain *hc_create(size_t nbuckets)
{
    HashChain *h;

    if (nbuckets == 0) nbuckets = 1;
    h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->buckets = calloc(nbuckets, sizeof *h->buckets);
    if (!h->buckets) {
        free(h);
        return NULL;
    }
    h->nbuckets = nbuckets;
    return h;
}

void hc_free(HashChain *h)
{
    size_t i;

    if (!h) return;
    for (i = 0; i < h->nbuckets; i++) {
        HCNode *n = h->buckets[i];
        while (n) {
            HCNode *next = n->next;
            free(n);
            n = next;
        }
    }
    free(h->buckets);
    free(h);
}

int hc_insert(HashChain *h, int32_t key, void *value)
{
    size_t b;
    HCNode *n;

    if (!h) return HT_INVALID;
    b = hash_int32(key) % h->nbuckets;
    for (n = h->buckets[b]; n; n = n->next) {
        if (n->key == key) {
            n->value = value;
            return HT_SUCCESS;
        }
    }
    n = malloc(sizeof *n);
    if (!n) return HT_ALLOC_FAILED;
    n->key = key;
    n->value = value;
    n->next = h->buckets[b];
    h->buckets[b] = n;
    h->count++;
    return HT_SUCCESS;
}

void *hc_find(HashChain *h, int32_t key)
{
    HCNode *n;

    if (!h) return NULL;
    for (n = h->buckets[hash_int32(key) % h->nbuckets]; n; n = n->next) {
        h->probes++;
        if (n->key == key) return n->value;
    }
    return NULL;
}

int hc_remove(HashChain *h, int32_t key)
{
    HCNode **link;

    if (!h) return HT_INVALID;
    for (link = &h->buckets[hash_int32(key) % h->nbuckets]; *link; link = &(*link)->next) {
        if ((*link)->key == key) {
            HCNode *dead = *link;
            *link = dead->next;
            free(dead);
            h->count--;
            return HT_SUCCESS;
        }
    }
    return HT_NOT_FOUND;
}

double hc_load(const HashChain *h)
{
    return h && h->nbuckets ? (double)h->count / (double)h->nbuckets : 0.0;
}
