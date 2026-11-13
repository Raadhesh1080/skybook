/*
 * pds.c - Persistent Data Store engine.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pds.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define PDS_MKDIR(p) _mkdir(p)
#define PDS_RMDIR(p) _rmdir(p)
#define PDS_CHDIR(p) _chdir(p)
#define pds_fseek    _fseeki64
#define pds_ftell    _ftelli64
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define PDS_MKDIR(p) mkdir((p), 0755)
#define PDS_RMDIR(p) rmdir(p)
#define PDS_CHDIR(p) chdir(p)
#define pds_fseek    fseeko
#define pds_ftell    ftello
#endif

#define INFO_MAGIC     "SBD2"
#define INFO_MAGIC_OLD "SBDB"   /* format before tombstone frames */
#define NDX_MAGIC      "SBN2"
#define PATH_LEN       (2 * PDS_NAME_LEN + 16)
#define DATA_BUF       (64 * 1024)

static PDS_DBInfo g_db;
static int g_sync_mode = PDS_SYNC_ON_CLOSE;

/* ======================================================================== */
/*  Helpers                                                                 */
/* ======================================================================== */

static int valid_name(const char *s)
{
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (n == 0 || n >= PDS_NAME_LEN) return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    }
    return 1;
}

static PDS_TableInfo *find_table(const char *name)
{
    int i;
    if (!name) return NULL;
    for (i = 0; i < g_db.num_tables; i++) {
        if (strcmp(g_db.tables[i].name, name) == 0) return &g_db.tables[i];
    }
    return NULL;
}

/* Every data operation goes through this guard: it fails unless the
 * database is open AND the table is open. */
static int get_open_table(const char *name, PDS_TableInfo **out)
{
    PDS_TableInfo *t;
    if (g_db.status != PDS_DB_OPEN) return PDS_DB_NOT_OPEN;
    t = find_table(name);
    if (!t) return PDS_TABLE_NOT_FOUND;
    if (t->status != PDS_TABLE_OPEN) return PDS_TABLE_NOT_OPEN;
    *out = t;
    return PDS_SUCCESS;
}

static int64_t frame_size(const PDS_TableInfo *t)
{
    return (int64_t)FRAME_HEADER + t->rec_size;
}

static int match_all(const void *rec, const void *query)
{
    (void)rec;
    (void)query;
    return 0;
}

/* ---- Durability --------------------------------------------------------- */

void pds_set_sync_mode(int mode)
{
    if (mode >= PDS_SYNC_NONE && mode <= PDS_SYNC_ALWAYS) g_sync_mode = mode;
}

int pds_get_sync_mode(void)
{
    return g_sync_mode;
}

/* fflush moves stdio's buffer into the OS; fsync (_commit on Windows) makes
 * the OS write its cache to the disk and wait until it is there. Only after
 * fsync does data survive a power cut. */
static int sync_fp(FILE *fp)
{
    if (fflush(fp) != 0) return -1;
#ifdef _WIN32
    return _commit(_fileno(fp));
#else
    return fsync(fileno(fp));
#endif
}

/* On POSIX a rename is only durable once the directory itself is synced.
 * Windows has no equivalent for directories. */
static void sync_cwd(void)
{
#ifndef _WIN32
    int fd = open(".", O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
#endif
}

/* ---- .info catalog ------------------------------------------------------ */

static int write_info(const char *path, const PDS_DBInfo *db)
{
    FILE *fp = fopen(path, "wb");
    int32_t n = db->num_tables;
    int i, ok;

    if (!fp) return PDS_FILE_ERROR;
    ok = fwrite(INFO_MAGIC, 1, 4, fp) == 4 && fwrite(&n, sizeof n, 1, fp) == 1;
    for (i = 0; ok && i < n; i++) {
        ok = fwrite(db->tables[i].name, PDS_NAME_LEN, 1, fp) == 1 &&
             fwrite(&db->tables[i].rec_size, sizeof(int32_t), 1, fp) == 1;
    }
    if (ok && g_sync_mode != PDS_SYNC_NONE && sync_fp(fp) != 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    return ok ? PDS_SUCCESS : PDS_FILE_ERROR;
}

static int read_info(const char *path, PDS_DBInfo *db)
{
    FILE *fp = fopen(path, "rb");
    char magic[4];
    int32_t n;
    int i;

    if (!fp) return PDS_FILE_ERROR;
    if (fread(magic, 1, 4, fp) != 4) {
        fclose(fp);
        return PDS_CORRUPT;
    }
    if (memcmp(magic, INFO_MAGIC_OLD, 4) == 0) {
        fclose(fp);
        return PDS_OLD_FORMAT;
    }
    if (memcmp(magic, INFO_MAGIC, 4) != 0 || fread(&n, sizeof n, 1, fp) != 1 ||
        n < 0 || n > PDS_MAX_TABLES) {
        fclose(fp);
        return PDS_CORRUPT;
    }
    db->num_tables = n;
    for (i = 0; i < n; i++) {
        PDS_TableInfo *t = &db->tables[i];
        memset(t, 0, sizeof *t);
        if (fread(t->name, PDS_NAME_LEN, 1, fp) != 1 ||
            fread(&t->rec_size, sizeof(int32_t), 1, fp) != 1 || t->rec_size <= 0) {
            fclose(fp);
            return PDS_CORRUPT;
        }
        t->name[PDS_NAME_LEN - 1] = '\0';
        t->status = PDS_TABLE_CLOSED;
    }
    fclose(fp);
    return PDS_SUCCESS;
}

static int save_catalog(void)
{
    char path[PATH_LEN];
    snprintf(path, sizeof path, "%s.info", g_db.name);
    return write_info(path, &g_db);
}

/* ---- .ndx index file ---------------------------------------------------- */

static int write_entry(FILE *fp, const PDS_NdxInfo *e)
{
    return fwrite(&e->key, sizeof e->key, 1, fp) == 1 &&
           fwrite(&e->is_deleted, sizeof e->is_deleted, 1, fp) == 1 &&
           fwrite(&e->offset, sizeof e->offset, 1, fp) == 1;
}

static int read_entry(FILE *fp, PDS_NdxInfo *e)
{
    return fread(&e->key, sizeof e->key, 1, fp) == 1 &&
           fread(&e->is_deleted, sizeof e->is_deleted, 1, fp) == 1 &&
           fread(&e->offset, sizeof e->offset, 1, fp) == 1 &&
           (e->is_deleted == REC_NOT_DELETED || e->is_deleted == REC_DELETED) &&
           e->offset >= 0;
}

static int write_entry_cb(BSTNode *node, void *ctx)
{
    return write_entry((FILE *)ctx, (const PDS_NdxInfo *)node->data) ? 0 : 1;
}

/* Header: magic, entry count, and dat_end - the size of the .dat file this
 * snapshot describes. Entries are written in AVL level order, so
 * re-inserting them on the next open rebuilds a tree of the same shape.
 * The file is written to <table>.ndx.tmp, synced, then swapped in. If we
 * crash in between, the .ndx is missing or old - and open() then rebuilds
 * the index from .dat, so nothing is lost either way. */
static int write_ndx_file(const char *table, BSTNode *root, int32_t count, int64_t dat_end)
{
    char tmp[PATH_LEN], final_path[PATH_LEN];
    FILE *fp;
    int ok;

    snprintf(tmp, sizeof tmp, "%s.ndx.tmp", table);
    snprintf(final_path, sizeof final_path, "%s.ndx", table);

    fp = fopen(tmp, "wb");
    if (!fp) return PDS_FILE_ERROR;
    ok = fwrite(NDX_MAGIC, 1, 4, fp) == 4 && fwrite(&count, sizeof count, 1, fp) == 1 &&
         fwrite(&dat_end, sizeof dat_end, 1, fp) == 1 &&
         bst_levelorder(root, write_entry_cb, fp) == 0;
    if (ok && g_sync_mode != PDS_SYNC_NONE && sync_fp(fp) != 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return PDS_FILE_ERROR;
    }
    remove(final_path);              /* rename() won't replace a file on Windows */
    if (rename(tmp, final_path) != 0) return PDS_FILE_ERROR;
    if (g_sync_mode != PDS_SYNC_NONE) sync_cwd();
    return PDS_SUCCESS;
}

/* Frees the in-memory indexes (not the data file). */
static void release_indexes(PDS_TableInfo *t)
{
    int i;
    bst_free_with_data(t->bst_root);
    ho_free(t->hash);
    for (i = 0; i < t->num_secondary; i++) si_free(t->secondary[i]);
    t->bst_root = NULL;
    t->hash = NULL;
    t->num_secondary = 0;
    t->live_count = t->deleted_count = 0;
}

static void release_table(PDS_TableInfo *t)
{
    release_indexes(t);
    free(t->scratch);
    t->scratch = NULL;
}

/* Loads both in-memory indexes from <table>.ndx. If the file is missing,
 * unreadable or describes a different .dat size, returns an error and sets
 * *reason; the caller then rebuilds from .dat. */
static int load_ndx(PDS_TableInfo *t, int *reason)
{
    char path[PATH_LEN], magic[4];
    int32_t count, i;
    int64_t dat_end;
    int rc = PDS_SUCCESS;
    FILE *fp;

    snprintf(path, sizeof path, "%s.ndx", t->name);
    fp = fopen(path, "rb");
    if (!fp) {
        *reason = PDS_RECOVERY_NDX_MISSING;
        return PDS_FILE_ERROR;
    }
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, NDX_MAGIC, 4) != 0 ||
        fread(&count, sizeof count, 1, fp) != 1 || count < 0 ||
        fread(&dat_end, sizeof dat_end, 1, fp) != 1) {
        fclose(fp);
        *reason = PDS_RECOVERY_NDX_CORRUPT;
        return PDS_CORRUPT;
    }
    if (dat_end != t->end_offset) {
        fclose(fp);
        *reason = PDS_RECOVERY_NDX_STALE;
        return PDS_CORRUPT;
    }

    for (i = 0; i < count && rc == PDS_SUCCESS; i++) {
        PDS_NdxInfo *e = malloc(sizeof *e);
        if (!e) { rc = PDS_ALLOC_FAILED; break; }
        if (!read_entry(fp, e) || e->offset >= dat_end) { free(e); rc = PDS_CORRUPT; break; }
        switch (bst_add_node(&t->bst_root, e->key, e)) {
        case BST_SUCCESS: break;
        case BST_DUP_KEY: free(e); rc = PDS_CORRUPT; continue;
        default:          free(e); rc = PDS_ALLOC_FAILED; continue;
        }
        if (e->is_deleted) {
            t->deleted_count++;
        } else if (ho_insert(t->hash, e->key, e) != HT_SUCCESS) {
            rc = PDS_ALLOC_FAILED;
        } else {
            t->live_count++;
        }
    }
    fclose(fp);
    if (rc == PDS_CORRUPT) *reason = PDS_RECOVERY_NDX_CORRUPT;
    return rc;
}

/* Recovery: rebuilds the index by replaying .dat from the start. Each LIVE
 * frame makes its key live at that offset (a later frame for the same key
 * is a newer version); each TOMBSTONE frame marks its key deleted. Zeroed
 * alignment gaps and a torn frame at the very end are ignored. */
static int rebuild_from_dat(PDS_TableInfo *t)
{
    unsigned char *block;
    size_t frame = (size_t)frame_size(t), per_block, got, i;
    int64_t pos = 0;
    int rc = PDS_SUCCESS;

    per_block = DATA_BUF / frame ? DATA_BUF / frame : 1;
    block = malloc(per_block * frame);
    if (!block) return PDS_ALLOC_FAILED;
    t->at_end = 0;
    if (pds_fseek(t->data_fp, 0, SEEK_SET) != 0) {
        free(block);
        return PDS_FILE_ERROR;
    }

    while (rc == PDS_SUCCESS && (got = fread(block, frame, per_block, t->data_fp)) > 0) {
        for (i = 0; i < got && rc == PDS_SUCCESS; i++, pos += (int64_t)frame) {
            const unsigned char *f = block + i * frame;
            int32_t key, flags;
            BSTNode *node;
            PDS_NdxInfo *e;

            memcpy(&key, f, sizeof key);
            memcpy(&flags, f + sizeof key, sizeof flags);
            if (flags != FRAME_LIVE && flags != FRAME_TOMBSTONE) continue;

            node = bst_search(t->bst_root, key);
            e = node ? node->data : NULL;
            if (!e) {
                e = malloc(sizeof *e);
                if (!e) { rc = PDS_ALLOC_FAILED; break; }
                e->key = key;
                e->offset = pos;
                e->is_deleted = REC_DELETED;          /* made live below if LIVE */
                if (bst_add_node(&t->bst_root, key, e) != BST_SUCCESS) {
                    free(e);
                    rc = PDS_ALLOC_FAILED;
                    break;
                }
                t->deleted_count++;
            }
            if (flags == FRAME_LIVE) {
                e->offset = pos;
                if (e->is_deleted) {
                    if (ho_insert(t->hash, key, e) != HT_SUCCESS) { rc = PDS_ALLOC_FAILED; break; }
                    e->is_deleted = REC_NOT_DELETED;
                    t->deleted_count--;
                    t->live_count++;
                }
            } else if (!e->is_deleted) {
                e->is_deleted = REC_DELETED;
                ho_remove(t->hash, key);
                t->live_count--;
                t->deleted_count++;
            }
        }
    }
    free(block);
    return rc;
}

static int read_rec_at(PDS_TableInfo *t, const PDS_NdxInfo *e, void *rec)
{
    int32_t key, flags;
    t->at_end = 0;
    if (pds_fseek(t->data_fp, e->offset, SEEK_SET) != 0) return PDS_FILE_ERROR;
    if (fread(&key, sizeof key, 1, t->data_fp) != 1 ||
        fread(&flags, sizeof flags, 1, t->data_fp) != 1) {
        return PDS_FILE_ERROR;
    }
    if (key != e->key || flags != FRAME_LIVE) return PDS_CORRUPT;  /* index points at the wrong frame */
    if (fread(rec, (size_t)t->rec_size, 1, t->data_fp) != 1) return PDS_FILE_ERROR;
    return PDS_SUCCESS;
}

/* Appends one frame at end_offset. Back-to-back appends skip the seek, so
 * stdio can batch the writes instead of flushing on every record. */
static int append_frame(PDS_TableInfo *t, int32_t key, int32_t flags, const void *rec,
                        int64_t *offset)
{
    int64_t off = t->end_offset;
    if (!t->at_end && pds_fseek(t->data_fp, off, SEEK_SET) != 0) return PDS_FILE_ERROR;
    t->at_end = 1;
    t->end_offset += frame_size(t);    /* even on failure: skip a half-written frame */
    if (fwrite(&key, sizeof key, 1, t->data_fp) != 1 ||
        fwrite(&flags, sizeof flags, 1, t->data_fp) != 1 ||
        fwrite(rec, (size_t)t->rec_size, 1, t->data_fp) != 1) {
        t->at_end = 0;
        return PDS_FILE_ERROR;
    }
    if (offset) *offset = off;
    return PDS_SUCCESS;
}

static int sync_after_write(PDS_TableInfo *t)
{
    if (g_sync_mode != PDS_SYNC_ALWAYS) return PDS_SUCCESS;
    return sync_fp(t->data_fp) == 0 ? PDS_SUCCESS : PDS_FILE_ERROR;
}

/* ======================================================================== */
/*  Database                                                                */
/* ======================================================================== */

int pds_create_db(const char *db_name)
{
    PDS_DBInfo empty;
    char path[PATH_LEN];

    if (g_db.status == PDS_DB_OPEN) return PDS_DB_ALREADY_OPEN;
    if (!valid_name(db_name)) return PDS_INVALID_ARG;
    if (PDS_MKDIR(db_name) != 0) return PDS_DB_EXISTS;

    memset(&empty, 0, sizeof empty);
    snprintf(path, sizeof path, "%s/%s.info", db_name, db_name);
    return write_info(path, &empty);
}

int pds_open_db(const char *db_name)
{
    char path[PATH_LEN];
    int rc;

    if (g_db.status == PDS_DB_OPEN) return PDS_DB_ALREADY_OPEN;
    if (!valid_name(db_name)) return PDS_INVALID_ARG;
    if (PDS_CHDIR(db_name) != 0) return PDS_FILE_ERROR;

    memset(&g_db, 0, sizeof g_db);
    snprintf(path, sizeof path, "%s.info", db_name);
    rc = read_info(path, &g_db);
    if (rc != PDS_SUCCESS) {
        memset(&g_db, 0, sizeof g_db);
        PDS_CHDIR("..");
        return rc;
    }
    snprintf(g_db.name, sizeof g_db.name, "%s", db_name);
    g_db.status = PDS_DB_OPEN;
    return PDS_SUCCESS;
}

int pds_close_db(void)
{
    int i, rc = PDS_SUCCESS, r;

    if (g_db.status != PDS_DB_OPEN) return PDS_DB_NOT_OPEN;

    for (i = 0; i < g_db.num_tables; i++) {
        if (g_db.tables[i].status == PDS_TABLE_OPEN) {
            r = pds_close_table(g_db.tables[i].name);
            if (r != PDS_SUCCESS && rc == PDS_SUCCESS) rc = r;
        }
    }
    r = save_catalog();
    if (r != PDS_SUCCESS && rc == PDS_SUCCESS) rc = r;

    PDS_CHDIR("..");
    memset(&g_db, 0, sizeof g_db);
    return rc;
}

int pds_drop_db(const char *db_name)
{
    PDS_DBInfo info;
    char path[PATH_LEN];
    int i;

    if (!valid_name(db_name)) return PDS_INVALID_ARG;
    if (g_db.status == PDS_DB_OPEN) return PDS_DB_ALREADY_OPEN;

    memset(&info, 0, sizeof info);
    snprintf(path, sizeof path, "%s/%s.info", db_name, db_name);
    if (read_info(path, &info) == PDS_SUCCESS) {
        for (i = 0; i < info.num_tables; i++) {
            const char *t = info.tables[i].name;
            snprintf(path, sizeof path, "%s/%s.dat", db_name, t);     remove(path);
            snprintf(path, sizeof path, "%s/%s.ndx", db_name, t);     remove(path);
            snprintf(path, sizeof path, "%s/%s.ndx.tmp", db_name, t); remove(path);
        }
    }
    snprintf(path, sizeof path, "%s/%s.info", db_name, db_name);
    remove(path);
    return PDS_RMDIR(db_name) == 0 ? PDS_SUCCESS : PDS_FILE_ERROR;
}

/* ======================================================================== */
/*  Tables                                                                  */
/* ======================================================================== */

int pds_create_table(const char *table, int32_t rec_size)
{
    PDS_TableInfo *t;
    char path[PATH_LEN];
    FILE *fp;
    int rc;

    if (g_db.status != PDS_DB_OPEN) return PDS_DB_NOT_OPEN;
    if (!valid_name(table) || rec_size <= 0) return PDS_INVALID_ARG;
    if (find_table(table)) return PDS_TABLE_EXISTS;
    if (g_db.num_tables >= PDS_MAX_TABLES) return PDS_TOO_MANY_TABLES;

    snprintf(path, sizeof path, "%s.dat", table);
    fp = fopen(path, "wb");
    if (!fp) return PDS_FILE_ERROR;
    fclose(fp);

    rc = write_ndx_file(table, NULL, 0, 0);
    if (rc != PDS_SUCCESS) return rc;

    t = &g_db.tables[g_db.num_tables++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", table);
    t->rec_size = rec_size;
    t->status = PDS_TABLE_CLOSED;
    return save_catalog();
}

int pds_open_table(const char *table)
{
    PDS_TableInfo *t;
    char path[PATH_LEN];
    int rc, reason = PDS_RECOVERY_NONE;

    if (g_db.status != PDS_DB_OPEN) return PDS_DB_NOT_OPEN;
    t = find_table(table);
    if (!t) return PDS_TABLE_NOT_FOUND;
    if (t->status == PDS_TABLE_OPEN) return PDS_TABLE_ALREADY_OPEN;

    snprintf(path, sizeof path, "%s.dat", table);
    t->data_fp = fopen(path, "rb+");
    if (!t->data_fp) return PDS_FILE_ERROR;
    /* Keep stdio's default (small) buffer: most access is random fseek +
     * fread of one frame, and every seek discards whatever was buffered.
     * Sequential scans do their own large block reads instead. */

    t->bst_root = NULL;
    t->num_secondary = 0;
    t->live_count = t->deleted_count = 0;
    t->scratch = malloc((size_t)t->rec_size);
    t->hash = ho_create(1024, 0.7);
    if (!t->scratch || !t->hash) {
        rc = PDS_ALLOC_FAILED;
        goto fail;
    }

    /* Next append goes at the end, rounded up to a whole frame so that a
     * torn write from an earlier crash can't misalign every later frame. */
    if (pds_fseek(t->data_fp, 0, SEEK_END) != 0 || (t->end_offset = pds_ftell(t->data_fp)) < 0) {
        rc = PDS_FILE_ERROR;
        goto fail;
    }
    if (t->end_offset % frame_size(t) != 0)
        t->end_offset += frame_size(t) - t->end_offset % frame_size(t);
    t->at_end = 0;

    rc = load_ndx(t, &reason);
    if (rc == PDS_ALLOC_FAILED) goto fail;
    if (rc != PDS_SUCCESS) {
        /* .ndx missing, corrupt or stale: rebuild from the data file. */
        release_indexes(t);
        t->hash = ho_create(1024, 0.7);
        if (!t->hash) { rc = PDS_ALLOC_FAILED; goto fail; }
        rc = rebuild_from_dat(t);
        if (rc != PDS_SUCCESS) goto fail;
        /* Save the repaired index now, so the next open is a normal one.
         * If this fails the close will simply try again. */
        write_ndx_file(t->name, t->bst_root, (int32_t)(t->live_count + t->deleted_count),
                       t->end_offset);
    }
    t->recovery = reason;
    t->status = PDS_TABLE_OPEN;
    return PDS_SUCCESS;

fail:
    release_table(t);
    fclose(t->data_fp);
    t->data_fp = NULL;
    return rc;
}

int pds_close_table(const char *table)
{
    PDS_TableInfo *t;
    int rc = get_open_table(table, &t);
    if (rc != PDS_SUCCESS) return rc;

    /* Data first, then the index that describes it. */
    if (g_sync_mode != PDS_SYNC_NONE ? sync_fp(t->data_fp) != 0 : fflush(t->data_fp) != 0)
        return PDS_FILE_ERROR;

    /* If the index can't be written the table stays open, so the caller can
     * retry instead of losing the in-memory index. */
    rc = write_ndx_file(t->name, t->bst_root, (int32_t)(t->live_count + t->deleted_count),
                        t->end_offset);
    if (rc != PDS_SUCCESS) return rc;

    if (fclose(t->data_fp) != 0) rc = PDS_FILE_ERROR;
    t->data_fp = NULL;
    release_table(t);
    t->status = PDS_TABLE_CLOSED;
    return rc;
}

int pds_simulate_crash(const char *table)
{
    PDS_TableInfo *t;
    int rc = get_open_table(table, &t);
    if (rc != PDS_SUCCESS) return rc;

    fclose(t->data_fp);           /* no fsync, no .ndx */
    t->data_fp = NULL;
    release_table(t);
    t->status = PDS_TABLE_CLOSED;
    return PDS_SUCCESS;
}

/* ======================================================================== */
/*  CRUD                                                                    */
/* ======================================================================== */

int pds_put_rec(const char *table, int32_t key, const void *rec)
{
    PDS_TableInfo *t;
    BSTNode *node;
    PDS_NdxInfo *e;
    int64_t off;
    int i, rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!rec) return PDS_INVALID_ARG;

    node = bst_search(t->bst_root, key);
    e = node ? node->data : NULL;
    if (e && !e->is_deleted) return PDS_DUPLICATE_KEY;

    rc = append_frame(t, key, FRAME_LIVE, rec, &off);
    if (rc != PDS_SUCCESS) return rc;

    if (e) {
        /* Key was deleted before: revive its index entry. */
        e->offset = off;
        e->is_deleted = REC_NOT_DELETED;
        if (ho_insert(t->hash, key, e) != HT_SUCCESS) {
            e->is_deleted = REC_DELETED;
            return PDS_ALLOC_FAILED;
        }
        t->deleted_count--;
    } else {
        e = malloc(sizeof *e);
        if (!e) return PDS_ALLOC_FAILED;
        e->key = key;
        e->offset = off;
        e->is_deleted = REC_NOT_DELETED;
        if (bst_add_node(&t->bst_root, key, e) != BST_SUCCESS) {
            free(e);
            return PDS_ALLOC_FAILED;
        }
        if (ho_insert(t->hash, key, e) != HT_SUCCESS) {
            /* keep both indexes consistent: entry exists, but as deleted */
            e->is_deleted = REC_DELETED;
            t->deleted_count++;
            return PDS_ALLOC_FAILED;
        }
    }
    t->live_count++;

    for (i = 0; i < t->num_secondary; i++) {
        SecIndex *s = t->secondary[i];
        if (si_add(s, s->key_fn(rec), key) != SI_SUCCESS) return PDS_ALLOC_FAILED;
    }
    return sync_after_write(t);
}

int pds_get_rec(const char *table, int32_t key, void *rec)
{
    PDS_TableInfo *t;
    PDS_NdxInfo *e;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!rec) return PDS_INVALID_ARG;
    e = ho_find(t->hash, key);
    if (!e) return PDS_REC_NOT_FOUND;
    return read_rec_at(t, e, rec);
}

int pds_get_rec_via_bst(const char *table, int32_t key, void *rec)
{
    PDS_TableInfo *t;
    BSTNode *node;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!rec) return PDS_INVALID_ARG;
    node = bst_search(t->bst_root, key);
    if (!node || ((PDS_NdxInfo *)node->data)->is_deleted) return PDS_REC_NOT_FOUND;
    return read_rec_at(t, node->data, rec);
}

int pds_update_rec(const char *table, int32_t key, const void *rec)
{
    PDS_TableInfo *t;
    PDS_NdxInfo *e;
    int i, rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!rec) return PDS_INVALID_ARG;
    e = ho_find(t->hash, key);
    if (!e) return PDS_REC_NOT_FOUND;

    /* Secondary keys may change: keep the old version to compare. */
    if (t->num_secondary > 0) {
        rc = read_rec_at(t, e, t->scratch);
        if (rc != PDS_SUCCESS) return rc;
    }

    /* Records are fixed-size, so an update overwrites the payload in place. */
    t->at_end = 0;
    if (pds_fseek(t->data_fp, e->offset + FRAME_HEADER, SEEK_SET) != 0 ||
        fwrite(rec, (size_t)t->rec_size, 1, t->data_fp) != 1) {
        return PDS_FILE_ERROR;
    }

    for (i = 0; i < t->num_secondary; i++) {
        SecIndex *s = t->secondary[i];
        int64_t old_key = s->key_fn(t->scratch), new_key = s->key_fn(rec);
        if (old_key != new_key) {
            si_remove(s, old_key, key);
            if (si_add(s, new_key, key) != SI_SUCCESS) return PDS_ALLOC_FAILED;
        }
    }
    return sync_after_write(t);
}

int pds_delete_rec(const char *table, int32_t key)
{
    PDS_TableInfo *t;
    PDS_NdxInfo *e;
    int64_t old_keys[PDS_MAX_SECONDARY];
    int i, rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    e = ho_find(t->hash, key);
    if (!e) return PDS_REC_NOT_FOUND;

    if (t->num_secondary > 0) {
        rc = read_rec_at(t, e, t->scratch);
        if (rc != PDS_SUCCESS) return rc;
        for (i = 0; i < t->num_secondary; i++)
            old_keys[i] = t->secondary[i]->key_fn(t->scratch);
    }

    /* Record the delete in .dat itself, so it survives a crash even if the
     * .ndx is never written. The payload of a tombstone is all zeros. */
    memset(t->scratch, 0, (size_t)t->rec_size);
    rc = append_frame(t, key, FRAME_TOMBSTONE, t->scratch, NULL);
    if (rc != PDS_SUCCESS) return rc;

    /* The AVL tree keeps the entry flagged as deleted; the hash drops it. */
    e->is_deleted = REC_DELETED;
    ho_remove(t->hash, key);
    t->live_count--;
    t->deleted_count++;
    for (i = 0; i < t->num_secondary; i++) si_remove(t->secondary[i], old_keys[i], key);
    return sync_after_write(t);
}

/* ======================================================================== */
/*  Scans                                                                   */
/* ======================================================================== */

int pds_get_rec_by_field(const char *table, const void *query, pds_matcher_fn matcher,
                         pds_visit_fn visit, void *ctx,
                         long *match_count, long *comp_count)
{
    PDS_TableInfo *t;
    unsigned char *block;
    void *rec;
    size_t frame, per_block, got, i;
    int64_t pos = 0;
    long matches = 0, comps = 0;
    int stop = 0;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!matcher) return PDS_INVALID_ARG;

    /* Sequential pass over the data file, many frames per fread. */
    frame = (size_t)frame_size(t);
    per_block = DATA_BUF / frame ? DATA_BUF / frame : 1;
    block = malloc(per_block * frame);
    rec = malloc((size_t)t->rec_size);     /* aligned copy for the matcher */
    if (!block || !rec) {
        free(block);
        free(rec);
        return PDS_ALLOC_FAILED;
    }
    t->at_end = 0;
    if (pds_fseek(t->data_fp, 0, SEEK_SET) != 0) {
        free(block);
        free(rec);
        return PDS_FILE_ERROR;
    }

    while (!stop && (got = fread(block, frame, per_block, t->data_fp)) > 0) {
        for (i = 0; i < got; i++, pos += (int64_t)frame) {
            const unsigned char *f = block + i * frame;
            const PDS_NdxInfo *e;
            int32_t key;
            int m;

            memcpy(&key, f, sizeof key);
            /* A frame is the current version of a live record only if the
             * index points exactly at it - this skips tombstones, deleted
             * records and older copies of re-inserted keys. */
            e = ho_find(t->hash, key);
            if (!e || e->offset != pos) continue;

            memcpy(rec, f + FRAME_HEADER, (size_t)t->rec_size);
            m = matcher(rec, query);
            comps++;
            if (m == 0) {
                matches++;
                if (visit && visit(key, rec, ctx)) { stop = 1; break; }
            } else if (m > 1) {
                rc = PDS_MATCHER_ERROR;
                stop = 1;
                break;
            }
        }
    }
    free(block);
    free(rec);
    if (match_count) *match_count = matches;
    if (comp_count) *comp_count = comps;
    return rc;
}

int pds_get_rec_by_scan(const char *table, int32_t key, void *rec, long *comp_count)
{
    PDS_TableInfo *t;
    unsigned char *block;
    size_t frame, per_block, got, i;
    int64_t pos = 0;
    long comps = 0;
    int found = 0;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!rec) return PDS_INVALID_ARG;

    frame = (size_t)frame_size(t);
    per_block = DATA_BUF / frame ? DATA_BUF / frame : 1;
    block = malloc(per_block * frame);
    if (!block) return PDS_ALLOC_FAILED;
    t->at_end = 0;
    if (pds_fseek(t->data_fp, 0, SEEK_SET) != 0) {
        free(block);
        return PDS_FILE_ERROR;
    }

    while (!found && (got = fread(block, frame, per_block, t->data_fp)) > 0) {
        for (i = 0; i < got; i++, pos += (int64_t)frame) {
            const unsigned char *f = block + i * frame;
            const PDS_NdxInfo *e;
            int32_t k;

            memcpy(&k, f, sizeof k);
            comps++;
            if (k != key) continue;
            /* Same key, but is this frame the live, current version? */
            e = ho_find(t->hash, key);
            if (e && e->offset == pos) {
                memcpy(rec, f + FRAME_HEADER, (size_t)t->rec_size);
                found = 1;
                break;
            }
        }
    }
    free(block);
    if (comp_count) *comp_count = comps;
    return found ? PDS_SUCCESS : PDS_REC_NOT_FOUND;
}

int pds_get_rec_at(const char *table, const PDS_NdxInfo *entry, void *rec)
{
    PDS_TableInfo *t;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!entry || !rec) return PDS_INVALID_ARG;
    if (entry->is_deleted) return PDS_REC_NOT_FOUND;
    return read_rec_at(t, entry, rec);
}

typedef struct {
    PDS_TableInfo *t;
    pds_visit_fn visit;
    void *ctx;
    void *buf;
    int rc;
    long count;
} RangeCtx;

static int range_cb(BSTNode *node, void *p)
{
    RangeCtx *c = p;
    const PDS_NdxInfo *e = node->data;
    if (e->is_deleted) return 0;
    c->rc = read_rec_at(c->t, e, c->buf);
    if (c->rc != PDS_SUCCESS) return 1;
    c->count++;
    return c->visit ? c->visit(e->key, c->buf, c->ctx) : 0;
}

int pds_scan_range(const char *table, int32_t lo, int32_t hi,
                   pds_visit_fn visit, void *ctx, long *count)
{
    RangeCtx c;
    int rc = get_open_table(table, &c.t);

    if (rc != PDS_SUCCESS) return rc;
    c.visit = visit;
    c.ctx = ctx;
    c.rc = PDS_SUCCESS;
    c.count = 0;
    c.buf = malloc((size_t)c.t->rec_size);
    if (!c.buf) return PDS_ALLOC_FAILED;

    if (bst_range(c.t->bst_root, lo, hi, range_cb, &c) < 0) c.rc = PDS_ALLOC_FAILED;
    free(c.buf);
    if (count) *count = c.count;
    return c.rc;
}

int pds_max_key(const char *table, int32_t *key)
{
    PDS_TableInfo *t;
    BSTNode *max;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    max = bst_max(t->bst_root);
    if (!max) return PDS_REC_NOT_FOUND;
    if (key) *key = max->key;
    return PDS_SUCCESS;
}

int pds_table_stats(const char *table, PDS_TableStats *s)
{
    PDS_TableInfo *t;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!s) return PDS_INVALID_ARG;
    memset(s, 0, sizeof *s);
    s->live = t->live_count;
    s->deleted = t->deleted_count;
    t->at_end = 0;
    if (pds_fseek(t->data_fp, 0, SEEK_END) == 0) s->data_bytes = pds_ftell(t->data_fp);
    s->hash_cap = t->hash->cap;
    s->hash_used = t->hash->used;
    s->hash_tombs = t->hash->tombs;
    bst_stats(t->bst_root, NULL, &s->bst_height, &s->bst_avg_depth);
    s->num_secondary = t->num_secondary;
    s->recovery = t->recovery;
    return PDS_SUCCESS;
}

/* ======================================================================== */
/*  Secondary indexes                                                       */
/* ======================================================================== */

static SecIndex *find_secondary(PDS_TableInfo *t, const char *name)
{
    int i;
    for (i = 0; i < t->num_secondary; i++) {
        if (strcmp(t->secondary[i]->name, name) == 0) return t->secondary[i];
    }
    return NULL;
}

typedef struct {
    SecIndex *s;
    int rc;
} BuildCtx;

static int build_cb(int32_t key, const void *rec, void *p)
{
    BuildCtx *c = p;
    if (si_add(c->s, c->s->key_fn(rec), key) != SI_SUCCESS) {
        c->rc = PDS_ALLOC_FAILED;
        return 1;
    }
    return 0;
}

int pds_create_secondary_index(const char *table, const char *index_name, si_key_fn key_fn)
{
    PDS_TableInfo *t;
    BuildCtx c;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!index_name || !key_fn || strlen(index_name) >= SI_NAME_LEN) return PDS_INVALID_ARG;
    if (find_secondary(t, index_name)) return PDS_INDEX_EXISTS;
    if (t->num_secondary >= PDS_MAX_SECONDARY) return PDS_TOO_MANY_INDEXES;

    c.s = si_create(index_name, key_fn);
    if (!c.s) return PDS_ALLOC_FAILED;
    c.rc = PDS_SUCCESS;

    /* One sequential pass over the live records. */
    rc = pds_get_rec_by_field(table, NULL, match_all, build_cb, &c, NULL, NULL);
    if (rc == PDS_SUCCESS) rc = c.rc;
    if (rc != PDS_SUCCESS) {
        si_free(c.s);
        return rc;
    }
    t->secondary[t->num_secondary++] = c.s;
    return PDS_SUCCESS;
}

int pds_get_by_secondary(const char *table, const char *index_name, int64_t skey,
                         pds_visit_fn visit, void *ctx, long *match_count, long *fetched)
{
    PDS_TableInfo *t;
    SecIndex *s;
    const int32_t *pks;
    int32_t *keys = NULL;
    void *buf;
    size_t n, i;
    long matches = 0, reads = 0;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!index_name) return PDS_INVALID_ARG;
    s = find_secondary(t, index_name);
    if (!s) return PDS_INDEX_NOT_FOUND;

    pks = si_find(s, skey, &n);
    buf = malloc((size_t)t->rec_size);
    if (n > 0) keys = malloc(n * sizeof *keys);   /* visit may modify the table */
    if (!buf || (n > 0 && !keys)) {
        free(buf);
        free(keys);
        return PDS_ALLOC_FAILED;
    }
    if (n > 0) memcpy(keys, pks, n * sizeof *keys);

    for (i = 0; i < n; i++) {
        const PDS_NdxInfo *e = ho_find(t->hash, keys[i]);
        if (!e) continue;
        rc = read_rec_at(t, e, buf);
        if (rc != PDS_SUCCESS) break;
        reads++;
        if (s->key_fn(buf) != skey) continue;     /* defensive: index out of date */
        matches++;
        if (visit && visit(keys[i], buf, ctx)) break;
    }
    free(buf);
    free(keys);
    if (match_count) *match_count = matches;
    if (fetched) *fetched = reads;
    return rc;
}

/* ======================================================================== */
/*  Text (CSV) export / import                                              */
/* ======================================================================== */

#define TEXT_LINE 1024

typedef struct {
    FILE *fp;
    pds_to_text_fn to_text;
    char line[TEXT_LINE];
    int rc;
} ExportCtx;

static int export_cb(int32_t key, const void *rec, void *p)
{
    ExportCtx *c = p;
    (void)key;
    if (c->to_text(rec, c->line, sizeof c->line) != 0) {
        c->rc = PDS_INVALID_ARG;        /* record can't be represented as a line */
        return 1;
    }
    if (fprintf(c->fp, "%s\n", c->line) < 0) {
        c->rc = PDS_FILE_ERROR;
        return 1;
    }
    return 0;
}

int pds_export_text(const char *table, const char *path, const char *header,
                    pds_to_text_fn to_text, long *count)
{
    PDS_TableInfo *t;
    ExportCtx c;
    long n = 0;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!path || !to_text) return PDS_INVALID_ARG;

    /* Text mode ("w", not "wb"): on Windows every '\n' becomes "\r\n". */
    c.fp = fopen(path, "w");
    if (!c.fp) return PDS_FILE_ERROR;
    c.to_text = to_text;
    c.rc = PDS_SUCCESS;

    if (header && fprintf(c.fp, "%s\n", header) < 0) c.rc = PDS_FILE_ERROR;
    if (c.rc == PDS_SUCCESS) {
        /* Storage order: one sequential pass over .dat. */
        rc = pds_get_rec_by_field(table, NULL, match_all, export_cb, &c, &n, NULL);
        if (rc != PDS_SUCCESS && c.rc == PDS_SUCCESS) c.rc = rc;
    }
    if (fclose(c.fp) != 0 && c.rc == PDS_SUCCESS) c.rc = PDS_FILE_ERROR;
    if (c.rc != PDS_SUCCESS) {
        remove(path);
        return c.rc;
    }
    if (count) *count = n;
    return PDS_SUCCESS;
}

int pds_import_text(const char *table, const char *path, int skip_header,
                    pds_from_text_fn from_text, long *imported, long *skipped)
{
    PDS_TableInfo *t;
    FILE *fp;
    char line[TEXT_LINE];
    void *rec;
    long n_ok = 0, n_skip = 0;
    int first = 1;
    int rc = get_open_table(table, &t);

    if (rc != PDS_SUCCESS) return rc;
    if (!path || !from_text) return PDS_INVALID_ARG;

    fp = fopen(path, "r");
    if (!fp) return PDS_FILE_ERROR;
    rec = malloc((size_t)t->rec_size);
    if (!rec) {
        fclose(fp);
        return PDS_ALLOC_FAILED;
    }

    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        int32_t key;

        if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
            /* line longer than the buffer: discard the rest of it */
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {}
            n_skip++;
            first = 0;
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (first) {
            first = 0;
            if (skip_header) continue;
        }
        if (line[0] == '\0') continue;

        memset(rec, 0, (size_t)t->rec_size);
        if (from_text(line, rec, &key) != 0) {
            n_skip++;
            continue;
        }
        rc = pds_put_rec(table, key, rec);
        if (rc == PDS_SUCCESS) {
            n_ok++;
        } else if (rc == PDS_DUPLICATE_KEY) {
            n_skip++;
        } else {
            break;
        }
        rc = PDS_SUCCESS;
    }
    if (rc == PDS_SUCCESS && ferror(fp)) rc = PDS_FILE_ERROR;
    fclose(fp);
    free(rec);
    if (imported) *imported = n_ok;
    if (skipped) *skipped = n_skip;
    return rc;
}

/* ======================================================================== */

PDS_TableInfo *pds_table_info(const char *table)
{
    PDS_TableInfo *t;
    return get_open_table(table, &t) == PDS_SUCCESS ? t : NULL;
}

const PDS_DBInfo *pds_db_info(void)
{
    return &g_db;
}

const char *pds_recovery_name(int recovery)
{
    switch (recovery) {
    case PDS_RECOVERY_NONE:        return "loaded from .ndx";
    case PDS_RECOVERY_NDX_MISSING: return "rebuilt from .dat (.ndx missing)";
    case PDS_RECOVERY_NDX_CORRUPT: return "rebuilt from .dat (.ndx unreadable)";
    case PDS_RECOVERY_NDX_STALE:   return "rebuilt from .dat (.ndx out of date)";
    default:                       return "unknown";
    }
}

const char *pds_strerror(int code)
{
    switch (code) {
    case PDS_SUCCESS:            return "success";
    case PDS_FILE_ERROR:         return "file I/O error";
    case PDS_ALLOC_FAILED:       return "out of memory";
    case PDS_REC_NOT_FOUND:      return "record not found";
    case PDS_DUPLICATE_KEY:      return "duplicate key";
    case PDS_DB_NOT_OPEN:        return "database is not open";
    case PDS_DB_ALREADY_OPEN:    return "a database is already open";
    case PDS_DB_EXISTS:          return "database already exists (or folder could not be created)";
    case PDS_TABLE_NOT_FOUND:    return "table not found";
    case PDS_TABLE_NOT_OPEN:     return "table is not open";
    case PDS_TABLE_ALREADY_OPEN: return "table is already open";
    case PDS_TABLE_EXISTS:       return "table already exists";
    case PDS_TOO_MANY_TABLES:    return "too many tables";
    case PDS_INVALID_ARG:        return "invalid argument";
    case PDS_MATCHER_ERROR:      return "matcher reported an error";
    case PDS_CORRUPT:            return "index or data file is corrupt";
    case PDS_OLD_FORMAT:         return "database was created by an older SkyBook version (incompatible file format)";
    case PDS_INDEX_EXISTS:       return "secondary index already exists";
    case PDS_INDEX_NOT_FOUND:    return "secondary index not found";
    case PDS_TOO_MANY_INDEXES:   return "too many secondary indexes";
    default:                     return "unknown error";
    }
}
