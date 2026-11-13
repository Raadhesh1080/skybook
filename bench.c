/*
 * bench.c - SkyBook benchmark suite.
 *
 * Part A runs the real engine on disk (insert, lookups, scans, updates,
 * deletes, close/reopen with full verification). Part B isolates the
 * in-memory index structures to compare collision strategies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "array_index.h"
#include "bench.h"
#include "hash_chain.h"
#include "pds.h"
#include "record.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

#define BENCH_DB    "skybench"
#define BENCH_TABLE "flights"

static volatile uintptr_t g_sink;   /* defeats dead-code elimination */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return g_rng = x;
}

static void shuffle(int32_t *a, long n)
{
    long i;
    for (i = n - 1; i > 0; i--) {
        long j = (long)(rng_next() % (uint64_t)(i + 1));
        int32_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static const char *const AIRPORTS[] = {
    "DEL", "BOM", "BLR", "HYD", "MAA", "CCU", "GOI", "PNQ", "AMD", "COK"
};

/* Deterministic: the same id always produces the same flight, so the
 * verification pass can regenerate the expected record. */
static void gen_flight(int32_t id, Flight *f)
{
    int s = id % 10;
    int d = (s + 1 + (id / 10) % 9) % 10;
    memset(f, 0, sizeof *f);
    f->flight_id = id;
    snprintf(f->flight_no, sizeof f->flight_no, "SB%04d", (int)(id % 10000));
    memcpy(f->source, AIRPORTS[s], CODE_LEN);
    memcpy(f->dest, AIRPORTS[d], CODE_LEN);
    snprintf(f->date, sizeof f->date, "2026-12-%02d", (int)(1 + id % 28));
    f->seats_total = 180;
    f->seats_booked = 0;
    f->fare = 3000 + (int32_t)((id * 37) % 9000);
}

#define TRY(expr)                                                              \
    do {                                                                       \
        int rc_ = (expr);                                                      \
        if (rc_ != PDS_SUCCESS) {                                              \
            fprintf(stderr, "  %s failed: %s\n", #expr, pds_strerror(rc_));    \
            goto fail;                                                         \
        }                                                                      \
    } while (0)

static int fill_chain_cb(BSTNode *node, void *ctx)
{
    const PDS_NdxInfo *e = node->data;
    if (!e->is_deleted && hc_insert((HashChain *)ctx, e->key, node->data) != HT_SUCCESS) return 1;
    return 0;
}

/* In-order walk yields ascending keys, so the array can be bulk-appended. */
static int fill_array_cb(BSTNode *node, void *ctx)
{
    const PDS_NdxInfo *e = node->data;
    if (!e->is_deleted && ai_append((ArrayIndex *)ctx, e->key, node->data) != AI_SUCCESS) return 1;
    return 0;
}

typedef struct {
    FILE *fp;
    char line[256];
} CsvWriter;

static int write_csv_cb(int32_t key, const void *rec, void *p)
{
    CsvWriter *w = p;
    (void)key;
    if (flight_to_csv(rec, w->line, sizeof w->line) != 0) return 1;
    return fprintf(w->fp, "%s\n", w->line) < 0;
}

/* ======================================================================== */
/*  Part A - the engine on disk                                             */
/* ======================================================================== */

static int bench_engine(long n)
{
    const long L = 1000000;     /* in-memory lookups per index  */
    const long L_LIN = 2000;    /* linear array scans (O(n) each) */
    const long R = 200000;      /* full record fetches           */
    const long R_SCAN = 200;    /* no-index fetches (O(n) disk reads each) */
    int32_t *ids = NULL, *hits = NULL, *misses = NULL;
    unsigned char *deleted = NULL, *updated = NULL;
    HashChain *chain = NULL;
    ArrayIndex *array = NULL;
    double ns_ai_lin, ns_ai_hit, ns_ai_miss, ai_cmp_lin, ai_cmp_hit, ai_cmp_miss;
    PDS_TableInfo *t;
    PDS_TableStats st;
    Flight f, expect;
    RouteQuery route;
    long i, matches, comps, count, n_del, n_upd, failures = 0;
    double t0, t1, ns_ins, ns_ho_hit, ns_ho_miss, ns_hc_hit, ns_hc_miss, ns_bst_hit, ns_bst_miss;
    double ho_probes_hit, ho_probes_miss, hc_probes_hit, hc_probes_miss, ms_open_normal = 0;
    uintptr_t acc = 0;
    int ok = 0;

    ids = malloc((size_t)n * sizeof *ids);
    hits = malloc((size_t)L * sizeof *hits);
    misses = malloc((size_t)L * sizeof *misses);
    deleted = calloc((size_t)(n + n / 100) + 1, 1);    /* A9 adds n/100 new IDs */
    updated = calloc((size_t)(n + n / 100) + 1, 1);
    if (!ids || !hits || !misses || !deleted || !updated) {
        fprintf(stderr, "  out of memory\n");
        goto fail;
    }
    for (i = 0; i < n; i++) ids[i] = (int32_t)(i + 1);
    shuffle(ids, n);
    for (i = 0; i < L; i++) {
        hits[i] = (int32_t)(1 + rng_next() % (uint64_t)n);
        misses[i] = (int32_t)(n + 1 + rng_next() % (uint64_t)n);
    }

    pds_drop_db(BENCH_DB);
    TRY(pds_create_db(BENCH_DB));
    TRY(pds_open_db(BENCH_DB));
    TRY(pds_create_table(BENCH_TABLE, sizeof(Flight)));
    TRY(pds_open_table(BENCH_TABLE));
    t = pds_table_info(BENCH_TABLE);

    /* ---- A1. Insert ---------------------------------------------------- */
    t0 = now_ns();
    for (i = 0; i < n; i++) {
        gen_flight(ids[i], &f);
        TRY(pds_put_rec(BENCH_TABLE, f.flight_id, &f));
    }
    t1 = now_ns();
    ns_ins = (t1 - t0) / n;

    TRY(pds_table_stats(BENCH_TABLE, &st));
    {
        /* Same flights again, but with IDs arriving in sorted order - the
         * case that turned the old unbalanced BST into a chain. */
        PDS_TableStats sq;
        double t2, t3;
        TRY(pds_create_table("flights_sorted", sizeof(Flight)));
        TRY(pds_open_table("flights_sorted"));
        t2 = now_ns();
        for (i = 1; i <= n; i++) {
            gen_flight((int32_t)i, &f);
            TRY(pds_put_rec("flights_sorted", f.flight_id, &f));
        }
        t3 = now_ns();
        TRY(pds_table_stats("flights_sorted", &sq));
        TRY(pds_close_table("flights_sorted"));

        printf("### A1. Insert %ld flights (disk append + hash + AVL)\n\n", n);
        printf("| Key order | Total | Per insert | AVL height | Avg depth |\n|---|---|---|---|---|\n");
        printf("| Random | %.1f ms | %.0f ns | %d | %.2f |\n", (t1 - t0) / 1e6, ns_ins,
               st.bst_height, st.bst_avg_depth);
        printf("| Sorted (1, 2, 3, ...) | %.1f ms | %.0f ns | %d | %.2f |\n\n", (t3 - t2) / 1e6,
               (t3 - t2) / n, sq.bst_height, sq.bst_avg_depth);
        printf("Record frame on disk: %d bytes (4-byte key + 4-byte flags + %d-byte Flight). "
               "Hash index: cap %zu, load %.2f.\n\n", (int)(FRAME_HEADER + sizeof(Flight)),
               (int)sizeof(Flight), st.hash_cap, (double)st.hash_used / st.hash_cap);
    }

    /* ---- A2. Index-only lookups --------------------------------------- */
    chain = hc_create(200003);
    if (!chain || bst_inorder(t->bst_root, fill_chain_cb, chain) != 0) {
        fprintf(stderr, "  could not build chained table\n");
        goto fail;
    }

    array = ai_create((size_t)n);
    if (!array || bst_inorder(t->bst_root, fill_array_cb, array) != 0) {
        fprintf(stderr, "  could not build array index\n");
        goto fail;
    }

    array->comparisons = 0;
    t0 = now_ns();
    for (i = 0; i < L_LIN; i++) acc += (uintptr_t)ai_find_linear(array, hits[i]);
    t1 = now_ns();
    ns_ai_lin = (t1 - t0) / L_LIN;
    ai_cmp_lin = (double)array->comparisons / L_LIN;

    array->comparisons = 0;
    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)ai_find(array, hits[i]);
    t1 = now_ns();
    ns_ai_hit = (t1 - t0) / L;
    ai_cmp_hit = (double)array->comparisons / L;

    array->comparisons = 0;
    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)ai_find(array, misses[i]);
    t1 = now_ns();
    ns_ai_miss = (t1 - t0) / L;
    ai_cmp_miss = (double)array->comparisons / L;

    t->hash->probes = 0;
    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)ho_find(t->hash, hits[i]);
    t1 = now_ns();
    ns_ho_hit = (t1 - t0) / L;
    ho_probes_hit = (double)t->hash->probes / L;

    t->hash->probes = 0;
    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)ho_find(t->hash, misses[i]);
    t1 = now_ns();
    ns_ho_miss = (t1 - t0) / L;
    ho_probes_miss = (double)t->hash->probes / L;

    chain->probes = 0;
    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)hc_find(chain, hits[i]);
    t1 = now_ns();
    ns_hc_hit = (t1 - t0) / L;
    hc_probes_hit = (double)chain->probes / L;

    chain->probes = 0;
    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)hc_find(chain, misses[i]);
    t1 = now_ns();
    ns_hc_miss = (t1 - t0) / L;
    hc_probes_miss = (double)chain->probes / L;

    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)bst_search(t->bst_root, hits[i]);
    t1 = now_ns();
    ns_bst_hit = (t1 - t0) / L;

    t0 = now_ns();
    for (i = 0; i < L; i++) acc += (uintptr_t)bst_search(t->bst_root, misses[i]);
    t1 = now_ns();
    ns_bst_miss = (t1 - t0) / L;
    g_sink = acc;

    printf("### A2. In-memory index lookup (%ld random keys each, %ld records)\n\n", L, n);
    printf("| Index | Hit | Miss | Avg probes (hit / miss) |\n|---|---|---|---|\n");
    printf("| Array, linear search (%ld lookups) | %.0f ns | - | %.0f entries / - |\n",
           L_LIN, ns_ai_lin, ai_cmp_lin);
    printf("| Sorted array, binary search | %.1f ns | %.1f ns | %.2f / %.2f entries |\n",
           ns_ai_hit, ns_ai_miss, ai_cmp_hit, ai_cmp_miss);
    printf("| AVL tree (height %d) | %.1f ns | %.1f ns | ~%.1f nodes (avg depth) |\n",
           st.bst_height, ns_bst_hit, ns_bst_miss, st.bst_avg_depth);
    printf("| Separate chaining (200003 buckets, load %.2f) | %.1f ns | %.1f ns | %.2f / %.2f nodes |\n",
           hc_load(chain), ns_hc_hit, ns_hc_miss, hc_probes_hit, hc_probes_miss);
    printf("| Open addressing, linear probing (cap %zu, load %.2f) | **%.1f ns** | **%.1f ns** | %.2f / %.2f slots |\n\n",
           t->hash->cap, ho_load(t->hash), ns_ho_hit, ns_ho_miss, ho_probes_hit, ho_probes_miss);
    printf("Open addressing on hits: **%.0fx** faster than a linear array search, %.1fx faster than binary search, "
           "**%.1fx** faster than the AVL tree, %.2fx faster than chaining.\n\n",
           ns_ai_lin / ns_ho_hit, ns_ai_hit / ns_ho_hit, ns_bst_hit / ns_ho_hit, ns_hc_hit / ns_ho_hit);
    hc_free(chain);
    chain = NULL;

    /* ---- A3. Full record fetch ---------------------------------------- */
    {
        double ns_get_hash, ns_get_bst, ns_get_arr, ns_get_scan;
        long comps_total = 0, c;
        t0 = now_ns();
        for (i = 0; i < R_SCAN; i++) {
            TRY(pds_get_rec_by_scan(BENCH_TABLE, hits[i], &f, &c));
            comps_total += c;
        }
        t1 = now_ns();
        ns_get_scan = (t1 - t0) / R_SCAN;
        t0 = now_ns();
        for (i = 0; i < R; i++) {
            const PDS_NdxInfo *e = ai_find(array, hits[i]);
            if (!e) { fprintf(stderr, "  array lookup failed\n"); goto fail; }
            TRY(pds_get_rec_at(BENCH_TABLE, e, &f));
        }
        t1 = now_ns();
        ns_get_arr = (t1 - t0) / R;
        t0 = now_ns();
        for (i = 0; i < R; i++) TRY(pds_get_rec_via_bst(BENCH_TABLE, hits[i], &f));
        t1 = now_ns();
        ns_get_bst = (t1 - t0) / R;
        t0 = now_ns();
        for (i = 0; i < R; i++) TRY(pds_get_rec(BENCH_TABLE, hits[i], &f));
        t1 = now_ns();
        ns_get_hash = (t1 - t0) / R;
        printf("### A3. Full record fetch by key (random keys)\n\n");
        printf("| Path | Lookups | Per fetch |\n|---|---|---|\n");
        printf("| No index: `pds_get_rec_by_scan` reads `.dat` from the start | %ld | %.2f ms (avg %.0f frames compared) |\n",
               R_SCAN, ns_get_scan / 1e6, (double)comps_total / R_SCAN);
        printf("| Sorted array + `fseek` + `fread` | %ld | %.0f ns |\n", R, ns_get_arr);
        printf("| AVL tree + `fseek` + `fread` (`pds_get_rec_via_bst`) | %ld | %.0f ns |\n", R, ns_get_bst);
        printf("| Hash + `fseek` + `fread` (`pds_get_rec`) | %ld | %.0f ns |\n\n", R, ns_get_hash);
        printf("Any index is **%.0fx** faster than scanning the data file.\n\n", ns_get_scan / ns_get_hash);
    }
    ai_free(array);
    array = NULL;

    /* ---- A4. Field search: full scan vs route secondary index ----------- */
    memcpy(route.source, "DEL", CODE_LEN);
    memcpy(route.dest, "BOM", CODE_LEN);
    {
        const long REPS = 20, ID_REPS = 100000;
        const int64_t rk = route_key("DEL", "BOM");
        long idx_matches = 0, fetched = 0, r;
        double ms_scan, ms_build, ms_idx, ns_ids;
        size_t n_ids = 0;
        SecIndex *si;

        t0 = now_ns();
        for (r = 0; r < REPS; r++)
            TRY(pds_get_rec_by_field(BENCH_TABLE, &route, match_flight_route, NULL, NULL, &matches, &comps));
        ms_scan = (now_ns() - t0) / 1e6 / REPS;

        t0 = now_ns();
        TRY(pds_create_secondary_index(BENCH_TABLE, "route", flight_route_key));
        ms_build = (now_ns() - t0) / 1e6;
        si = t->secondary[0];

        t0 = now_ns();
        for (r = 0; r < ID_REPS; r++) acc += (uintptr_t)si_find(si, rk, &n_ids);
        ns_ids = (now_ns() - t0) / ID_REPS;
        g_sink = acc;

        t0 = now_ns();
        for (r = 0; r < REPS; r++)
            TRY(pds_get_by_secondary(BENCH_TABLE, "route", rk, NULL, NULL, &idx_matches, &fetched));
        ms_idx = (now_ns() - t0) / 1e6 / REPS;
        if (idx_matches != matches || (long)n_ids != matches) failures++;

        printf("### A4. Field search: all DEL -> BOM flights, full scan vs route secondary index\n\n");
        printf("| Method | Matcher comparisons | Records read from disk | Time |\n|---|---|---|---|\n");
        printf("| Full scan (`pds_get_rec_by_field`) | %ld | %ld (sequential) | %.2f ms |\n",
               comps, comps, ms_scan);
        printf("| Route index: IDs only (`si_find`) | 0 | 0 | %.0f ns |\n", ns_ids);
        printf("| Route index + fetch each flight (`pds_get_by_secondary`) | 0 | %ld (random) | %.2f ms |\n\n",
               fetched, ms_idx);
        printf("Both return %ld flights%s. Building the index (one sequential pass over %ld flights) "
               "took %.1f ms; it then stays in sync on every insert, update and delete.\n\n",
               matches, idx_matches == matches ? " (identical)" : " (MISMATCH)", t->live_count, ms_build);
    }

    /* ---- A5. Range scan ------------------------------------------------ */
    {
        int32_t lo = (int32_t)(n / 2), hi = lo + 999;
        t0 = now_ns();
        TRY(pds_scan_range(BENCH_TABLE, lo, hi, NULL, NULL, &count));
        t1 = now_ns();
        printf("### A5. AVL range scan, flight IDs %d..%d\n\n", lo, hi);
        printf("| Metric | Value |\n|---|---|\n");
        printf("| Records returned (in key order) | %ld |\n", count);
        printf("| Time | %.3f ms |\n\n", (t1 - t0) / 1e6);
    }

    /* ---- A6. Updates and deletes -------------------------------------- */
    n_del = n / 10;
    n_upd = n / 10;
    t0 = now_ns();
    for (i = 0; i < n_upd; i++) {               /* ids[] is shuffled: random distinct keys */
        int32_t id = ids[n_del + i];
        TRY(pds_get_rec(BENCH_TABLE, id, &f));
        f.fare += 100;
        TRY(pds_update_rec(BENCH_TABLE, id, &f));
        updated[id] = 1;
    }
    t1 = now_ns();
    {
        double ns_upd = (t1 - t0) / n_upd, ns_del;
        t0 = now_ns();
        for (i = 0; i < n_del; i++) {
            TRY(pds_delete_rec(BENCH_TABLE, ids[i]));
            deleted[ids[i]] = 1;
        }
        t1 = now_ns();
        ns_del = (t1 - t0) / n_del;
        printf("### A6. Mutations\n\n");
        printf("| Operation | Count | Per op |\n|---|---|---|\n");
        printf("| Read-modify-write fare (in-place overwrite) | %ld | %.0f ns |\n", n_upd, ns_upd);
        printf("| Soft delete (tombstone frame appended to `.dat`) | %ld | %.0f ns |\n\n", n_del, ns_del);
        printf("With the route index registered, each update and delete first re-reads the old "
               "record to keep the index in sync.\n\n");
    }

    /* ---- A7. Persistence ---------------------------------------------- */
    TRY(pds_table_stats(BENCH_TABLE, &st));
    {
        double ms_close, ms_open, ms_verify;
        long live_after;
        FILE *fp;
        long long ndx_bytes = -1;

        t0 = now_ns();
        TRY(pds_close_db());
        t1 = now_ns();
        ms_close = (t1 - t0) / 1e6;

        fp = fopen(BENCH_DB "/" BENCH_TABLE ".ndx", "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            ndx_bytes = ftell(fp);
            fclose(fp);
        }

        t0 = now_ns();
        TRY(pds_open_db(BENCH_DB));
        TRY(pds_open_table(BENCH_TABLE));
        t1 = now_ns();
        ms_open = (t1 - t0) / 1e6;
        ms_open_normal = ms_open;
        t = pds_table_info(BENCH_TABLE);
        live_after = pds_table_info(BENCH_TABLE)->live_count;

        t0 = now_ns();
        for (i = 1; i <= n; i++) {
            int rc = pds_get_rec(BENCH_TABLE, (int32_t)i, &f);
            if (deleted[i]) {
                if (rc != PDS_REC_NOT_FOUND) failures++;
                continue;
            }
            gen_flight((int32_t)i, &expect);
            if (updated[i]) expect.fare += 100;
            if (rc != PDS_SUCCESS || memcmp(&f, &expect, sizeof f) != 0) failures++;
        }
        t1 = now_ns();
        ms_verify = (t1 - t0) / 1e6;
        if (live_after != n - n_del) failures++;

        printf("### A7. Persistence: close, reopen, verify every key\n\n");
        printf("| Metric | Value |\n|---|---|\n");
        printf("| `.dat` size | %.2f MB |\n", st.data_bytes / 1048576.0);
        printf("| `.ndx` size | %.2f MB (%ld entries incl. tombstones) |\n",
               ndx_bytes / 1048576.0, st.live + st.deleted);
        printf("| Close (fsync `.dat`, write + fsync `.ndx`) | %.1f ms |\n", ms_close);
        printf("| Reopen (load hash + AVL from `.ndx`) | %.1f ms |\n", ms_open);
        printf("| Verify all %ld keys after reopen | %.1f ms |\n", n, ms_verify);
        printf("| Live records after reopen | %ld (expected %ld) |\n", live_after, n - n_del);
        printf("| Result | **%s** (%ld mismatches) |\n\n", failures ? "FAIL" : "PASS", failures);
    }

    /* ---- A8. CSV round trip through the engine ------------------------- */
    {
        const char *csv = "flights_export.csv";
        long exported = 0, imported = 0, skipped = 0, rt_fail = 0;
        double ms_exp, ms_imp;
        Flight g;

        t0 = now_ns();
        TRY(pds_export_text(BENCH_TABLE, csv, FLIGHT_CSV_HEADER, flight_to_csv, &exported));
        ms_exp = (now_ns() - t0) / 1e6;

        TRY(pds_create_table("flights_copy", sizeof(Flight)));
        TRY(pds_open_table("flights_copy"));
        t0 = now_ns();
        TRY(pds_import_text("flights_copy", csv, 1, flight_from_csv, &imported, &skipped));
        ms_imp = (now_ns() - t0) / 1e6;

        for (i = 1; i <= n; i++) {
            int r1 = pds_get_rec(BENCH_TABLE, (int32_t)i, &f);
            int r2 = pds_get_rec("flights_copy", (int32_t)i, &g);
            if (r1 != r2 || (r1 == PDS_SUCCESS && memcmp(&f, &g, sizeof f) != 0)) rt_fail++;
        }
        if (exported != n - n_del || imported != exported || skipped != 0) rt_fail++;
        failures += rt_fail;
        remove(csv);

        printf("### A8. Text export / import round trip (`pds_export_text` -> `pds_import_text`)\n\n");
        printf("| Metric | Value |\n|---|---|\n");
        printf("| Export %ld live flights to CSV (storage order, one sequential pass) | %.1f ms |\n", exported, ms_exp);
        printf("| Import CSV into a new table (parse + insert) | %.1f ms |\n", ms_imp);
        printf("| Imported / skipped | %ld / %ld |\n", imported, skipped);
        printf("| Every record identical in both tables | **%s** (%ld mismatches) |\n",
               rt_fail ? "FAIL" : "PASS", rt_fail);

        /* A key-sorted CSV - e.g. produced by another tool - is the input
         * that made the previous, unbalanced-BST version take minutes. */
        {
            const char *sorted_csv = "flights_by_key.csv";
            CsvWriter w;
            long n_sorted = 0, imp2 = 0, skip2 = 0;
            double ms_imp2;
            PDS_TableStats ks;

            w.fp = fopen(sorted_csv, "w");
            if (!w.fp) { fprintf(stderr, "  cannot write %s\n", sorted_csv); goto fail; }
            fprintf(w.fp, "%s\n", FLIGHT_CSV_HEADER);
            TRY(pds_scan_range(BENCH_TABLE, INT32_MIN, INT32_MAX, write_csv_cb, &w, &n_sorted));
            fclose(w.fp);

            TRY(pds_create_table("flights_keyorder", sizeof(Flight)));
            TRY(pds_open_table("flights_keyorder"));
            t0 = now_ns();
            TRY(pds_import_text("flights_keyorder", sorted_csv, 1, flight_from_csv, &imp2, &skip2));
            ms_imp2 = (now_ns() - t0) / 1e6;
            TRY(pds_table_stats("flights_keyorder", &ks));
            remove(sorted_csv);
            if (imp2 != n_sorted || skip2 != 0) failures++;

            printf("| Import a **key-sorted** CSV (%ld rows, IDs ascending) | %.1f ms, AVL height %d |\n\n",
                   imp2, ms_imp2, ks.bst_height);
        }
    }

    /* ---- A9. Crash recovery ------------------------------------------- */
    {
        const long K = n / 100;                       /* 1,000 for 100,000 flights */
        const long base = n_del + n_upd;              /* ids[base..] untouched so far */
        long crash_fail = 0, last_id = n + K;
        double ms_rebuild, ms_verify2;
        PDS_TableStats cs;

        for (i = 0; i < K; i++) {
            int32_t id = ids[base + i];
            TRY(pds_delete_rec(BENCH_TABLE, id));
            deleted[id] = 1;
        }
        for (i = 0; i < K; i++) {
            int32_t id = ids[base + K + i];
            TRY(pds_get_rec(BENCH_TABLE, id, &f));
            f.fare += 100;
            TRY(pds_update_rec(BENCH_TABLE, id, &f));
            updated[id] = 1;
        }
        for (i = 1; i <= K; i++) {
            gen_flight((int32_t)(n + i), &f);
            TRY(pds_put_rec(BENCH_TABLE, f.flight_id, &f));
        }

        TRY(pds_simulate_crash(BENCH_TABLE));          /* no .ndx written */
        t0 = now_ns();
        TRY(pds_open_table(BENCH_TABLE));
        ms_rebuild = (now_ns() - t0) / 1e6;
        TRY(pds_table_stats(BENCH_TABLE, &cs));

        t0 = now_ns();
        for (i = 1; i <= last_id; i++) {
            int rc = pds_get_rec(BENCH_TABLE, (int32_t)i, &f);
            if (deleted[i]) {
                if (rc != PDS_REC_NOT_FOUND) crash_fail++;
                continue;
            }
            gen_flight((int32_t)i, &expect);
            if (updated[i]) expect.fare += 100;
            if (rc != PDS_SUCCESS || memcmp(&f, &expect, sizeof f) != 0) crash_fail++;
        }
        ms_verify2 = (now_ns() - t0) / 1e6;
        if (cs.live != n - n_del || cs.recovery != PDS_RECOVERY_NDX_STALE) crash_fail++;
        if (!bst_is_avl(pds_table_info(BENCH_TABLE)->bst_root)) crash_fail++;
        failures += crash_fail;

        printf("### A9. Crash recovery: rebuild the index from `.dat`\n\n");
        printf("After the clean close in A7: %ld deletes, %ld updates and %ld inserts, then "
               "`pds_simulate_crash` (no `.ndx` written).\n\n", K, K, K);
        printf("| Metric | Value |\n|---|---|\n");
        printf("| Detected on open | %s |\n", pds_recovery_name(cs.recovery));
        printf("| Reopen with rebuild (replay %.2f MB of `.dat`) | %.1f ms (normal open from `.ndx`: %.1f ms) |\n",
               cs.data_bytes / 1048576.0, ms_rebuild, ms_open_normal);
        printf("| Live / deleted after rebuild | %ld / %ld (expected %ld live) |\n",
               cs.live, cs.deleted, n - n_del);
        printf("| Verify all %ld keys | %.1f ms |\n", last_id, ms_verify2);
        printf("| Result | **%s** (%ld mismatches) |\n\n", crash_fail ? "FAIL" : "PASS", crash_fail);
    }

    /* ---- A10. Cost of fsync -------------------------------------------- */
    {
        static const char *const names[] = {
            "`PDS_SYNC_NONE`", "`PDS_SYNC_ON_CLOSE` (default)", "`PDS_SYNC_ALWAYS`"
        };
        const long K2 = 1000;
        int mode;

        printf("### A10. Cost of fsync (%ld inserts into a fresh table, then close)\n\n", K2);
        printf("| Mode | Per insert | Close | Survives a power cut |\n|---|---|---|---|\n");
        for (mode = PDS_SYNC_NONE; mode <= PDS_SYNC_ALWAYS; mode++) {
            char tname[16];
            double ns_put, ms_cls;
            snprintf(tname, sizeof tname, "sync%d", mode);
            pds_set_sync_mode(mode);
            TRY(pds_create_table(tname, sizeof(Flight)));
            TRY(pds_open_table(tname));
            t0 = now_ns();
            for (i = 1; i <= K2; i++) {
                gen_flight((int32_t)i, &f);
                TRY(pds_put_rec(tname, f.flight_id, &f));
            }
            ns_put = (now_ns() - t0) / K2;
            t0 = now_ns();
            TRY(pds_close_table(tname));
            ms_cls = (now_ns() - t0) / 1e6;
            printf("| %s | %.1f us | %.1f ms | %s |\n", names[mode], ns_put / 1e3, ms_cls,
                   mode == PDS_SYNC_NONE ? "not guaranteed"
                   : mode == PDS_SYNC_ON_CLOSE ? "everything up to the last close"
                   : "every acknowledged write");
        }
        pds_set_sync_mode(PDS_SYNC_ON_CLOSE);
        printf("\n");
    }

    TRY(pds_close_db());
    ok = failures == 0;

fail:
    pds_set_sync_mode(PDS_SYNC_ON_CLOSE);
    if (pds_db_info()->status == PDS_DB_OPEN) pds_close_db();
    pds_drop_db(BENCH_DB);
    hc_free(chain);
    ai_free(array);
    free(ids);
    free(hits);
    free(misses);
    free(deleted);
    free(updated);
    return ok;
}

/* ======================================================================== */
/*  Part B - index structures in isolation                                  */
/* ======================================================================== */

static void bench_load_factors(void)
{
    static const double loads[] = { 0.25, 0.50, 0.75, 0.90 };
    const size_t cap = (size_t)1 << 17;     /* 131072 slots / buckets */
    const long L = 1000000;
    int32_t *keys = malloc(2 * cap * sizeof *keys);
    int32_t *probe_hit = malloc((size_t)L * sizeof *probe_hit);
    int32_t *probe_miss = malloc((size_t)L * sizeof *probe_miss);
    size_t k;
    int li;

    if (!keys || !probe_hit || !probe_miss) goto done;

    printf("### B1. Load factor sweep (%zu slots, %ld lookups per cell)\n\n", cap, L);
    printf("Expected probes for linear probing: hit = (1 + 1/(1-a))/2, miss = (1 + 1/(1-a)^2)/2. "
           "For chaining: hit = 1 + a/2, miss = a.\n\n");
    printf("| Load a | Open: probes hit / miss (expected) | Open: ns hit / miss | Chain: nodes hit / miss (expected) | Chain: ns hit / miss |\n");
    printf("|---|---|---|---|---|\n");

    for (li = 0; li < 4; li++) {
        double a = loads[li];
        size_t n = (size_t)(a * cap);
        HashOpen *ho = ho_create(cap, 0.95);
        HashChain *hc = hc_create(cap);
        double t0, ho_h, ho_m, hc_h, hc_m;
        double ph, pm, ch, cm;
        uintptr_t acc = 0;
        long i;

        if (!ho || !hc) { ho_free(ho); hc_free(hc); break; }
        for (k = 0; k < 2 * cap; k++) keys[k] = (int32_t)(k + 1);
        shuffle(keys, (long)(2 * cap));
        for (k = 0; k < n; k++) {
            ho_insert(ho, keys[k], &keys[k]);
            hc_insert(hc, keys[k], &keys[k]);
        }
        for (i = 0; i < L; i++) {
            probe_hit[i] = keys[rng_next() % n];
            probe_miss[i] = keys[n + rng_next() % (2 * cap - n)];
        }

        ho->probes = 0; t0 = now_ns();
        for (i = 0; i < L; i++) acc += (uintptr_t)ho_find(ho, probe_hit[i]);
        ho_h = (now_ns() - t0) / L; ph = (double)ho->probes / L;
        ho->probes = 0; t0 = now_ns();
        for (i = 0; i < L; i++) acc += (uintptr_t)ho_find(ho, probe_miss[i]);
        ho_m = (now_ns() - t0) / L; pm = (double)ho->probes / L;
        hc->probes = 0; t0 = now_ns();
        for (i = 0; i < L; i++) acc += (uintptr_t)hc_find(hc, probe_hit[i]);
        hc_h = (now_ns() - t0) / L; ch = (double)hc->probes / L;
        hc->probes = 0; t0 = now_ns();
        for (i = 0; i < L; i++) acc += (uintptr_t)hc_find(hc, probe_miss[i]);
        hc_m = (now_ns() - t0) / L; cm = (double)hc->probes / L;
        g_sink = acc;

        printf("| %.2f | %.2f / %.2f (%.2f / %.2f) | %.1f / %.1f | %.2f / %.2f (%.2f / %.2f) | %.1f / %.1f |\n",
               a, ph, pm, 0.5 * (1 + 1 / (1 - a)), 0.5 * (1 + 1 / ((1 - a) * (1 - a))),
               ho_h, ho_m, ch, cm, 1 + a / 2, a, hc_h, hc_m);

        ho_free(ho);
        hc_free(hc);
    }
    printf("\n");
done:
    free(keys);
    free(probe_hit);
    free(probe_miss);
}

static double avg_miss_probes(HashOpen *h, const int32_t *miss, long L)
{
    long i;
    uintptr_t acc = 0;
    h->probes = 0;
    for (i = 0; i < L; i++) acc += (uintptr_t)ho_find(h, miss[i]);
    g_sink = acc;
    return (double)h->probes / L;
}

static void bench_tombstones(void)
{
    const size_t cap = (size_t)1 << 17;
    const size_t live = cap / 2;            /* load 0.50 */
    const long L = 500000;
    int32_t *keys = malloc(3 * cap * sizeof *keys);
    int32_t *miss = malloc((size_t)L * sizeof *miss);
    HashOpen *h = ho_create(cap, 0.95);
    double fresh, churned, rehashed;
    size_t k, tombs;
    long i;

    if (!keys || !miss || !h) goto done;
    for (k = 0; k < 3 * cap; k++) keys[k] = (int32_t)(k + 1);
    shuffle(keys, (long)(3 * cap));
    for (i = 0; i < L; i++) miss[i] = keys[2 * cap + rng_next() % cap];  /* never inserted */

    for (k = 0; k < live; k++) ho_insert(h, keys[k], &keys[k]);
    fresh = avg_miss_probes(h, miss, L);

    /* churn: delete half the keys, insert the same number of new ones */
    for (k = 0; k < live / 2; k++) ho_remove(h, keys[k]);
    for (k = live; k < live + live / 2; k++) ho_insert(h, keys[k], &keys[k]);
    tombs = h->tombs;
    churned = avg_miss_probes(h, miss, L);

    ho_rehash(h, h->cap);
    rehashed = avg_miss_probes(h, miss, L);

    printf("### B2. Tombstone build-up (open addressing, %zu slots, %zu live keys)\n\n", cap, live);
    printf("Deleting half the keys and inserting replacements keeps the live load at 0.50, "
           "but tombstones still lengthen every unsuccessful probe run.\n\n");
    printf("| State | Tombstones | Occupancy | Avg probes per miss |\n|---|---|---|---|\n");
    printf("| Fresh | 0 | 0.50 | %.2f |\n", fresh);
    printf("| After churn | %zu | %.2f | %.2f |\n", tombs, (double)(live + tombs) / cap, churned);
    printf("| After `ho_rehash` | 0 | 0.50 | %.2f |\n\n", rehashed);

done:
    ho_free(h);
    free(keys);
    free(miss);
}

static void bench_bst_order(void)
{
    const long n = 20000, big_n = 1000000, L = 20000;
    int32_t *keys = malloc((size_t)big_n * sizeof *keys);
    int32_t *look = malloc((size_t)L * sizeof *look);
    int pass;
    long i;

    if (!keys || !look) goto done;

    printf("### B3. Plain BST vs AVL tree, sorted vs random insert order\n\n");
    printf("| Tree | Keys | Insert order | Height | Avg depth | Build | Per insert | Lookup |\n"
           "|---|---|---|---|---|---|---|---|\n");
    /* pass: 0/1 plain BST sorted/random at n; 2/3 AVL sorted/random at n;
     *       4/5 AVL sorted/random at big_n (plain BST sorted would take minutes) */
    for (pass = 0; pass < 6; pass++) {
        BSTNode *root = NULL;
        int avl = pass >= 2, sorted = pass % 2 == 0;
        long cnt = pass >= 4 ? big_n : n;
        double t0, build_ms, ns_look, avg;
        int height;
        uintptr_t acc = 0;

        for (i = 0; i < cnt; i++) keys[i] = (int32_t)(i + 1);
        if (!sorted) shuffle(keys, cnt);
        for (i = 0; i < L; i++) look[i] = (int32_t)(1 + rng_next() % (uint64_t)cnt);

        t0 = now_ns();
        if (avl) for (i = 0; i < cnt; i++) bst_add_node(&root, keys[i], NULL);
        else     for (i = 0; i < cnt; i++) bst_add_node_unbalanced(&root, keys[i], NULL);
        build_ms = (now_ns() - t0) / 1e6;
        bst_stats(root, NULL, &height, &avg);

        t0 = now_ns();
        for (i = 0; i < L; i++) acc += (uintptr_t)bst_search(root, look[i]);
        ns_look = (now_ns() - t0) / L;
        g_sink = acc;

        printf("| %s | %ld | %s | %d | %.1f | %.1f ms | %.0f ns | %.0f ns |\n",
               avl ? "**AVL**" : "Plain BST", cnt, sorted ? "Sorted (1, 2, 3, ...)" : "Random",
               height, avg, build_ms, build_ms * 1e6 / cnt, ns_look);
        bst_free(root);
    }
    printf("\nSorted IDs turn the plain BST into a linked list (height = number of keys). "
           "The AVL tree rotates on insert, so sorted input gives the *shortest* tree.\n\n");
done:
    free(keys);
    free(look);
}

static void bench_insert_cost(void)
{
    const long n = 20000;
    int32_t *keys = malloc((size_t)n * sizeof *keys);
    ArrayIndex *a = ai_create(16);
    BSTNode *root = NULL;
    HashChain *hc = hc_create(200003);
    HashOpen *ho = ho_create(16, 0.7);
    double t0, ns_a, ns_b, ns_c, ns_o;
    long i;

    if (!keys || !a || !hc || !ho) goto done;
    for (i = 0; i < n; i++) keys[i] = (int32_t)(i + 1);
    shuffle(keys, n);

    t0 = now_ns();
    for (i = 0; i < n; i++) ai_insert(a, keys[i], NULL);
    ns_a = (now_ns() - t0) / n;
    t0 = now_ns();
    for (i = 0; i < n; i++) bst_add_node(&root, keys[i], NULL);
    ns_b = (now_ns() - t0) / n;
    t0 = now_ns();
    for (i = 0; i < n; i++) hc_insert(hc, keys[i], NULL);
    ns_c = (now_ns() - t0) / n;
    t0 = now_ns();
    for (i = 0; i < n; i++) ho_insert(ho, keys[i], NULL);
    ns_o = (now_ns() - t0) / n;

    printf("### B4. Insert cost, %ld keys in random order\n\n", n);
    printf("| Index | Per insert | Why |\n|---|---|---|\n");
    printf("| Sorted array | %.0f ns | shifts on average half the array (`memmove`) |\n", ns_a);
    printf("| AVL tree | %.0f ns | walks ~log n nodes, may rotate, one malloc |\n", ns_b);
    printf("| Separate chaining | %.0f ns | one malloc per key |\n", ns_c);
    printf("| Open addressing | %.0f ns | writes a slot; occasional doubling rehash |\n\n", ns_o);
    printf("The sorted array has fast lookups (binary search) but O(n) inserts, which is why the "
           "engine keeps an AVL tree for ordering instead.\n\n");
done:
    free(keys);
    ai_free(a);
    bst_free(root);
    hc_free(hc);
    ho_free(ho);
}

/* Same records, written and read back two ways:
 *   binary - fopen "wb"/"rb", one fwrite/fread of the raw struct
 *   text   - fopen "w"/"r",   fprintf of a CSV line / fgets + parse */
static int bench_text_vs_binary(long n)
{
    const char *bin_path = "skybench_io.bin";
    const char *txt_path = "skybench_io.csv";
    const long RAND = 1000;
    Flight f, expect;
    char line[256];
    FILE *fp;
    long i, bad = 0;
    double t0, ms_bw, ms_tw, ms_br, ms_tr, ns_bseek, ms_tseek;
    long long bin_bytes = 0, txt_bytes = 0;
    int32_t key;

    /* write */
    if (!(fp = fopen(bin_path, "wb"))) return 0;
    t0 = now_ns();
    for (i = 1; i <= n; i++) {
        gen_flight((int32_t)i, &f);
        if (fwrite(&f, sizeof f, 1, fp) != 1) bad++;
    }
    fclose(fp);
    ms_bw = (now_ns() - t0) / 1e6;

    if (!(fp = fopen(txt_path, "w"))) return 0;
    t0 = now_ns();
    fprintf(fp, "%s\n", FLIGHT_CSV_HEADER);
    for (i = 1; i <= n; i++) {
        gen_flight((int32_t)i, &f);
        if (flight_to_csv(&f, line, sizeof line) != 0 || fprintf(fp, "%s\n", line) < 0) bad++;
    }
    fclose(fp);
    ms_tw = (now_ns() - t0) / 1e6;

    /* sizes */
    if ((fp = fopen(bin_path, "rb"))) { fseek(fp, 0, SEEK_END); bin_bytes = ftell(fp); fclose(fp); }
    if ((fp = fopen(txt_path, "rb"))) { fseek(fp, 0, SEEK_END); txt_bytes = ftell(fp); fclose(fp); }

    /* sequential read + verify */
    if (!(fp = fopen(bin_path, "rb"))) return 0;
    t0 = now_ns();
    for (i = 1; i <= n; i++) {
        if (fread(&f, sizeof f, 1, fp) != 1) { bad++; break; }
        gen_flight((int32_t)i, &expect);
        if (memcmp(&f, &expect, sizeof f) != 0) bad++;
    }
    ms_br = (now_ns() - t0) / 1e6;

    /* random access: record i lives at byte (i-1) * sizeof(Flight) */
    t0 = now_ns();
    for (i = 0; i < RAND; i++) {
        long r = (long)(rng_next() % (uint64_t)n);
        fseek(fp, r * (long)sizeof f, SEEK_SET);
        if (fread(&f, sizeof f, 1, fp) != 1 || f.flight_id != r + 1) bad++;
    }
    ns_bseek = (now_ns() - t0) / RAND;
    fclose(fp);

    if (!(fp = fopen(txt_path, "r"))) return 0;
    t0 = now_ns();
    if (!fgets(line, sizeof line, fp)) bad++;              /* header */
    for (i = 1; i <= n; i++) {
        if (!fgets(line, sizeof line, fp)) { bad++; break; }
        line[strcspn(line, "\r\n")] = '\0';
        gen_flight((int32_t)i, &expect);
        if (flight_from_csv(line, &f, &key) != 0 || memcmp(&f, &expect, sizeof f) != 0) bad++;
    }
    ms_tr = (now_ns() - t0) / 1e6;

    /* random access in text: lines have different lengths, so the only way
     * to reach record r is to read every line before it */
    t0 = now_ns();
    for (i = 0; i < RAND / 10; i++) {
        long r = (long)(rng_next() % (uint64_t)n), j;
        rewind(fp);
        for (j = 0; j <= r + 1 && fgets(line, sizeof line, fp); j++) {}
        line[strcspn(line, "\r\n")] = '\0';
        if (flight_from_csv(line, &f, &key) != 0 || key != r + 1) bad++;
    }
    ms_tseek = (now_ns() - t0) / (RAND / 10) / 1e6;
    fclose(fp);

    remove(bin_path);
    remove(txt_path);

    printf("### B5. Text vs binary file I/O (%ld flight records)\n\n", n);
    printf("| | Binary (`fwrite` / `fread`, \"wb\"/\"rb\") | Text (`fprintf` / `fgets` + parse, \"w\"/\"r\") |\n|---|---|---|\n");
    printf("| File size | %.2f MB (%d bytes/record, fixed) | %.2f MB (~%.1f bytes/record, varies) |\n",
           bin_bytes / 1048576.0, (int)sizeof(Flight), txt_bytes / 1048576.0, (double)txt_bytes / n);
    printf("| Write all | %.1f ms | %.1f ms |\n", ms_bw, ms_tw);
    printf("| Read all + verify | %.1f ms | %.1f ms |\n", ms_br, ms_tr);
    printf("| Read record #r at random | %.0f ns (`fseek` to (r-1) x %d) | %.2f ms (read r lines from the top) |\n",
           ns_bseek, (int)sizeof(Flight), ms_tseek);
    printf("| Round trip correct | %s | %s |\n\n", bad ? "FAIL" : "PASS", bad ? "FAIL" : "PASS");
    printf("Fixed-size binary records are what make `fseek`-based random access - and therefore "
           "the `.ndx` offset index - possible. Text is human-readable and portable, which is why "
           "SkyBook uses it only for export/import.\n\n");
    return bad == 0;
}

/* ======================================================================== */

int run_benchmark(long n)
{
    int ok;

    if (pds_db_info()->status == PDS_DB_OPEN) {
        fprintf(stderr, "Close the open database before running the benchmark.\n");
        return 1;
    }
    if (n < 1000) n = 1000;
    if (n > 50000000) n = 50000000;

    printf("## SkyBook benchmark\n\n");
#ifdef __VERSION__
    printf("Compiler: %s. ", __VERSION__);
#endif
    printf("Records: %ld flights. sizeof(Flight) = %d bytes.\n\n", n, (int)sizeof(Flight));

    printf("## Part A - engine on disk\n\n");
    ok = bench_engine(n);

    printf("## Part B - index structures in memory\n\n");
    bench_load_factors();
    bench_tombstones();
    bench_bst_order();
    bench_insert_cost();
    if (!bench_text_vs_binary(n)) ok = 0;

    printf("Overall: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
