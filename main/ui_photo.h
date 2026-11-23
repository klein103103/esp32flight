#pragma once
#include "flight_model.h"

/* Aircraft photo popup: fetches the photo (planespotters via adsbdb) for the
 * given ICAO hex in a worker task and shows it. Call from LVGL context. */
void ui_photo_init(void);
void ui_photo_preload_models(void);
void ui_photo_open(const aircraft_t *aircraft);
void ui_aircraft_more_open(const aircraft_t *aircraft, const char *live_details);
