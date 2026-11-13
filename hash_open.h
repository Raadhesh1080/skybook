/*
 * hash_open.h - Open-addressing hash table (linear probing + tombstones).
 *
 * This is SkyBook's primary-key index. All entries live in one flat array of
 * slots, so a lookup touches contiguous memory instead of chasing list
 * pointers. Deleting a key leaves a TOMBSTONE in its slot so that probe
 * sequences passing through it stay intact; tombstones are purged on rehash.
 */
#ifndef HASH_OPEN_H
#define HASH_OPEN_H

#include <stddef.h>
#include <stdint.h>

#include "hashfn.h"

typedef struct {
    int32_t key;
    uint8_t state;          /* empty / full / tombstone */
    void *value;
} HOSlot;

typedef struct {
    HOSlot *slots;
    size_t cap;             /* always a power of two */
    size_t used;            /* live keys             */
    size_t tombs;           /* tombstoned slots      */
    double max_load;        /* rehash when (used + tombs) / cap would exceed this */
    uint64_t probes;        /* slots inspected by ho_find (instrumentation) */
} HashOpen;

HashOpen *ho_create(size_t initial_cap, double max_load);
void      ho_free(HashOpen *h);

int   ho_insert(HashOpen *h, int32_t key, void *value);  /* insert or overwrite */
void *ho_find(HashOpen *h, int32_t key);                 /* NULL if absent      */
int   ho_remove(HashOpen *h, int32_t key);               /* leaves a tombstone  */
int   ho_rehash(HashOpen *h, size_t new_cap);            /* rebuild, drops tombstones */

double ho_load(const HashOpen *h);        /* used / cap            */
double ho_occupancy(const HashOpen *h);   /* (used + tombs) / cap  */

#endif
