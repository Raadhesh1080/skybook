/*
 * hashfn.h - Shared hash function and return codes for both hash tables.
 */
#ifndef HASHFN_H
#define HASHFN_H

#include <stdint.h>

#define HT_SUCCESS      0
#define HT_NOT_FOUND    1
#define HT_ALLOC_FAILED 2
#define HT_INVALID      3

/* Integer finalizer (lowbias32). Sequential keys such as 1, 2, 3 ... are
 * spread uniformly across the table, which matters a lot for linear probing:
 * without mixing, consecutive keys would form one long contiguous cluster. */
static inline uint32_t hash_int32(int32_t key)
{
    uint32_t x = (uint32_t)key;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

#endif
