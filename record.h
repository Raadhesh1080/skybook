/*
 * record.h - SkyBook record model.
 *
 * Every record is a fixed-size struct. Fixed size is what makes byte-offset
 * random access possible: a record lives at a known offset in the .dat file
 * and can be read or overwritten in place with a single fseek.
 */
#ifndef RECORD_H
#define RECORD_H

#include <stddef.h>
#include <stdint.h>

#define CODE_LEN      4    /* 3-letter IATA airport code + NUL */
#define FLIGHT_NO_LEN 8
#define DATE_LEN      12   /* YYYY-MM-DD + NUL */
#define NAME_LEN      40
#define PASSPORT_LEN  12
#define PHONE_LEN     16

typedef struct {
    int32_t flight_id;              /* primary key */
    char    flight_no[FLIGHT_NO_LEN];
    char    source[CODE_LEN];
    char    dest[CODE_LEN];
    char    date[DATE_LEN];
    int32_t seats_total;
    int32_t seats_booked;
    int32_t fare;                   /* in rupees */
} Flight;

typedef struct {
    int32_t passenger_id;           /* primary key */
    char    name[NAME_LEN];
    char    passport_no[PASSPORT_LEN];
    char    phone[PHONE_LEN];
} Passenger;

typedef struct {
    int32_t booking_id;             /* primary key */
    int32_t flight_id;
    int32_t passenger_id;
    int32_t fare_paid;
} Booking;

/* ---- Matcher callbacks used by pds_get_rec_by_field ----------------------
 * Convention (same for every matcher):
 *   MATCH_FOUND (0)     record matches the query
 *   MATCH_NOT_FOUND (1) record does not match
 *   MATCH_ERROR (>1)    bad arguments (e.g. NULL pointers)
 */
#define MATCH_FOUND     0
#define MATCH_NOT_FOUND 1
#define MATCH_ERROR     2

typedef struct {
    char source[CODE_LEN];
    char dest[CODE_LEN];
} RouteQuery;

typedef struct {
    int32_t flight_id;
    int32_t passenger_id;
} BookingPairQuery;

int match_flight_route(const void *rec, const void *query);      /* RouteQuery*      */
int match_flight_date(const void *rec, const void *query);       /* const char* date */
int match_passenger_name(const void *rec, const void *query);    /* const char* name, case-insensitive */
int match_passenger_passport(const void *rec, const void *query);/* const char*      */
int match_booking_flight(const void *rec, const void *query);    /* int32_t*         */
int match_booking_passenger(const void *rec, const void *query); /* int32_t*         */
int match_booking_pair(const void *rec, const void *query);      /* BookingPairQuery* */

/* ---- Secondary keys ------------------------------------------------------
 * A route is packed into one int64: the six bytes of the two 3-letter codes
 * (48 bits), so different routes always get different keys. */
int64_t route_key(const char *source, const char *dest);
int64_t flight_route_key(const void *rec);   /* si_key_fn for the "route" index */

/* ---- Text (CSV) representation -------------------------------------------
 * One record per line, fields separated by commas, no quoting - so string
 * fields must not contain ',' or line breaks (see csv_safe).
 * *_to_csv:   0 on success, 1 if a field can't be written or the line is too long
 * *_from_csv: 0 on success, 1 if the line is malformed; *key gets the record's key
 */
#define FLIGHT_CSV_HEADER    "flight_id,flight_no,source,dest,date,seats_total,seats_booked,fare"
#define PASSENGER_CSV_HEADER "passenger_id,name,passport_no,phone"
#define BOOKING_CSV_HEADER   "booking_id,flight_id,passenger_id,fare_paid"

int csv_safe(const char *s);

int flight_to_csv(const void *rec, char *line, size_t size);
int flight_from_csv(const char *line, void *rec, int32_t *key);
int passenger_to_csv(const void *rec, char *line, size_t size);
int passenger_from_csv(const char *line, void *rec, int32_t *key);
int booking_to_csv(const void *rec, char *line, size_t size);
int booking_from_csv(const char *line, void *rec, int32_t *key);

/* ---- Helpers ------------------------------------------------------------ */
void copy_str(char *dst, size_t size, const char *src);
void upper_str(char *s);
void print_flight(const Flight *f);
void print_passenger(const Passenger *p);
void print_booking(const Booking *b);

#endif
