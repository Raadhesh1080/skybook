/*
 * tests.c - SkyBook test suite (make test).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "array_index.h"
#include "hash_chain.h"
#include "skybook.h"

static int g_pass, g_fail;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

#define AS_PTR(i) ((void *)(intptr_t)(i))

/* ---- hash tables -------------------------------------------------------- */

static void test_hash_open(void)
{
    HashOpen *h = ho_create(16, 0.7);
    int i, all_found = 1, odd_ok = 1;

    printf("hash_open\n");
    CHECK(h != NULL);
    for (i = 1; i <= 10000; i++) CHECK(ho_insert(h, i, AS_PTR(i)) == HT_SUCCESS);
    CHECK(h->used == 10000);
    CHECK(h->cap >= 16384);                         /* grew from 16 */
    CHECK(ho_occupancy(h) <= 0.7);
    for (i = 1; i <= 10000; i++) if (ho_find(h, i) != AS_PTR(i)) all_found = 0;
    CHECK(all_found);
    CHECK(ho_find(h, 10001) == NULL);
    CHECK(ho_find(h, -5) == NULL);

    for (i = 2; i <= 10000; i += 2) CHECK(ho_remove(h, i) == HT_SUCCESS);
    CHECK(ho_remove(h, 2) == HT_NOT_FOUND);
    CHECK(h->used == 5000 && h->tombs == 5000);
    for (i = 1; i <= 10000; i++) {
        void *v = ho_find(h, i);
        if ((i % 2 == 0 && v != NULL) || (i % 2 == 1 && v != AS_PTR(i))) odd_ok = 0;
    }
    CHECK(odd_ok);                                  /* probe runs survive tombstones */

    CHECK(ho_insert(h, 3, AS_PTR(333)) == HT_SUCCESS);   /* overwrite */
    CHECK(ho_find(h, 3) == AS_PTR(333));
    CHECK(h->used == 5000);

    CHECK(ho_rehash(h, h->cap) == HT_SUCCESS);
    CHECK(h->tombs == 0 && h->used == 5000);
    CHECK(ho_find(h, 9999) == AS_PTR(9999));
    CHECK(ho_find(h, 3) == AS_PTR(333));

    /* heavy churn at a fixed live size must not grow the table forever */
    {
        HashOpen *c = ho_create(64, 0.7);
        size_t cap_after;
        for (i = 0; i < 20; i++) ho_insert(c, i, AS_PTR(1));
        for (i = 20; i < 200000; i++) {
            ho_insert(c, i, AS_PTR(1));
            ho_remove(c, i - 20);
        }
        cap_after = c->cap;
        CHECK(c->used == 20);
        CHECK(cap_after <= 64);
        ho_free(c);
    }
    ho_free(h);
}

static void test_hash_chain(void)
{
    HashChain *h = hc_create(101);
    int i, ok = 1;

    printf("hash_chain\n");
    for (i = 1; i <= 2000; i++) hc_insert(h, i, AS_PTR(i));
    CHECK(h->count == 2000);
    for (i = 1; i <= 2000; i++) if (hc_find(h, i) != AS_PTR(i)) ok = 0;
    CHECK(ok);
    CHECK(hc_remove(h, 50) == HT_SUCCESS);
    CHECK(hc_remove(h, 50) == HT_NOT_FOUND);
    CHECK(hc_find(h, 50) == NULL);
    CHECK(h->count == 1999);
    hc_free(h);
}

/* ---- array index -------------------------------------------------------- */

static void test_array_index(void)
{
    ArrayIndex *a = ai_create(4);
    int i, ok = 1, sorted = 1;

    printf("array_index\n");
    CHECK(a != NULL);
    for (i = 0; i < 3000; i++) {                 /* scrambled order: 0, 7, 14, ... mod 3000 */
        int k = (i * 7) % 3000 + 1;
        CHECK(ai_insert(a, k, AS_PTR(k)) == AI_SUCCESS);
    }
    CHECK(a->count == 3000);
    CHECK(ai_insert(a, 42, NULL) == AI_DUP_KEY);
    for (i = 1; i < 3000; i++) if (a->items[i - 1].key >= a->items[i].key) sorted = 0;
    CHECK(sorted);
    for (i = 1; i <= 3000; i++) {
        if (ai_find(a, i) != AS_PTR(i) || ai_find_linear(a, i) != AS_PTR(i)) ok = 0;
    }
    CHECK(ok);
    CHECK(ai_find(a, 0) == NULL && ai_find(a, 3001) == NULL && ai_find_linear(a, 3001) == NULL);

    a->comparisons = 0;
    ai_find(a, 1234);
    CHECK(a->comparisons <= 12);                 /* ceil(log2(3000)) = 12 */

    CHECK(ai_remove(a, 1500) == AI_SUCCESS);
    CHECK(ai_remove(a, 1500) == AI_NOT_FOUND);
    CHECK(ai_find(a, 1500) == NULL && ai_find(a, 1501) == AS_PTR(1501) && a->count == 2999);
    ai_free(a);

    a = ai_create(0);
    CHECK(ai_append(a, 5, NULL) == AI_SUCCESS && ai_append(a, 9, NULL) == AI_SUCCESS);
    CHECK(ai_append(a, 9, NULL) == AI_INVALID && ai_append(a, 3, NULL) == AI_INVALID);
    ai_free(a);
}

/* ---- BST ---------------------------------------------------------------- */

typedef struct { int32_t last; long count; int sorted; } OrderCtx;

static int order_cb(BSTNode *n, void *p)
{
    OrderCtx *c = p;
    if (c->count && n->key <= c->last) c->sorted = 0;
    c->last = n->key;
    c->count++;
    return 0;
}

static int collect_cb(BSTNode *n, void *p)
{
    BSTNode **root = p;
    return bst_add_node(root, n->key, NULL) != BST_SUCCESS;
}

typedef struct { int32_t *keys; long n; } KeyList;

static int list_cb(BSTNode *node, void *p)
{
    KeyList *l = p;
    l->keys[l->n++] = node->key;
    return 0;
}

/* Same keys in the same breadth-first positions = identical shape. */
static int same_shape(BSTNode *a, BSTNode *b, long n)
{
    KeyList la, lb;
    int same;
    la.keys = malloc((size_t)n * sizeof *la.keys);
    lb.keys = malloc((size_t)n * sizeof *lb.keys);
    la.n = lb.n = 0;
    bst_levelorder(a, list_cb, &la);
    bst_levelorder(b, list_cb, &lb);
    same = la.n == n && lb.n == n && memcmp(la.keys, lb.keys, (size_t)n * sizeof *la.keys) == 0;
    free(la.keys);
    free(lb.keys);
    return same;
}

static double log2_of(double x)
{
    double r = 0;
    while (x >= 2) { x /= 2; r++; }
    return r + (x - 1);       /* close enough for a bound check */
}

static void test_bst(void)
{
    BSTNode *root = NULL, *copy = NULL;
    int32_t keys[1000];
    OrderCtx c = { 0, 0, 1 };
    int i, h1, h2;
    double d1, d2;

    printf("bst\n");
    for (i = 0; i < 1000; i++) keys[i] = i + 1;
    for (i = 999; i > 0; i--) {              /* fixed-seed shuffle */
        int j = (int)((i * 7919u + 13u) % (unsigned)(i + 1));
        int32_t t = keys[i]; keys[i] = keys[j]; keys[j] = t;
    }
    for (i = 0; i < 1000; i++) CHECK(bst_add_node(&root, keys[i], NULL) == BST_SUCCESS);
    CHECK(bst_add_node(&root, 500, NULL) == BST_DUP_KEY);
    CHECK(bst_search(root, 777) && bst_search(root, 777)->key == 777);
    CHECK(bst_search(root, 1001) == NULL);
    CHECK(bst_max(root)->key == 1000);

    CHECK(bst_inorder(root, order_cb, &c) == 0);
    CHECK(c.count == 1000 && c.sorted);

    c.count = 0; c.sorted = 1;
    CHECK(bst_range(root, 100, 199, order_cb, &c) == 0);
    CHECK(c.count == 100 && c.sorted);

    CHECK(bst_is_avl(root));

    /* level-order replay rebuilds an identically shaped tree (the .ndx format relies on this) */
    CHECK(bst_levelorder(root, collect_cb, &copy) == 0);
    CHECK(bst_is_avl(copy));
    CHECK(same_shape(root, copy, 1000));
    bst_stats(root, NULL, &h1, &d1);
    bst_stats(copy, NULL, &h2, &d2);
    CHECK(h1 == h2 && d1 == d2);
    bst_free(copy);

    /* ...whereas pre-order replay into an AVL tree triggers rotations */
    copy = NULL;
    CHECK(bst_preorder(root, collect_cb, &copy) == 0);
    CHECK(bst_is_avl(copy));
    bst_free(root);
    bst_free(copy);

    /* AVL: sorted, reverse-sorted and alternating inserts all stay balanced */
    {
        static const char *const names[] = { "ascending", "descending", "zig-zag" };
        int pattern;
        double bound = 1.4405 * log2_of(100000 + 2);
        for (pattern = 0; pattern < 3; pattern++) {
            long n = 100000;
            root = NULL;
            for (i = 0; i < n; i++) {
                int32_t k = pattern == 0 ? i + 1
                          : pattern == 1 ? (int32_t)(n - i)
                          : (i % 2 ? i : (int32_t)(n * 2 - i));
                bst_add_node(&root, k, NULL);
            }
            bst_stats(root, NULL, &h1, NULL);
            if (!(h1 <= bound && bst_is_avl(root))) printf("  (%s inserts)\n", names[pattern]);
            CHECK(h1 <= bound);           /* ~ 24 for 100,000 keys */
            CHECK(bst_is_avl(root));
            bst_free(root);
        }
        root = NULL;
        for (i = 1; i <= 100000; i++) bst_add_node(&root, i, NULL);
        bst_stats(root, NULL, &h1, NULL);
        CHECK(h1 >= 17 && h1 <= 18);      /* minimum possible is 17: 2^17 > 100,000 */
        bst_free(root);
    }

    /* the unbalanced baseline degenerates, and iteration must still not overflow */
    root = NULL;
    for (i = 1; i <= 50000; i++) bst_add_node_unbalanced(&root, i, NULL);
    bst_stats(root, NULL, &h1, NULL);
    CHECK(h1 == 50000);
    c.count = 0; c.sorted = 1;
    CHECK(bst_inorder(root, order_cb, &c) == 0 && c.count == 50000);
    CHECK(!bst_is_avl(root));
    bst_free(root);
}

/* ---- PDS engine --------------------------------------------------------- */

static Passenger make_passenger(int id, const char *name)
{
    Passenger p;
    memset(&p, 0, sizeof p);
    p.passenger_id = id;
    copy_str(p.name, NAME_LEN, name);
    snprintf(p.passport_no, sizeof p.passport_no, "P%07d", id);
    return p;
}

static void test_pds(void)
{
    const char *db = "skytest";
    Passenger p, got;
    long matches, comps, count;
    int i;
    int32_t max;

    printf("pds\n");
    pds_drop_db(db);

    /* guards: nothing works before a db/table is open */
    CHECK(pds_put_rec("t", 1, &p) == PDS_DB_NOT_OPEN);
    CHECK(pds_create_db("bad/name") == PDS_INVALID_ARG);

    CHECK(pds_create_db(db) == PDS_SUCCESS);
    CHECK(pds_create_db(db) == PDS_DB_EXISTS);
    CHECK(pds_open_db(db) == PDS_SUCCESS);
    CHECK(pds_open_db(db) == PDS_DB_ALREADY_OPEN);
    CHECK(pds_create_table("people", sizeof(Passenger)) == PDS_SUCCESS);
    CHECK(pds_create_table("people", sizeof(Passenger)) == PDS_TABLE_EXISTS);
    CHECK(pds_put_rec("people", 1, &p) == PDS_TABLE_NOT_OPEN);
    CHECK(pds_get_rec("nope", 1, &got) == PDS_TABLE_NOT_FOUND);
    CHECK(pds_open_table("people") == PDS_SUCCESS);
    CHECK(pds_open_table("people") == PDS_TABLE_ALREADY_OPEN);

    for (i = 1; i <= 50; i++) {
        p = make_passenger(i, i % 10 == 0 ? "Common Name" : "Someone");
        CHECK(pds_put_rec("people", i, &p) == PDS_SUCCESS);
    }
    CHECK(pds_put_rec("people", 7, &p) == PDS_DUPLICATE_KEY);

    CHECK(pds_get_rec("people", 25, &got) == PDS_SUCCESS && got.passenger_id == 25);
    CHECK(pds_get_rec_via_bst("people", 25, &got) == PDS_SUCCESS && got.passenger_id == 25);
    CHECK(pds_get_rec("people", 51, &got) == PDS_REC_NOT_FOUND);

    /* update in place */
    p = make_passenger(25, "Updated Name");
    CHECK(pds_update_rec("people", 25, &p) == PDS_SUCCESS);
    CHECK(pds_get_rec("people", 25, &got) == PDS_SUCCESS && strcmp(got.name, "Updated Name") == 0);
    CHECK(pds_update_rec("people", 99, &p) == PDS_REC_NOT_FOUND);

    /* soft delete and revive */
    CHECK(pds_delete_rec("people", 10) == PDS_SUCCESS);
    CHECK(pds_delete_rec("people", 10) == PDS_REC_NOT_FOUND);
    CHECK(pds_get_rec("people", 10, &got) == PDS_REC_NOT_FOUND);
    CHECK(pds_get_rec_via_bst("people", 10, &got) == PDS_REC_NOT_FOUND);
    CHECK(pds_update_rec("people", 10, &p) == PDS_REC_NOT_FOUND);
    p = make_passenger(10, "Revived");
    CHECK(pds_put_rec("people", 10, &p) == PDS_SUCCESS);
    CHECK(pds_get_rec("people", 10, &got) == PDS_SUCCESS && strcmp(got.name, "Revived") == 0);
    CHECK(pds_delete_rec("people", 11) == PDS_SUCCESS);

    /* field search sees only current, live versions */
    CHECK(pds_get_rec_by_field("people", "common name", match_passenger_name, NULL, NULL,
                               &matches, &comps) == PDS_SUCCESS);
    CHECK(matches == 4);            /* 20, 30, 40, 50 - id 10 was revived as "Revived" */
    CHECK(comps == 49);             /* 50 live records minus deleted 11 */
    CHECK(pds_get_rec_by_field("people", "Revived", match_passenger_name, NULL, NULL,
                               &matches, NULL) == PDS_SUCCESS && matches == 1);

    /* no-index lookup: frames are 1..50 in insert order, then the revived 10 at frame 51 */
    CHECK(pds_get_rec_by_scan("people", 25, &got, &comps) == PDS_SUCCESS);
    CHECK(comps == 25 && strcmp(got.name, "Updated Name") == 0);
    CHECK(pds_get_rec_by_scan("people", 10, &got, &comps) == PDS_SUCCESS);
    /* frames: 1..50, 51 = tombstone(10), 52 = revived 10 */
    CHECK(comps == 52 && strcmp(got.name, "Revived") == 0);   /* stale frame 10 + tombstone skipped */
    /* 53 = tombstone(11) */
    CHECK(pds_get_rec_by_scan("people", 11, &got, &comps) == PDS_REC_NOT_FOUND && comps == 53);
    CHECK(pds_get_rec_by_scan("people", 999, &got, &comps) == PDS_REC_NOT_FOUND);
    {
        const PDS_NdxInfo *e = ho_find(pds_table_info("people")->hash, 30);
        CHECK(e && pds_get_rec_at("people", e, &got) == PDS_SUCCESS && got.passenger_id == 30);
        CHECK(pds_get_rec_at("people", NULL, &got) == PDS_INVALID_ARG);
    }

    CHECK(pds_scan_range("people", 5, 15, NULL, NULL, &count) == PDS_SUCCESS);
    CHECK(count == 10);             /* 5..15 minus deleted 11 */
    CHECK(pds_max_key("people", &max) == PDS_SUCCESS && max == 50);

    /* persistence */
    CHECK(pds_close_db() == PDS_SUCCESS);
    CHECK(pds_get_rec("people", 1, &got) == PDS_DB_NOT_OPEN);
    CHECK(pds_open_db(db) == PDS_SUCCESS);
    CHECK(pds_open_table("people") == PDS_SUCCESS);
    CHECK(pds_get_rec("people", 25, &got) == PDS_SUCCESS && strcmp(got.name, "Updated Name") == 0);
    CHECK(pds_get_rec("people", 10, &got) == PDS_SUCCESS && strcmp(got.name, "Revived") == 0);
    CHECK(pds_get_rec("people", 11, &got) == PDS_REC_NOT_FOUND);
    CHECK(pds_get_rec("people", 50, &got) == PDS_SUCCESS && got.passenger_id == 50);
    {
        PDS_TableStats s;
        CHECK(pds_table_stats("people", &s) == PDS_SUCCESS);
        CHECK(s.live == 49 && s.deleted == 1);
        /* 50 puts + 2 tombstones + 1 revived copy, 8-byte frame header each */
        CHECK(s.data_bytes == 53 * (int64_t)(FRAME_HEADER + sizeof(Passenger)));
        CHECK(s.recovery == PDS_RECOVERY_NONE);
    }
    CHECK(pds_get_rec_by_field("people", "common name", match_passenger_name, NULL, NULL,
                               &matches, NULL) == PDS_SUCCESS && matches == 4);
    CHECK(pds_close_db() == PDS_SUCCESS);
    CHECK(pds_drop_db(db) == PDS_SUCCESS);
}

/* ---- Text (CSV) export / import ----------------------------------------- */

static void test_csv(void)
{
    const char *db = "skytest3";
    Passenger p, q;
    Flight f, g;
    Booking b, c;
    char line[256], first[256];
    long n, imported, skipped;
    int32_t key;
    int i, same = 1;
    FILE *fp;

    printf("csv\n");

    /* record <-> line conversions */
    memset(&f, 0, sizeof f);
    f.flight_id = 7; copy_str(f.flight_no, FLIGHT_NO_LEN, "SB007");
    copy_str(f.source, CODE_LEN, "DEL"); copy_str(f.dest, CODE_LEN, "GOI");
    copy_str(f.date, DATE_LEN, "2026-12-24");
    f.seats_total = 180; f.seats_booked = 3; f.fare = 5200;
    CHECK(flight_to_csv(&f, line, sizeof line) == 0);
    CHECK(strcmp(line, "7,SB007,DEL,GOI,2026-12-24,180,3,5200") == 0);
    CHECK(flight_from_csv(line, &g, &key) == 0 && key == 7 && memcmp(&f, &g, sizeof f) == 0);
    CHECK(flight_to_csv(&f, line, 10) == 1);                       /* line too small */

    b.booking_id = 3; b.flight_id = 7; b.passenger_id = 9; b.fare_paid = 5200;
    CHECK(booking_to_csv(&b, line, sizeof line) == 0);
    CHECK(booking_from_csv(line, &c, &key) == 0 && key == 3 && memcmp(&b, &c, sizeof b) == 0);

    p = make_passenger(1, "Diya Patel");
    p.phone[0] = '\0';                                             /* empty field survives */
    CHECK(passenger_to_csv(&p, line, sizeof line) == 0);
    CHECK(passenger_from_csv(line, &q, &key) == 0 && memcmp(&p, &q, sizeof p) == 0);
    copy_str(p.name, NAME_LEN, "Patel, Diya");
    CHECK(passenger_to_csv(&p, line, sizeof line) == 1);           /* comma can't be written */

    CHECK(flight_from_csv("7,SB007,DEL,GOI,2026-12-24,180,3", &g, &key) == 1);        /* 7 fields */
    CHECK(flight_from_csv("7,SB007,DEL,GOI,2026-12-24,180,3,52x0", &g, &key) == 1);   /* bad int */
    CHECK(flight_from_csv("7,SB007,DELHI,GOI,2026-12-24,180,3,5200", &g, &key) == 1); /* too long */
    CHECK(passenger_from_csv("1,A,B,C,D", &q, &key) == 1);                             /* 5 fields */

    /* engine round trip */
    pds_drop_db(db);
    CHECK(pds_create_db(db) == PDS_SUCCESS && pds_open_db(db) == PDS_SUCCESS);
    CHECK(pds_create_table("src", sizeof(Passenger)) == PDS_SUCCESS);
    CHECK(pds_create_table("dst", sizeof(Passenger)) == PDS_SUCCESS);
    CHECK(pds_open_table("src") == PDS_SUCCESS && pds_open_table("dst") == PDS_SUCCESS);
    for (i = 300; i >= 1; i--) {                   /* inserted in reverse order */
        p = make_passenger(i, i % 2 ? "Odd Person" : "Even Person");
        pds_put_rec("src", i, &p);
    }
    pds_delete_rec("src", 150);
    CHECK(pds_export_text("src", "people.csv", PASSENGER_CSV_HEADER, passenger_to_csv, &n) == PDS_SUCCESS);
    CHECK(n == 299);

    fp = fopen("people.csv", "r");
    CHECK(fp != NULL);
    if (fp) {
        CHECK(fgets(first, sizeof first, fp) && strcmp(first, PASSENGER_CSV_HEADER "\n") == 0);
        /* storage order: rows were inserted 300, 299, ... so 300 comes first */
        CHECK(fgets(line, sizeof line, fp) && strncmp(line, "300,Even Person,", 16) == 0);
        fclose(fp);
    }

    CHECK(pds_import_text("dst", "people.csv", 1, passenger_from_csv, &imported, &skipped) == PDS_SUCCESS);
    CHECK(imported == 299 && skipped == 0);
    for (i = 1; i <= 300; i++) {
        int r1 = pds_get_rec("src", i, &p), r2 = pds_get_rec("dst", i, &q);
        if (r1 != r2 || (r1 == PDS_SUCCESS && memcmp(&p, &q, sizeof p) != 0)) same = 0;
    }
    CHECK(same);
    {
        PDS_TableStats s1, s2;
        pds_table_stats("src", &s1);
        pds_table_stats("dst", &s2);
        /* src was filled 300, 299, ..., 1 - sorted input, but the AVL tree
         * stays balanced: 300 keys fit in height 9 */
        CHECK(s1.bst_height <= 10 && s2.bst_height <= 10);
        CHECK(bst_is_avl(pds_table_info("src")->bst_root) && bst_is_avl(pds_table_info("dst")->bst_root));
    }

    /* importing again: every key already exists */
    CHECK(pds_import_text("dst", "people.csv", 1, passenger_from_csv, &imported, &skipped) == PDS_SUCCESS);
    CHECK(imported == 0 && skipped == 299);

    /* malformed lines are skipped, good ones imported, blank lines ignored */
    fp = fopen("messy.csv", "w");
    if (fp) {
        fputs("passenger_id,name,passport_no,phone\n"
              "1000,Good One,P1,123\n"
              "not,a,valid\n"
              "\n"
              "1001,Also Good,P2,\n"
              "1002,Too,Many,Fields,Here\n"
              "abc,Bad Key,P3,1\n", fp);
        fclose(fp);
    }
    CHECK(pds_import_text("dst", "messy.csv", 1, passenger_from_csv, &imported, &skipped) == PDS_SUCCESS);
    CHECK(imported == 2 && skipped == 3);
    CHECK(pds_get_rec("dst", 1001, &q) == PDS_SUCCESS && strcmp(q.name, "Also Good") == 0);

    CHECK(pds_import_text("dst", "missing.csv", 1, passenger_from_csv, &imported, &skipped) == PDS_FILE_ERROR);

    /* a record that can't be written as CSV aborts the export and leaves no file */
    p = make_passenger(301, "Comma, Name");
    pds_put_rec("src", 301, &p);
    CHECK(pds_export_text("src", "bad.csv", NULL, passenger_to_csv, &n) == PDS_INVALID_ARG);
    fp = fopen("bad.csv", "r");
    CHECK(fp == NULL);
    if (fp) fclose(fp);

    remove("people.csv");
    remove("messy.csv");
    CHECK(pds_close_db() == PDS_SUCCESS);
    CHECK(pds_drop_db(db) == PDS_SUCCESS);
}

/* ---- Secondary index (unit) --------------------------------------------- */

static int64_t first_byte_key(const void *rec)
{
    return ((const unsigned char *)rec)[0];
}

static void test_secindex(void)
{
    SecIndex *s = si_create("t", first_byte_key);
    size_t n;
    const int32_t *pks;
    int i, ok = 1;

    printf("secindex\n");
    CHECK(s != NULL);
    for (i = 0; i < 5000; i++) CHECK(si_add(s, i % 100, i) == SI_SUCCESS);
    CHECK(s->nkeys == 100 && s->nentries == 5000);
    CHECK(s->nbuckets >= 100);                     /* grew from 64 */
    pks = si_find(s, 42, &n);
    CHECK(n == 50);
    for (i = 0; pks && i < (int)n; i++) if (pks[i] % 100 != 42) ok = 0;
    CHECK(ok);
    CHECK(si_find(s, 1000, &n) == NULL && n == 0);

    CHECK(si_remove(s, 42, 142) == SI_SUCCESS);
    CHECK(si_remove(s, 42, 142) == SI_NOT_FOUND);
    CHECK(si_remove(s, 43, 142) == SI_NOT_FOUND);
    si_find(s, 42, &n);
    CHECK(n == 49);
    for (i = 42; i < 5000; i += 100) si_remove(s, 42, i);
    CHECK(si_find(s, 42, &n) == NULL && n == 0 && s->nkeys == 99);   /* empty key dropped */
    si_free(s);
}

/* ---- Crash recovery and fsync ------------------------------------------- */

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp) fclose(fp);
    return fp != NULL;
}

/* After the standard mutation sequence: 1..100 put, 10..19 deleted,
 * 15 re-inserted as "Back Again", 50 renamed "Changed". */
static int check_recovered_state(void)
{
    Passenger got;
    int i, ok = 1;
    PDS_TableStats s;
    for (i = 1; i <= 100; i++) {
        int rc = pds_get_rec("crash", i, &got);
        int should_exist = !(i >= 10 && i <= 19) || i == 15;
        if (should_exist != (rc == PDS_SUCCESS)) ok = 0;
    }
    if (pds_get_rec("crash", 15, &got) != PDS_SUCCESS || strcmp(got.name, "Back Again") != 0) ok = 0;
    if (pds_get_rec("crash", 50, &got) != PDS_SUCCESS || strcmp(got.name, "Changed") != 0) ok = 0;
    if (pds_table_stats("crash", &s) != PDS_SUCCESS || s.live != 91 || s.deleted != 9) ok = 0;
    if (!bst_is_avl(pds_table_info("crash")->bst_root)) ok = 0;
    return ok;
}

static int recovery_of(const char *table)
{
    PDS_TableStats s;
    return pds_table_stats(table, &s) == PDS_SUCCESS ? s.recovery : -1;
}

static void test_recovery(void)
{
    const char *db = "skytest4";
    Passenger p;
    int i;
    FILE *fp;

    printf("recovery\n");
    pds_drop_db(db);
    CHECK(pds_create_db(db) == PDS_SUCCESS && pds_open_db(db) == PDS_SUCCESS);
    CHECK(pds_create_table("crash", sizeof(Passenger)) == PDS_SUCCESS);
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NONE);

    for (i = 1; i <= 100; i++) {
        p = make_passenger(i, "Original");
        pds_put_rec("crash", i, &p);
    }
    for (i = 10; i <= 19; i++) pds_delete_rec("crash", i);
    p = make_passenger(15, "Back Again");
    CHECK(pds_put_rec("crash", 15, &p) == PDS_SUCCESS);
    p = make_passenger(50, "Changed");
    CHECK(pds_update_rec("crash", 50, &p) == PDS_SUCCESS);

    /* 1. crash: .ndx on disk still describes the empty table -> stale */
    CHECK(pds_simulate_crash("crash") == PDS_SUCCESS);
    CHECK(pds_get_rec("crash", 1, &p) == PDS_TABLE_NOT_OPEN);
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NDX_STALE);
    CHECK(check_recovered_state());

    /* 2. clean close -> normal load, same state */
    CHECK(pds_close_table("crash") == PDS_SUCCESS);
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NONE);
    CHECK(check_recovered_state());
    CHECK(pds_close_table("crash") == PDS_SUCCESS);

    /* 3. .ndx deleted -> rebuilt, and the repaired .ndx is written back */
    CHECK(remove("crash.ndx") == 0);
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NDX_MISSING);
    CHECK(check_recovered_state());
    CHECK(file_exists("crash.ndx"));
    CHECK(pds_close_table("crash") == PDS_SUCCESS);

    /* 4. .ndx overwritten with garbage -> rebuilt */
    fp = fopen("crash.ndx", "wb");
    if (fp) { fputs("this is not an index", fp); fclose(fp); }
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NDX_CORRUPT);
    CHECK(check_recovered_state());
    CHECK(pds_close_table("crash") == PDS_SUCCESS);

    /* 5. torn write: half a frame of junk at the end of .dat */
    fp = fopen("crash.dat", "ab");
    if (fp) { fwrite("\x07\x07\x07\x07junkjunk", 1, 12, fp); fclose(fp); }
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NDX_STALE);
    CHECK(check_recovered_state());
    p = make_passenger(101, "After Tear");
    CHECK(pds_put_rec("crash", 101, &p) == PDS_SUCCESS);    /* lands on a frame boundary */
    CHECK(pds_close_table("crash") == PDS_SUCCESS);
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NONE);
    CHECK(pds_get_rec("crash", 101, &p) == PDS_SUCCESS && strcmp(p.name, "After Tear") == 0);
    CHECK(pds_get_rec("crash", 15, &p) == PDS_SUCCESS && strcmp(p.name, "Back Again") == 0);

    /* 6. fsync after every write, then crash: nothing acknowledged is lost */
    pds_set_sync_mode(PDS_SYNC_ALWAYS);
    CHECK(pds_get_sync_mode() == PDS_SYNC_ALWAYS);
    p = make_passenger(200, "Synced");
    CHECK(pds_put_rec("crash", 200, &p) == PDS_SUCCESS);
    CHECK(pds_delete_rec("crash", 101) == PDS_SUCCESS);
    CHECK(pds_simulate_crash("crash") == PDS_SUCCESS);
    CHECK(pds_open_table("crash") == PDS_SUCCESS);
    CHECK(recovery_of("crash") == PDS_RECOVERY_NDX_STALE);
    CHECK(pds_get_rec("crash", 200, &p) == PDS_SUCCESS && strcmp(p.name, "Synced") == 0);
    CHECK(pds_get_rec("crash", 101, &p) == PDS_REC_NOT_FOUND);
    pds_set_sync_mode(PDS_SYNC_NONE);
    CHECK(pds_close_table("crash") == PDS_SUCCESS);           /* also works without fsync */
    CHECK(pds_open_table("crash") == PDS_SUCCESS && recovery_of("crash") == PDS_RECOVERY_NONE);
    pds_set_sync_mode(99);                                     /* invalid: ignored */
    CHECK(pds_get_sync_mode() == PDS_SYNC_NONE);
    pds_set_sync_mode(PDS_SYNC_ON_CLOSE);

    CHECK(pds_close_db() == PDS_SUCCESS);
    CHECK(pds_drop_db(db) == PDS_SUCCESS);

    /* 7. a database from the previous file format is refused clearly */
    CHECK(pds_create_db(db) == PDS_SUCCESS);
    fp = fopen("skytest4/skytest4.info", "wb");
    if (fp) { fwrite("SBDB\0\0\0\0", 1, 8, fp); fclose(fp); }
    CHECK(pds_open_db(db) == PDS_OLD_FORMAT);
    CHECK(pds_db_info()->status == PDS_DB_CLOSED);
    CHECK(pds_drop_db(db) == PDS_SUCCESS);
}

/* ---- Route secondary index (SkyBook) ------------------------------------ */

static Flight make_flight(int id, const char *src, const char *dst)
{
    Flight f;
    memset(&f, 0, sizeof f);
    f.flight_id = id;
    snprintf(f.flight_no, sizeof f.flight_no, "SB%03d", id % 1000);
    copy_str(f.source, CODE_LEN, src);
    copy_str(f.dest, CODE_LEN, dst);
    copy_str(f.date, DATE_LEN, "2026-12-01");
    f.seats_total = 100;
    f.fare = 4000;
    return f;
}

typedef struct { int32_t ids[64]; int n; } IdList;

static int collect_ids(int32_t key, const void *rec, void *p)
{
    IdList *l = p;
    (void)rec;
    if (l->n < 64) l->ids[l->n++] = key;
    return 0;
}

static int route_count(const char *src, const char *dst)
{
    long m = -1;
    sb_find_route(src, dst, NULL, NULL, &m, NULL);
    return (int)m;
}

static void test_route_index(void)
{
    const char *db = "skytest3";
    static const char *const ap[] = { "DEL", "BOM", "BLR", "HYD" };
    Flight f;
    IdList l;
    RouteQuery q;
    long m, fetched, comps;
    int i, consistent = 1;

    printf("route index\n");
    pds_drop_db(db);
    CHECK(sb_create(db) == PDS_SUCCESS);
    CHECK(sb_open(db) == PDS_SUCCESS);
    CHECK(pds_create_secondary_index(SB_FLIGHTS, SB_ROUTE_INDEX, flight_route_key) == PDS_INDEX_EXISTS);
    CHECK(pds_get_by_secondary(SB_FLIGHTS, "nope", 0, NULL, NULL, &m, NULL) == PDS_INDEX_NOT_FOUND);

    CHECK(route_key("DEL", "BOM") != route_key("BOM", "DEL"));
    CHECK(route_key("DEL", "BOM") == ((int64_t)'D' << 40 | (int64_t)'E' << 32 | (int64_t)'L' << 24 |
                                      'B' << 16 | 'O' << 8 | 'M'));

    for (i = 1; i <= 120; i++) {                    /* 12 routes x 10 flights */
        int s = i % 4, d = (s + 1 + (i / 4) % 3) % 4;
        f = make_flight(i, ap[s], ap[d]);
        CHECK(sb_add_flight(&f) == PDS_SUCCESS);
    }
    l.n = 0;
    CHECK(sb_find_route("DEL", "BOM", collect_ids, &l, &m, &fetched) == PDS_SUCCESS);
    copy_str(q.source, CODE_LEN, "DEL");
    copy_str(q.dest, CODE_LEN, "BOM");
    {
        long scan_m;
        CHECK(pds_get_rec_by_field(SB_FLIGHTS, &q, match_flight_route, NULL, NULL, &scan_m, &comps) == PDS_SUCCESS);
        CHECK(m == scan_m && m == l.n && m > 0);
        CHECK(fetched == m);                        /* reads only the matching records */
        CHECK(comps == 120);                        /* the scan compares every record  */
    }

    /* every route agrees with a full scan */
    for (i = 0; i < 16; i++) {
        long scan_m;
        copy_str(q.source, CODE_LEN, ap[i / 4]);
        copy_str(q.dest, CODE_LEN, ap[i % 4]);
        pds_get_rec_by_field(SB_FLIGHTS, &q, match_flight_route, NULL, NULL, &scan_m, NULL);
        if (route_count(q.source, q.dest) != scan_m) consistent = 0;
    }
    CHECK(consistent);

    /* update moves a flight between routes; a fare change doesn't touch the index */
    {
        int before_from = route_count("DEL", "BOM"), before_to = route_count("HYD", "BLR");
        f = make_flight(l.ids[0], "HYD", "BLR");
        CHECK(pds_update_rec(SB_FLIGHTS, l.ids[0], &f) == PDS_SUCCESS);
        CHECK(route_count("DEL", "BOM") == before_from - 1);
        CHECK(route_count("HYD", "BLR") == before_to + 1);
        f.fare = 9999;
        CHECK(pds_update_rec(SB_FLIGHTS, l.ids[0], &f) == PDS_SUCCESS);
        CHECK(route_count("HYD", "BLR") == before_to + 1);

        /* delete removes it; re-insert adds it back */
        CHECK(sb_delete_flight(l.ids[0]) == PDS_SUCCESS);
        CHECK(route_count("HYD", "BLR") == before_to);
        f = make_flight(l.ids[0], "GOI", "PNQ");
        CHECK(sb_add_flight(&f) == PDS_SUCCESS);
        CHECK(route_count("GOI", "PNQ") == 1);
    }
    CHECK(route_count("XXX", "YYY") == 0);

    /* not persisted, but rebuilt on open: same answers after reopen */
    {
        int del_bom = route_count("DEL", "BOM"), goi_pnq = route_count("GOI", "PNQ");
        CHECK(sb_close() == PDS_SUCCESS);
        CHECK(sb_open(db) == PDS_SUCCESS);
        CHECK(route_count("DEL", "BOM") == del_bom && route_count("GOI", "PNQ") == goi_pnq);
    }
    CHECK(sb_close() == PDS_SUCCESS);
    CHECK(pds_drop_db(db) == PDS_SUCCESS);
}

/* ---- SkyBook rules ------------------------------------------------------ */

static void test_skybook(void)
{
    const char *db = "skytest2";
    Flight f, got;
    Booking b1, b2, b3;
    Passenger p;
    int i;

    printf("skybook\n");
    pds_drop_db(db);
    CHECK(sb_create(db) == PDS_SUCCESS);
    CHECK(sb_open(db) == PDS_SUCCESS);

    memset(&f, 0, sizeof f);
    f.flight_id = 1;
    copy_str(f.flight_no, FLIGHT_NO_LEN, "SB001");
    copy_str(f.source, CODE_LEN, "DEL");
    copy_str(f.dest, CODE_LEN, "BOM");
    copy_str(f.date, DATE_LEN, "2026-12-01");
    f.seats_total = 2;
    f.fare = 4000;
    CHECK(sb_add_flight(&f) == PDS_SUCCESS);
    f.flight_id = 2; f.seats_total = 0;
    CHECK(sb_add_flight(&f) == SB_INVALID_FLIGHT);

    for (i = 1; i <= 3; i++) {
        p = make_passenger(i, "Traveller");
        CHECK(sb_add_passenger(&p) == PDS_SUCCESS);
    }
    p = make_passenger(4, "Doe, Jane");
    CHECK(sb_add_passenger(&p) == SB_INVALID_PASSENGER);
    p = make_passenger(4, "");
    CHECK(sb_add_passenger(&p) == SB_INVALID_PASSENGER);
    f.flight_id = 2; f.seats_total = 10;
    copy_str(f.date, DATE_LEN, "12,25");
    CHECK(sb_add_flight(&f) == SB_INVALID_FLIGHT);
    copy_str(f.date, DATE_LEN, "2026-12-01");

    CHECK(sb_book(1, 1, &b1) == PDS_SUCCESS && b1.booking_id == 1 && b1.fare_paid == 4000);
    CHECK(sb_book(1, 1, NULL) == SB_ALREADY_BOOKED);
    CHECK(sb_book(1, 2, &b2) == PDS_SUCCESS && b2.booking_id == 2);
    CHECK(sb_book(1, 3, NULL) == SB_FLIGHT_FULL);
    CHECK(sb_book(9, 3, NULL) == SB_NO_FLIGHT);
    CHECK(sb_book(1, 9, NULL) == SB_FLIGHT_FULL);   /* capacity checked before passenger */
    CHECK(pds_get_rec(SB_FLIGHTS, 1, &got) == PDS_SUCCESS && got.seats_booked == 2);

    CHECK(sb_cancel(1) == PDS_SUCCESS);
    CHECK(sb_cancel(1) == PDS_REC_NOT_FOUND);
    CHECK(pds_get_rec(SB_FLIGHTS, 1, &got) == PDS_SUCCESS && got.seats_booked == 1);
    CHECK(sb_book(1, 9, NULL) == SB_NO_PASSENGER);
    CHECK(sb_book(1, 3, &b3) == PDS_SUCCESS && b3.booking_id == 3);   /* id 1 never reused */
    CHECK(sb_book(1, 1, NULL) == SB_FLIGHT_FULL);

    CHECK(sb_delete_flight(1) == SB_HAS_BOOKINGS);
    CHECK(sb_cancel(2) == PDS_SUCCESS && sb_cancel(3) == PDS_SUCCESS);
    CHECK(sb_delete_flight(1) == PDS_SUCCESS);
    CHECK(sb_delete_flight(1) == SB_NO_FLIGHT);

    CHECK(sb_close() == PDS_SUCCESS);
    CHECK(sb_open(db) == PDS_SUCCESS);
    CHECK(pds_get_rec(SB_FLIGHTS, 1, &got) == PDS_REC_NOT_FOUND);
    CHECK(pds_get_rec(SB_PASSENGERS, 3, &p) == PDS_SUCCESS);
    CHECK(sb_close() == PDS_SUCCESS);
    CHECK(pds_drop_db(db) == PDS_SUCCESS);
}

int main(void)
{
    test_hash_open();
    test_hash_chain();
    test_array_index();
    test_bst();
    test_secindex();
    test_pds();
    test_csv();
    test_recovery();
    test_route_index();
    test_skybook();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
