#pragma once
#include "flight_model.h"

/* Aircraft photo popup: fetches the photo (planespotters via adsbdb) for the
 * given ICAO hex in a worker task and shows it. Call from LVGL context. */
void ui_photo_open(const char *hex, const char *callsign);
void ui_aircraft_more_open(const aircraft_t *aircraft, const char *live_details);
