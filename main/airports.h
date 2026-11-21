#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "flight_model.h"

/* Local airport database (OurAirports dataset in SPIFFS). Call after the
 * assets partition is mounted. */
void airports_init(void);

/* Fill iata/city/country/name/coordinates for an ICAO code from the local
 * database. Returns false if the airport is unknown. */
bool airports_lookup(const char *icao, airport_t *ap);

/* Nearest airport (ICAO) to a point; false when the database is missing. */
bool airports_nearest(double lat, double lon, char icao_out[5]);

typedef struct {
    char code[5];
    float lat, lon;
    double distance_km;
} airport_marker_t;

/* Copy the nearest bundled airports inside the radius, up to capacity. */
size_t airports_nearby(double lat, double lon, double radius_km, airport_marker_t *out, size_t capacity);

/* Like airports_lookup, but also accepts a 3-letter IATA code. */
bool airports_lookup_any(const char *code, airport_t *ap);
