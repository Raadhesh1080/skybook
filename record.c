/*
 * record.c - SkyBook record helpers and matcher callbacks.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "record.h"

void copy_str(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    if (!src) src = "";
    snprintf(dst, size, "%s", src);
}

void upper_str(char *s)
{
    for (; s && *s; s++) *s = (char)toupper((unsigned char)*s);
}

static int equals_ignore_case(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- Flight matchers ---------------------------------------------------- */

int match_flight_route(const void *rec, const void *query)
{
    const Flight *f = rec;
    const RouteQuery *q = query;
    if (!f || !q) return MATCH_ERROR;
    return (strcmp(f->source, q->source) == 0 && strcmp(f->dest, q->dest) == 0)
               ? MATCH_FOUND : MATCH_NOT_FOUND;
}

int match_flight_date(const void *rec, const void *query)
{
    const Flight *f = rec;
    const char *date = query;
    if (!f || !date) return MATCH_ERROR;
    return strcmp(f->date, date) == 0 ? MATCH_FOUND : MATCH_NOT_FOUND;
}

/* ---- Passenger matchers ------------------------------------------------- */

int match_passenger_name(const void *rec, const void *query)
{
    const Passenger *p = rec;
    const char *name = query;
    if (!p || !name) return MATCH_ERROR;
    return equals_ignore_case(p->name, name) ? MATCH_FOUND : MATCH_NOT_FOUND;
}

int match_passenger_passport(const void *rec, const void *query)
{
    const Passenger *p = rec;
    const char *passport = query;
    if (!p || !passport) return MATCH_ERROR;
    return strcmp(p->passport_no, passport) == 0 ? MATCH_FOUND : MATCH_NOT_FOUND;
}

/* ---- Booking matchers --------------------------------------------------- */

int match_booking_flight(const void *rec, const void *query)
{
    const Booking *b = rec;
    const int32_t *flight_id = query;
    if (!b || !flight_id) return MATCH_ERROR;
    return b->flight_id == *flight_id ? MATCH_FOUND : MATCH_NOT_FOUND;
}

int match_booking_passenger(const void *rec, const void *query)
{
    const Booking *b = rec;
    const int32_t *passenger_id = query;
    if (!b || !passenger_id) return MATCH_ERROR;
    return b->passenger_id == *passenger_id ? MATCH_FOUND : MATCH_NOT_FOUND;
}

int match_booking_pair(const void *rec, const void *query)
{
    const Booking *b = rec;
    const BookingPairQuery *q = query;
    if (!b || !q) return MATCH_ERROR;
    return (b->flight_id == q->flight_id && b->passenger_id == q->passenger_id)
               ? MATCH_FOUND : MATCH_NOT_FOUND;
}

/* ---- Secondary keys ----------------------------------------------------- */

int64_t route_key(const char *source, const char *dest)
{
    int64_t k = 0;
    int i;
    for (i = 0; i < 3; i++) k = (k << 8) | (unsigned char)(source && source[0] ? source[i] : 0);
    for (i = 0; i < 3; i++) k = (k << 8) | (unsigned char)(dest && dest[0] ? dest[i] : 0);
    return k;
}

int64_t flight_route_key(const void *rec)
{
    const Flight *f = rec;
    return route_key(f->source, f->dest);
}

/* ---- Text (CSV) conversion ---------------------------------------------- */

int csv_safe(const char *s)
{
    return s && strpbrk(s, ",\r\n") == NULL;
}

/* Splits buf in place on commas. Returns the number of fields, or -1 if
 * there are more than max. Empty fields are allowed. */
static int split_fields(char *buf, char **fields, int max)
{
    int n = 0;
    char *p = buf;
    for (;;) {
        char *comma;
        if (n == max) return -1;
        fields[n++] = p;
        comma = strchr(p, ',');
        if (!comma) return n;
        *comma = '\0';
        p = comma + 1;
    }
}

static int parse_int(const char *s, int32_t *out)
{
    char *end;
    long v;
    if (!*s) return 0;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < INT32_MIN || v > INT32_MAX) return 0;
    *out = (int32_t)v;
    return 1;
}

static int parse_str(const char *s, char *dst, size_t size)
{
    if (strlen(s) >= size) return 0;
    copy_str(dst, size, s);
    return 1;
}

/* Copies line into a scratch buffer and splits it; expects exactly n fields. */
#define SPLIT_OR_FAIL(line, buf, fields, n)                                   \
    do {                                                                      \
        if (!(line) || strlen(line) >= sizeof(buf)) return 1;                 \
        strcpy((buf), (line));                                                \
        if (split_fields((buf), (fields), (n)) != (n)) return 1;              \
    } while (0)

static int fits(int written, size_t size)
{
    return written >= 0 && (size_t)written < size;
}

int flight_to_csv(const void *rec, char *line, size_t size)
{
    const Flight *f = rec;
    if (!f || !csv_safe(f->flight_no) || !csv_safe(f->source) || !csv_safe(f->dest) ||
        !csv_safe(f->date)) {
        return 1;
    }
    return fits(snprintf(line, size, "%d,%s,%s,%s,%s,%d,%d,%d", f->flight_id, f->flight_no,
                         f->source, f->dest, f->date, f->seats_total, f->seats_booked,
                         f->fare), size) ? 0 : 1;
}

int flight_from_csv(const char *line, void *rec, int32_t *key)
{
    Flight *f = rec;
    char buf[256], *v[8];
    SPLIT_OR_FAIL(line, buf, v, 8);
    memset(f, 0, sizeof *f);
    if (!parse_int(v[0], &f->flight_id) || !parse_str(v[1], f->flight_no, FLIGHT_NO_LEN) ||
        !parse_str(v[2], f->source, CODE_LEN) || !parse_str(v[3], f->dest, CODE_LEN) ||
        !parse_str(v[4], f->date, DATE_LEN) || !parse_int(v[5], &f->seats_total) ||
        !parse_int(v[6], &f->seats_booked) || !parse_int(v[7], &f->fare)) {
        return 1;
    }
    if (key) *key = f->flight_id;
    return 0;
}

int passenger_to_csv(const void *rec, char *line, size_t size)
{
    const Passenger *p = rec;
    if (!p || !csv_safe(p->name) || !csv_safe(p->passport_no) || !csv_safe(p->phone)) return 1;
    return fits(snprintf(line, size, "%d,%s,%s,%s", p->passenger_id, p->name, p->passport_no,
                         p->phone), size) ? 0 : 1;
}

int passenger_from_csv(const char *line, void *rec, int32_t *key)
{
    Passenger *p = rec;
    char buf[256], *v[4];
    SPLIT_OR_FAIL(line, buf, v, 4);
    memset(p, 0, sizeof *p);
    if (!parse_int(v[0], &p->passenger_id) || !parse_str(v[1], p->name, NAME_LEN) ||
        !parse_str(v[2], p->passport_no, PASSPORT_LEN) || !parse_str(v[3], p->phone, PHONE_LEN)) {
        return 1;
    }
    if (key) *key = p->passenger_id;
    return 0;
}

int booking_to_csv(const void *rec, char *line, size_t size)
{
    const Booking *b = rec;
    if (!b) return 1;
    return fits(snprintf(line, size, "%d,%d,%d,%d", b->booking_id, b->flight_id,
                         b->passenger_id, b->fare_paid), size) ? 0 : 1;
}

int booking_from_csv(const char *line, void *rec, int32_t *key)
{
    Booking *b = rec;
    char buf[128], *v[4];
    SPLIT_OR_FAIL(line, buf, v, 4);
    memset(b, 0, sizeof *b);
    if (!parse_int(v[0], &b->booking_id) || !parse_int(v[1], &b->flight_id) ||
        !parse_int(v[2], &b->passenger_id) || !parse_int(v[3], &b->fare_paid)) {
        return 1;
    }
    if (key) *key = b->booking_id;
    return 0;
}

/* ---- Printing ----------------------------------------------------------- */

void print_flight(const Flight *f)
{
    printf("  Flight #%-6d %-7s %s -> %s  %s  seats %3d/%-3d  fare Rs %d\n",
           f->flight_id, f->flight_no, f->source, f->dest, f->date,
           f->seats_booked, f->seats_total, f->fare);
}

void print_passenger(const Passenger *p)
{
    printf("  Passenger #%-6d %-25s passport %-10s phone %s\n",
           p->passenger_id, p->name, p->passport_no, p->phone);
}

void print_booking(const Booking *b)
{
    printf("  Booking #%-6d flight #%-6d passenger #%-6d paid Rs %d\n",
           b->booking_id, b->flight_id, b->passenger_id, b->fare_paid);
}
