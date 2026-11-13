/*
 * main.c - SkyBook interactive CLI.
 *
 *   skybook              interactive menu
 *   skybook --bench [N]  run the benchmark with N flights (default 100000)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "skybook.h"

/* ---- Input helpers ------------------------------------------------------ */

static int read_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

static int read_int(const char *prompt, int *out)
{
    char buf[64], *end;
    for (;;) {
        long v;
        if (!read_line(prompt, buf, sizeof buf)) return 0;
        v = strtol(buf, &end, 10);
        if (end != buf && *end == '\0') {
            *out = (int)v;
            return 1;
        }
        printf("  Please enter a whole number.\n");
    }
}

static int read_code(const char *prompt, char *code)
{
    char buf[32];
    for (;;) {
        if (!read_line(prompt, buf, sizeof buf)) return 0;
        if (strlen(buf) == 3) {
            upper_str(buf);
            copy_str(code, CODE_LEN, buf);
            return 1;
        }
        printf("  Airport codes are 3 letters, e.g. DEL.\n");
    }
}

static void report(int rc, const char *ok_msg)
{
    if (rc == PDS_SUCCESS) printf("  %s\n", ok_msg);
    else printf("  Error: %s\n", sb_strerror(rc));
}

static int require_open(void)
{
    if (pds_db_info()->status == PDS_DB_OPEN) return 1;
    printf("  Open a database first (option 2).\n");
    return 0;
}

/* ---- Visitors used by scans --------------------------------------------- */

static int show_flight(int32_t key, const void *rec, void *ctx)
{
    (void)key; (void)ctx;
    print_flight(rec);
    return 0;
}

static int show_passenger(int32_t key, const void *rec, void *ctx)
{
    (void)key; (void)ctx;
    print_passenger(rec);
    return 0;
}

static int show_booking(int32_t key, const void *rec, void *ctx)
{
    (void)key; (void)ctx;
    print_booking(rec);
    return 0;
}

/* ---- Sample data -------------------------------------------------------- */

static void load_sample_data(void)
{
    static const char *const airports[] = { "DEL", "BOM", "BLR", "HYD", "MAA", "CCU" };
    static const char *const names[] = {
        "Aarav Sharma", "Diya Patel", "Rohan Iyer", "Ananya Reddy", "Kabir Singh",
        "Meera Nair", "Vihaan Gupta", "Isha Menon", "Arjun Rao", "Saanvi Joshi"
    };
    int i, added = 0;

    for (i = 0; i < 20; i++) {
        Flight f;
        int s = i % 6, d = (s + 1 + i / 6) % 6;
        memset(&f, 0, sizeof f);
        f.flight_id = 101 + i;
        snprintf(f.flight_no, sizeof f.flight_no, "SB%03d", 101 + i);
        copy_str(f.source, CODE_LEN, airports[s]);
        copy_str(f.dest, CODE_LEN, airports[d]);
        snprintf(f.date, sizeof f.date, "2026-12-%02d", 1 + i % 10);
        f.seats_total = (i % 4 == 0) ? 2 : 180;     /* a few tiny flights to show "full" */
        f.fare = 3500 + 250 * i;
        if (sb_add_flight(&f) == PDS_SUCCESS) added++;
    }
    for (i = 0; i < 10; i++) {
        Passenger p;
        memset(&p, 0, sizeof p);
        p.passenger_id = i + 1;
        copy_str(p.name, NAME_LEN, names[i]);
        snprintf(p.passport_no, sizeof p.passport_no, "P%07d", 4500000 + i * 137);
        snprintf(p.phone, sizeof p.phone, "98450%05d", 10000 + i * 911);
        if (sb_add_passenger(&p) == PDS_SUCCESS) added++;
    }
    printf("  Added %d sample records (flights 101-120, passengers 1-10).\n", added);
    printf("  Records whose IDs already existed were skipped.\n");
}

/* ---- Menu actions ------------------------------------------------------- */

static void do_add_flight(void)
{
    Flight f;
    int v;
    char buf[64];

    memset(&f, 0, sizeof f);
    if (!read_int("  Flight ID: ", &v)) return;
    f.flight_id = v;
    if (!read_line("  Flight number (e.g. SB204): ", buf, sizeof buf)) return;
    upper_str(buf);
    copy_str(f.flight_no, FLIGHT_NO_LEN, buf);
    if (!read_code("  From (airport code): ", f.source)) return;
    if (!read_code("  To   (airport code): ", f.dest)) return;
    if (!read_line("  Date (YYYY-MM-DD): ", buf, sizeof buf)) return;
    copy_str(f.date, DATE_LEN, buf);
    if (!read_int("  Total seats: ", &v)) return;
    f.seats_total = v;
    if (!read_int("  Fare (Rs): ", &v)) return;
    f.fare = v;
    report(sb_add_flight(&f), "Flight added.");
}

static void do_add_passenger(void)
{
    Passenger p;
    int v;
    char buf[64];

    memset(&p, 0, sizeof p);
    if (!read_int("  Passenger ID: ", &v)) return;
    p.passenger_id = v;
    if (!read_line("  Full name: ", buf, sizeof buf)) return;
    copy_str(p.name, NAME_LEN, buf);
    if (!read_line("  Passport number: ", buf, sizeof buf)) return;
    upper_str(buf);
    copy_str(p.passport_no, PASSPORT_LEN, buf);
    if (!read_line("  Phone: ", buf, sizeof buf)) return;
    copy_str(p.phone, PHONE_LEN, buf);
    report(sb_add_passenger(&p), "Passenger added.");
}

static void do_book(void)
{
    int fid, pid, rc;
    Booking b;
    if (!read_int("  Flight ID: ", &fid) || !read_int("  Passenger ID: ", &pid)) return;
    rc = sb_book(fid, pid, &b);
    if (rc == PDS_SUCCESS) {
        printf("  Booked.\n");
        print_booking(&b);
    } else {
        report(rc, "");
    }
}

static void do_get(const char *table, const char *prompt)
{
    union { Flight f; Passenger p; Booking b; } rec;
    int id, rc;
    if (!read_int(prompt, &id)) return;
    rc = pds_get_rec(table, id, &rec);
    if (rc != PDS_SUCCESS) {
        report(rc, "");
    } else if (strcmp(table, SB_FLIGHTS) == 0) {
        print_flight(&rec.f);
    } else if (strcmp(table, SB_PASSENGERS) == 0) {
        print_passenger(&rec.p);
    } else {
        print_booking(&rec.b);
    }
}

static void do_search_route(int use_index)
{
    RouteQuery q;
    long matches = 0, work = 0;
    int rc;
    if (!read_code("  From (airport code): ", q.source)) return;
    if (!read_code("  To   (airport code): ", q.dest)) return;
    if (use_index) {
        rc = sb_find_route(q.source, q.dest, show_flight, NULL, &matches, &work);
        if (rc != PDS_SUCCESS) report(rc, "");
        else printf("  %ld flight(s) found via the route index, %ld record(s) read.\n", matches, work);
    } else {
        rc = pds_get_rec_by_field(SB_FLIGHTS, &q, match_flight_route, show_flight, NULL,
                                  &matches, &work);
        if (rc != PDS_SUCCESS) report(rc, "");
        else printf("  %ld flight(s) found, %ld comparisons (full scan).\n", matches, work);
    }
}

static void do_search_name(void)
{
    char name[NAME_LEN];
    long matches = 0, comps = 0;
    int rc;
    if (!read_line("  Name: ", name, sizeof name)) return;
    rc = pds_get_rec_by_field(SB_PASSENGERS, name, match_passenger_name, show_passenger, NULL,
                              &matches, &comps);
    if (rc != PDS_SUCCESS) report(rc, "");
    else printf("  %ld passenger(s) found, %ld comparisons (full scan).\n", matches, comps);
}

static void do_bookings_for_flight(void)
{
    int fid, rc;
    int32_t key;
    long matches = 0, comps = 0;
    if (!read_int("  Flight ID: ", &fid)) return;
    key = fid;
    rc = pds_get_rec_by_field(SB_BOOKINGS, &key, match_booking_flight, show_booking, NULL,
                              &matches, &comps);
    if (rc != PDS_SUCCESS) report(rc, "");
    else printf("  %ld booking(s), %ld comparisons (full scan).\n", matches, comps);
}

static void do_range(void)
{
    int lo, hi, rc;
    long count = 0;
    if (!read_int("  From flight ID: ", &lo) || !read_int("  To flight ID:   ", &hi)) return;
    rc = pds_scan_range(SB_FLIGHTS, lo, hi, show_flight, NULL, &count);
    if (rc != PDS_SUCCESS) report(rc, "");
    else printf("  %ld flight(s) in key order (BST range scan).\n", count);
}

static void do_update_fare(void)
{
    Flight f;
    int id, fare, rc;
    if (!read_int("  Flight ID: ", &id)) return;
    rc = pds_get_rec(SB_FLIGHTS, id, &f);
    if (rc != PDS_SUCCESS) { report(rc, ""); return; }
    print_flight(&f);
    if (!read_int("  New fare (Rs): ", &fare)) return;
    if (fare < 0) { printf("  Fare can't be negative.\n"); return; }
    f.fare = fare;
    report(pds_update_rec(SB_FLIGHTS, id, &f), "Fare updated in place.");
}

static void do_stats(void)
{
    static const char *const tables[] = { SB_FLIGHTS, SB_PASSENGERS, SB_BOOKINGS };
    int i;
    static const char *const modes[] = { "none", "on close", "every write" };
    printf("  %-11s %6s %8s %10s %9s %7s %6s %10s %10s %4s\n", "table", "live", "deleted",
           ".dat bytes", "hash cap", "used", "tombs", "AVL height", "avg depth", "2nd");
    for (i = 0; i < 3; i++) {
        PDS_TableStats s;
        if (pds_table_stats(tables[i], &s) != PDS_SUCCESS) continue;
        printf("  %-11s %6ld %8ld %10lld %9zu %7zu %6zu %10d %10.2f %4d\n", tables[i], s.live,
               s.deleted, (long long)s.data_bytes, s.hash_cap, s.hash_used, s.hash_tombs,
               s.bst_height, s.bst_avg_depth, s.num_secondary);
    }
    for (i = 0; i < 3; i++) {
        PDS_TableStats s;
        if (pds_table_stats(tables[i], &s) == PDS_SUCCESS)
            printf("  %-11s index %s\n", tables[i], pds_recovery_name(s.recovery));
    }
    printf("  fsync mode: %s\n", modes[pds_get_sync_mode()]);
}

static void report_open(int rc)
{
    static const char *const tables[] = { SB_FLIGHTS, SB_PASSENGERS, SB_BOOKINGS };
    int i;
    if (rc != PDS_SUCCESS) {
        report(rc, "");
        return;
    }
    printf("  Database opened; route index built.\n");
    for (i = 0; i < 3; i++) {
        PDS_TableStats s;
        if (pds_table_stats(tables[i], &s) == PDS_SUCCESS && s.recovery != PDS_RECOVERY_NONE)
            printf("  Recovery: %s index %s.\n", tables[i], pds_recovery_name(s.recovery));
    }
}

static void do_sync_mode(void)
{
    int m;
    printf("  0 = never fsync (fastest)\n"
           "  1 = fsync when tables are closed (default)\n"
           "  2 = fsync after every write (safest, slowest)\n");
    if (!read_int("  Mode: ", &m)) return;
    if (m < PDS_SYNC_NONE || m > PDS_SYNC_ALWAYS) { printf("  Unknown mode.\n"); return; }
    pds_set_sync_mode(m);
    printf("  fsync mode set.\n");
}

/* ---- Text (CSV) export / import ----------------------------------------- */

typedef struct {
    const char *table;
    const char *header;
    pds_to_text_fn to_text;
    pds_from_text_fn from_text;
} TextFormat;

static const TextFormat FORMATS[] = {
    { SB_FLIGHTS,    FLIGHT_CSV_HEADER,    flight_to_csv,    flight_from_csv },
    { SB_PASSENGERS, PASSENGER_CSV_HEADER, passenger_to_csv, passenger_from_csv },
    { SB_BOOKINGS,   BOOKING_CSV_HEADER,   booking_to_csv,   booking_from_csv },
};

static const TextFormat *pick_table(char *path, size_t path_size)
{
    char buf[64];
    int i;
    if (!read_line("  Table (flights / passengers / bookings): ", buf, sizeof buf)) return NULL;
    for (i = 0; i < 3; i++) {
        if (strcmp(buf, FORMATS[i].table) == 0) {
            char prompt[128];
            snprintf(prompt, sizeof prompt,
                     "  CSV file, relative to the database folder [%s.csv]: ", buf);
            if (!read_line(prompt, path, path_size)) return NULL;
            if (path[0] == '\0') snprintf(path, path_size, "%s.csv", buf);
            return &FORMATS[i];
        }
    }
    printf("  Unknown table.\n");
    return NULL;
}

static void do_export(void)
{
    char path[256];
    long count = 0;
    int rc;
    const TextFormat *f = pick_table(path, sizeof path);
    if (!f) return;
    rc = pds_export_text(f->table, path, f->header, f->to_text, &count);
    if (rc != PDS_SUCCESS) report(rc, "");
    else printf("  Exported %ld live record(s) in storage order to %s (text mode).\n", count, path);
}

static void do_import(void)
{
    char path[256];
    long imported = 0, skipped = 0;
    int rc;
    const TextFormat *f = pick_table(path, sizeof path);
    if (!f) return;
    rc = pds_import_text(f->table, path, 1, f->from_text, &imported, &skipped);
    if (rc != PDS_SUCCESS) report(rc, "");
    else printf("  Imported %ld record(s); skipped %ld (malformed or key already exists).\n"
                "  Note: import restores raw rows; it does not re-check booking rules.\n",
                imported, skipped);
}

static void do_scan_flight(void)
{
    Flight f;
    long comps = 0;
    int id, rc;
    if (!read_int("  Flight ID: ", &id)) return;
    rc = pds_get_rec_by_scan(SB_FLIGHTS, id, &f, &comps);
    if (rc == PDS_SUCCESS) print_flight(&f);
    else report(rc, "");
    printf("  %ld frame(s) compared reading the .dat file from the start (no index).\n", comps);
}

static void print_menu(void)
{
    const PDS_DBInfo *db = pds_db_info();
    printf("\n==================== SkyBook ====================\n");
    if (db->status == PDS_DB_OPEN) printf("  database: %s (open)\n", db->name);
    else printf("  database: none open\n");
    printf("  1. Create database          2. Open database\n"
           "  3. Load sample data\n"
           "  4. Add flight               5. Add passenger\n"
           "  6. Book a seat              7. Cancel booking\n"
           "  8. Find flight by ID        9. Find passenger by ID\n"
           " 10. Find booking by ID\n"
           " 11. Search flights by route (route index)\n"
           " 12. Search passengers by name (field scan)\n"
           " 13. Bookings for a flight (field scan)\n"
           " 14. List flights in ID range (AVL tree)\n"
           " 15. Update flight fare      16. Delete flight\n"
           " 17. Table statistics        18. Run benchmark\n"
           " 19. Close database\n"
           " 20. Export table to CSV     21. Import table from CSV\n"
           " 22. Find flight by ID without the index (linear scan)\n"
           " 23. Search flights by route without the index (full scan)\n"
           " 24. Set fsync (durability) mode\n"
           "  0. Exit\n");
}

static void interactive(void)
{
    char name[64];
    int choice, id;

    for (;;) {
        print_menu();
        if (!read_int("Choice: ", &choice)) break;

        switch (choice) {
        case 0:
            if (pds_db_info()->status == PDS_DB_OPEN) report(sb_close(), "Database closed.");
            printf("Goodbye.\n");
            return;
        case 1:
            if (read_line("  New database name: ", name, sizeof name))
                report(sb_create(name), "Database created with tables flights, passengers, bookings.");
            break;
        case 2:
            if (read_line("  Database name: ", name, sizeof name))
                report_open(sb_open(name));
            break;
        case 24:
            do_sync_mode();
            break;
        case 19:
            report(sb_close(), "Database closed; indexes flushed to .ndx files.");
            break;
        case 18:
            if (pds_db_info()->status == PDS_DB_OPEN) {
                printf("  Close the database first (option 19).\n");
            } else if (read_int("  Number of flights (e.g. 100000): ", &id)) {
                run_benchmark(id);
            }
            break;
        default:
            if (choice < 0 || choice > 24) { printf("  Unknown option.\n"); break; }
            if (!require_open()) break;
            switch (choice) {
            case 3:  load_sample_data(); break;
            case 4:  do_add_flight(); break;
            case 5:  do_add_passenger(); break;
            case 6:  do_book(); break;
            case 7:
                if (read_int("  Booking ID: ", &id)) report(sb_cancel(id), "Booking cancelled.");
                break;
            case 8:  do_get(SB_FLIGHTS, "  Flight ID: "); break;
            case 9:  do_get(SB_PASSENGERS, "  Passenger ID: "); break;
            case 10: do_get(SB_BOOKINGS, "  Booking ID: "); break;
            case 11: do_search_route(1); break;
            case 23: do_search_route(0); break;
            case 12: do_search_name(); break;
            case 13: do_bookings_for_flight(); break;
            case 14: do_range(); break;
            case 15: do_update_fare(); break;
            case 16:
                if (read_int("  Flight ID: ", &id)) report(sb_delete_flight(id), "Flight deleted.");
                break;
            case 17: do_stats(); break;
            case 20: do_export(); break;
            case 21: do_import(); break;
            case 22: do_scan_flight(); break;
            }
        }
    }

    /* stdin closed: make sure indexes still get flushed */
    if (pds_db_info()->status == PDS_DB_OPEN) sb_close();
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--bench") == 0) {
        long n = argc >= 3 ? strtol(argv[2], NULL, 10) : 100000;
        return run_benchmark(n);
    }
    if (argc >= 2) {
        fprintf(stderr, "usage: %s [--bench [N]]\n", argv[0]);
        return 2;
    }
    interactive();
    return 0;
}
