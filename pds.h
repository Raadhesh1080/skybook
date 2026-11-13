/*
 * pds.h - Persistent Data Store engine.
 *
 * A database is a folder holding a <db>.info catalog plus, per table,
 *   <table>.dat  - append-only log of frames:
 *                  [int32 key][int32 flags][record bytes]
 *                  flags = FRAME_LIVE (a record version) or
 *                          FRAME_TOMBSTONE (the key was deleted)
 *   <table>.ndx  - index snapshot: header { count, dat_end } +
 *                  { key, is_deleted, offset } entries
 *
 * The .dat file alone holds the full history, so the index can always be
 * rebuilt from it. On open, if the .ndx is missing, unreadable, or its
 * dat_end doesn't match the data file (writes happened after the last
 * clean close - e.g. a crash), the index is rebuilt by scanning .dat.
 *
 * While a table is open, these in-memory indexes are kept in sync:
 *   - an open-addressing hash table (live keys -> index entry), O(1) average
 *   - an AVL tree over every index entry (live and deleted), for ordered
 *     traversal, range scans and persisting the index
 *   - optional secondary indexes (field value -> primary keys)
 */
#ifndef PDS_H
#define PDS_H

#include <stdint.h>
#include <stdio.h>

#include "bst.h"
#include "hash_open.h"
#include "secindex.h"

#define PDS_MAX_TABLES    16
#define PDS_MAX_SECONDARY 4
#define PDS_NAME_LEN      32

#define PDS_DB_CLOSED 0
#define PDS_DB_OPEN   1

#define PDS_TABLE_CLOSED 0
#define PDS_TABLE_OPEN   1

#define REC_NOT_DELETED 0
#define REC_DELETED     1

#define FRAME_LIVE      1
#define FRAME_TOMBSTONE 2
#define FRAME_HEADER    8     /* int32 key + int32 flags */

/* Durability (see pds_set_sync_mode). */
#define PDS_SYNC_NONE     0   /* never fsync; the OS writes data back when it likes */
#define PDS_SYNC_ON_CLOSE 1   /* fsync .dat/.ndx/.info when a table or db is closed (default) */
#define PDS_SYNC_ALWAYS   2   /* also flush + fsync .dat after every put/update/delete */

/* Why a table's index was rebuilt from .dat on open (PDS_TableStats). */
#define PDS_RECOVERY_NONE        0   /* loaded normally from .ndx */
#define PDS_RECOVERY_NDX_MISSING 1
#define PDS_RECOVERY_NDX_CORRUPT 2
#define PDS_RECOVERY_NDX_STALE   3   /* .dat changed after the .ndx was written */

enum {
    PDS_SUCCESS = 0,
    PDS_FILE_ERROR,
    PDS_ALLOC_FAILED,
    PDS_REC_NOT_FOUND,
    PDS_DUPLICATE_KEY,
    PDS_DB_NOT_OPEN,
    PDS_DB_ALREADY_OPEN,
    PDS_DB_EXISTS,
    PDS_TABLE_NOT_FOUND,
    PDS_TABLE_NOT_OPEN,
    PDS_TABLE_ALREADY_OPEN,
    PDS_TABLE_EXISTS,
    PDS_TOO_MANY_TABLES,
    PDS_INVALID_ARG,
    PDS_MATCHER_ERROR,
    PDS_CORRUPT,
    PDS_OLD_FORMAT,
    PDS_INDEX_EXISTS,
    PDS_INDEX_NOT_FOUND,
    PDS_TOO_MANY_INDEXES
};

/* One index entry: where a key's current record lives in the .dat file. */
typedef struct {
    int32_t key;
    int32_t is_deleted;
    int64_t offset;
} PDS_NdxInfo;

typedef struct {
    char name[PDS_NAME_LEN];
    int32_t rec_size;
    int status;
    FILE *data_fp;
    int64_t end_offset;     /* where the next frame will be appended */
    int at_end;             /* file position is already at end_offset */
    void *scratch;          /* rec_size bytes of working space */
    BSTNode *bst_root;      /* AVL tree; owns the PDS_NdxInfo entries */
    HashOpen *hash;         /* live keys only, points into the same entries */
    SecIndex *secondary[PDS_MAX_SECONDARY];
    int num_secondary;
    long live_count;
    long deleted_count;
    int recovery;           /* PDS_RECOVERY_* for the last open */
} PDS_TableInfo;

typedef struct {
    char name[PDS_NAME_LEN];
    int status;
    int num_tables;
    PDS_TableInfo tables[PDS_MAX_TABLES];
} PDS_DBInfo;

typedef struct {
    long live;
    long deleted;
    int64_t data_bytes;
    size_t hash_cap;
    size_t hash_used;
    size_t hash_tombs;
    int bst_height;
    double bst_avg_depth;
    int num_secondary;
    int recovery;           /* PDS_RECOVERY_* */
} PDS_TableStats;

/* Matcher: 0 = match, 1 = no match, >1 = error (see record.h). */
typedef int (*pds_matcher_fn)(const void *rec, const void *query);
/* Visitor for scans: return 0 to continue, non-zero to stop. */
typedef int (*pds_visit_fn)(int32_t key, const void *rec, void *ctx);
/* Text conversion for CSV export/import: return 0 on success. */
typedef int (*pds_to_text_fn)(const void *rec, char *line, size_t size);
typedef int (*pds_from_text_fn)(const char *line, void *rec, int32_t *key);

/* ---- Database ----------------------------------------------------------- */
int pds_create_db(const char *db_name);
int pds_open_db(const char *db_name);
int pds_close_db(void);
int pds_drop_db(const char *db_name);   /* deletes the folder; db must be closed */

/* ---- Durability --------------------------------------------------------- */
void pds_set_sync_mode(int mode);       /* PDS_SYNC_*; applies to all tables */
int  pds_get_sync_mode(void);

/* ---- Tables ------------------------------------------------------------- */
int pds_create_table(const char *table, int32_t rec_size);
int pds_open_table(const char *table);  /* rebuilds the index from .dat if needed */
int pds_close_table(const char *table);

/* Testing/benchmarks only: simulates the process dying with this table
 * open. Buffered writes are handed to the OS (as on a kill -9 after the
 * write calls returned), but no .ndx is written and nothing is fsynced. */
int pds_simulate_crash(const char *table);

/* ---- CRUD --------------------------------------------------------------- */
int pds_put_rec(const char *table, int32_t key, const void *rec);
int pds_get_rec(const char *table, int32_t key, void *rec);          /* hash index */
int pds_get_rec_via_bst(const char *table, int32_t key, void *rec);  /* AVL index  */
int pds_update_rec(const char *table, int32_t key, const void *rec); /* in place   */
int pds_delete_rec(const char *table, int32_t key);                  /* soft delete: appends a tombstone frame */

/* No-index key lookup: reads the .dat file from the start, comparing the
 * key of every frame until it finds the record. O(n). *comp_count receives
 * the number of frames compared. The in-memory index is consulted only to
 * reject a matching frame that is deleted or a stale older copy. */
int pds_get_rec_by_scan(const char *table, int32_t key, void *rec, long *comp_count);

/* Reads the record an index entry points at (for external indexes). */
int pds_get_rec_at(const char *table, const PDS_NdxInfo *entry, void *rec);

/* Unindexed search: scans every live record and calls matcher on it.
 * visit (may be NULL) is called for each match. */
int pds_get_rec_by_field(const char *table, const void *query, pds_matcher_fn matcher,
                         pds_visit_fn visit, void *ctx,
                         long *match_count, long *comp_count);

/* Ordered scan of live records with lo <= key <= hi, via the AVL tree. */
int pds_scan_range(const char *table, int32_t lo, int32_t hi,
                   pds_visit_fn visit, void *ctx, long *count);

/* Largest key ever used in the table (live or deleted). */
int pds_max_key(const char *table, int32_t *key);

int pds_table_stats(const char *table, PDS_TableStats *stats);

/* ---- Secondary indexes --------------------------------------------------
 * Built in memory from the live records when created, then kept in sync by
 * put/update/delete. Not persisted: register them again after each
 * pds_open_table (SkyBook does this in sb_open). */
int pds_create_secondary_index(const char *table, const char *index_name, si_key_fn key_fn);

/* Calls visit for every live record whose secondary key equals skey.
 * *fetched = records read from disk (each candidate is read once). */
int pds_get_by_secondary(const char *table, const char *index_name, int64_t skey,
                         pds_visit_fn visit, void *ctx, long *match_count, long *fetched);

/* ---- Text (CSV) export / import ----------------------------------------
 * Export writes every live record, in storage (insertion) order, as one
 * line of text using a text-mode file (fopen "w"). Import reads lines back
 * with fgets, parses them with from_text and inserts them; malformed lines
 * and keys that already exist are counted in *skipped. Paths are relative
 * to the database folder. */
int pds_export_text(const char *table, const char *path, const char *header,
                    pds_to_text_fn to_text, long *count);
int pds_import_text(const char *table, const char *path, int skip_header,
                    pds_from_text_fn from_text, long *imported, long *skipped);

/* Direct access for benchmarks and diagnostics; NULL if not open. */
PDS_TableInfo *pds_table_info(const char *table);
const PDS_DBInfo *pds_db_info(void);

const char *pds_strerror(int code);
const char *pds_recovery_name(int recovery);

#endif
