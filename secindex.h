/*
 * secindex.h - Secondary index: secondary key -> list of primary keys.
 *
 * A primary index answers "where is the record with key K?". A secondary
 * index answers "which records have field F = v?" - e.g. every flight on
 * the route DEL -> BOM - without reading the whole data file.
 *
 * The secondary key is an int64 produced by an extractor function from a
 * record (SkyBook packs the two 3-letter airport codes into one integer).
 * Many records can share a secondary key, so each key maps to a growable
 * array of primary keys. Buckets use separate chaining.
 */
#ifndef SECINDEX_H
#define SECINDEX_H

#include <stddef.h>
#include <stdint.h>

#define SI_NAME_LEN 32

#define SI_SUCCESS      0
#define SI_NOT_FOUND    1
#define SI_ALLOC_FAILED 2
#define SI_INVALID      3

/* Extracts the secondary key from a record. */
typedef int64_t (*si_key_fn)(const void *rec);

typedef struct SINode {
    int64_t skey;
    int32_t *pks;           /* primary keys with this secondary key */
    size_t count;
    size_t cap;
    struct SINode *next;
} SINode;

typedef struct {
    char name[SI_NAME_LEN];
    si_key_fn key_fn;
    SINode **buckets;
    size_t nbuckets;
    size_t nkeys;           /* distinct secondary keys */
    size_t nentries;        /* (secondary key, primary key) pairs */
} SecIndex;

SecIndex *si_create(const char *name, si_key_fn key_fn);
void      si_free(SecIndex *s);

int si_add(SecIndex *s, int64_t skey, int32_t pk);
int si_remove(SecIndex *s, int64_t skey, int32_t pk);

/* Primary keys for skey (NULL and *count = 0 if none). The pointer is
 * valid until the next si_add/si_remove. */
const int32_t *si_find(const SecIndex *s, int64_t skey, size_t *count);

#endif
