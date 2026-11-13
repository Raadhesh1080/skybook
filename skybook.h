/*
 * skybook.h - Airline reservation layer built on top of the PDS engine.
 *
 * Three tables: flights, passengers, bookings. This layer enforces the
 * business rules (seat capacity, no double booking, no deleting a flight
 * that still has bookings) and keeps flight seat counts in sync with
 * bookings.
 */
#ifndef SKYBOOK_H
#define SKYBOOK_H

#include "pds.h"
#include "record.h"

#define SB_FLIGHTS    "flights"
#define SB_PASSENGERS "passengers"
#define SB_BOOKINGS   "bookings"
#define SB_ROUTE_INDEX "route"      /* secondary index on flights: route -> flight IDs */

/* Errors specific to SkyBook (PDS error codes are passed through as-is). */
enum {
    SB_NO_FLIGHT = 100,
    SB_NO_PASSENGER,
    SB_FLIGHT_FULL,
    SB_ALREADY_BOOKED,
    SB_HAS_BOOKINGS,
    SB_INVALID_FLIGHT,
    SB_INVALID_PASSENGER
};

int sb_create(const char *db_name);   /* creates the db and its three tables */
int sb_open(const char *db_name);     /* opens the db, its three tables and the route index */
int sb_close(void);

int sb_add_flight(const Flight *f);
int sb_add_passenger(const Passenger *p);

/* Books a seat; on success *out (if non-NULL) receives the new booking. */
int sb_book(int32_t flight_id, int32_t passenger_id, Booking *out);
int sb_cancel(int32_t booking_id);
int sb_delete_flight(int32_t flight_id);

/* All flights on a route, via the route secondary index (no full scan).
 * *fetched = flight records read from disk. */
int sb_find_route(const char *source, const char *dest, pds_visit_fn visit, void *ctx,
                  long *matches, long *fetched);

const char *sb_strerror(int code);

#endif
