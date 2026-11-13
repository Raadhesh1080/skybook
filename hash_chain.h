/*
 * hash_chain.h - Separate-chaining hash table.
 *
 * Not used by the engine itself; it exists as the baseline that the
 * open-addressing index is benchmarked against.
 */
#ifndef HASH_CHAIN_H
#define HASH_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#include "hashfn.h"

typedef struct HCNode {
    int32_t key;
    void *value;
    struct HCNode *next;
} HCNode;

typedef struct {
    HCNode **buckets;
    size_t nbuckets;
    size_t count;
    uint64_t probes;        /* chain nodes inspected by hc_find (instrumentation) */
} HashChain;

HashChain *hc_create(size_t nbuckets);
void       hc_free(HashChain *h);

int   hc_insert(HashChain *h, int32_t key, void *value);  /* insert or overwrite */
void *hc_find(HashChain *h, int32_t key);
int   hc_remove(HashChain *h, int32_t key);

double hc_load(const HashChain *h);  /* count / nbuckets */

#endif
