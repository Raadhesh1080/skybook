/*
 * skybook.c - Airline reservation layer.
 */
#include <string.h>

#include "skybook.h"

static const char *const TABLES[] = { SB_FLIGHTS, SB_PASSENGERS, SB_BOOKINGS };
static const int32_t SIZES[] = { sizeof(Flight), sizeof(Passenger), sizeof(Booking) };
#define NUM_TABLES 3

int sb_create(const char *db_name)
{
    int i, rc = pds_create_db(db_name);
    if (rc != PDS_SUCCESS) return rc;
    rc = pds_open_db(db_name);
    if (rc != PDS_SUCCESS) return rc;
    for (i = 0; i < NUM_TABLES && rc == PDS_SUCCESS; i++) {
        rc = pds_create_table(TABLES[i], SIZES[i]);
    }
    pds_close_db();
    return rc;
}

int sb_open(const char *db_name)
{
    int i, rc = pds_open_db(db_name);
    if (rc != PDS_SUCCESS) return rc;
    for (i = 0; i < NUM_TABLES; i++) {
        rc = pds_open_table(TABLES[i]);
        if (rc != PDS_SUCCESS) {
            pds_close_db();     /* closes whichever tables did open */
            return rc;
        }
    }
    /* Secondary indexes live in memory only: rebuild on every open. */
    rc = pds_create_secondary_index(SB_FLIGHTS, SB_ROUTE_INDEX, flight_route_key);
    if (rc != PDS_SUCCESS) {
        pds_close_db();
        return rc;
    }
    return PDS_SUCCESS;
}

int sb_find_route(const char *source, const char *dest, pds_visit_fn visit, void *ctx,
                  long *matches, long *fetched)
{
    return pds_get_by_secondary(SB_FLIGHTS, SB_ROUTE_INDEX, route_key(source, dest),
                                visit, ctx, matches, fetched);
}

int sb_close(void)
{
    return pds_close_db();
}

int sb_add_flight(const Flight *f)
{
    if (!f || f->seats_total <= 0 || f->seats_booked < 0 ||
        f->seats_booked > f->seats_total || f->fare < 0 ||
        !csv_safe(f->flight_no) || !csv_safe(f->source) || !csv_safe(f->dest) ||
        !csv_safe(f->date)) {
        return SB_INVALID_FLIGHT;
    }
    return pds_put_rec(SB_FLIGHTS, f->flight_id, f);
}

int sb_add_passenger(const Passenger *p)
{
    /* commas would break the CSV export, so they're rejected up front */
    if (!p || p->name[0] == '\0' || !csv_safe(p->name) || !csv_safe(p->passport_no) ||
        !csv_safe(p->phone)) {
        return SB_INVALID_PASSENGER;
    }
    return pds_put_rec(SB_PASSENGERS, p->passenger_id, p);
}

int sb_book(int32_t flight_id, int32_t passenger_id, Booking *out)
{
    Flight f;
    Passenger p;
    Booking b;
    BookingPairQuery pair;
    long already = 0;
    int32_t max_id = 0;
    int rc;

    rc = pds_get_rec(SB_FLIGHTS, flight_id, &f);
    if (rc == PDS_REC_NOT_FOUND) return SB_NO_FLIGHT;
    if (rc != PDS_SUCCESS) return rc;
    if (f.seats_booked >= f.seats_total) return SB_FLIGHT_FULL;

    rc = pds_get_rec(SB_PASSENGERS, passenger_id, &p);
    if (rc == PDS_REC_NOT_FOUND) return SB_NO_PASSENGER;
    if (rc != PDS_SUCCESS) return rc;

    /* No secondary index on (flight, passenger): this is a field scan. */
    pair.flight_id = flight_id;
    pair.passenger_id = passenger_id;
    rc = pds_get_rec_by_field(SB_BOOKINGS, &pair, match_booking_pair, NULL, NULL, &already, NULL);
    if (rc != PDS_SUCCESS) return rc;
    if (already > 0) return SB_ALREADY_BOOKED;

    /* Booking IDs are never reused, even for cancelled bookings. */
    rc = pds_max_key(SB_BOOKINGS, &max_id);
    if (rc != PDS_SUCCESS && rc != PDS_REC_NOT_FOUND) return rc;

    memset(&b, 0, sizeof b);
    b.booking_id = (rc == PDS_REC_NOT_FOUND) ? 1 : max_id + 1;
    b.flight_id = flight_id;
    b.passenger_id = passenger_id;
    b.fare_paid = f.fare;

    rc = pds_put_rec(SB_BOOKINGS, b.booking_id, &b);
    if (rc != PDS_SUCCESS) return rc;

    f.seats_booked++;
    rc = pds_update_rec(SB_FLIGHTS, flight_id, &f);
    if (rc != PDS_SUCCESS) {
        pds_delete_rec(SB_BOOKINGS, b.booking_id);   /* roll back */
        return rc;
    }
    if (out) *out = b;
    return PDS_SUCCESS;
}

int sb_cancel(int32_t booking_id)
{
    Booking b;
    Flight f;
    int rc = pds_get_rec(SB_BOOKINGS, booking_id, &b);
    if (rc != PDS_SUCCESS) return rc;

    rc = pds_delete_rec(SB_BOOKINGS, booking_id);
    if (rc != PDS_SUCCESS) return rc;

    if (pds_get_rec(SB_FLIGHTS, b.flight_id, &f) == PDS_SUCCESS && f.seats_booked > 0) {
        f.seats_booked--;
        rc = pds_update_rec(SB_FLIGHTS, b.flight_id, &f);
    }
    return rc;
}

int sb_delete_flight(int32_t flight_id)
{
    Flight f;
    long bookings = 0;
    int rc = pds_get_rec(SB_FLIGHTS, flight_id, &f);
    if (rc == PDS_REC_NOT_FOUND) return SB_NO_FLIGHT;
    if (rc != PDS_SUCCESS) return rc;

    rc = pds_get_rec_by_field(SB_BOOKINGS, &flight_id, match_booking_flight, NULL, NULL,
                              &bookings, NULL);
    if (rc != PDS_SUCCESS) return rc;
    if (bookings > 0) return SB_HAS_BOOKINGS;
    return pds_delete_rec(SB_FLIGHTS, flight_id);
}

const char *sb_strerror(int code)
{
    switch (code) {
    case SB_NO_FLIGHT:      return "no such flight";
    case SB_NO_PASSENGER:   return "no such passenger";
    case SB_FLIGHT_FULL:    return "flight is full";
    case SB_ALREADY_BOOKED: return "passenger already has a booking on this flight";
    case SB_HAS_BOOKINGS:   return "flight still has active bookings";
    case SB_INVALID_FLIGHT: return "invalid flight details (check seats and fare; no commas)";
    case SB_INVALID_PASSENGER: return "invalid passenger details (name required; no commas)";
    default:                return pds_strerror(code);
    }
}
