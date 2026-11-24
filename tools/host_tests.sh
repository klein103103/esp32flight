#!/bin/sh
# Host-side unit tests for the pure logic that has bitten us before:
# geo math (corridors, antimeridian, sun times) and the HH:MM parser.
# Run before a release; no ESP toolchain needed.
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---- geo_math suite: links the real source file ----
cat > "$tmp/geo_tests.c" <<'EOF'
#include <math.h>
#include <stdio.h>
#include "geo_math.h"

static int fails;

static void ok(int cond, const char *name)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        fails++;
    }
}

int main(void)
{
    /* longitude unwrap around the antimeridian */
    ok(fabs(geo_lon_unwrap(170.0, -170.0) - 190.0) < 1e-9, "unwrap +");
    ok(fabs(geo_lon_unwrap(-170.0, 170.0) - (-190.0)) < 1e-9, "unwrap -");
    ok(fabs(geo_lon_unwrap(10.0, 20.0) - 20.0) < 1e-9, "unwrap noop");
    ok(fabs(geo_lon_unwrap(-122.4, 114.2) - (-245.8)) < 0.01, "unwrap SFO-HKG");

    /* great-circle endpoints */
    double la, lo;
    geo_gc_point(50.0, 10.0, 60.0, 20.0, 0.0, &la, &lo);
    ok(fabs(la - 50.0) < 1e-6 && fabs(lo - 10.0) < 1e-6, "gc t=0");
    geo_gc_point(50.0, 10.0, 60.0, 20.0, 1.0, &la, &lo);
    ok(fabs(la - 60.0) < 1e-6 && fabs(lo - 20.0) < 1e-6, "gc t=1");

    /* route corridor: WAW-GDN flight near Lodz is plausible,
       the same callsign near Lisbon is not */
    ok(geo_route_plausible(52.17, 20.97, 54.38, 18.47, 52.2, 19.5), "corridor in");
    ok(!geo_route_plausible(52.17, 20.97, 54.38, 18.47, 38.7, -9.1), "corridor out");

    /* haversine sanity: Warsaw-Gdansk ~283 km */
    double d = geo_haversine_km(52.23, 21.01, 54.35, 18.65);
    ok(d > 270 && d < 300, "haversine WAW-GDN");

    /* sun times, Gdansk in late July (UTC+2): rise ~4:30-5:15, set ~20:45-21:30 */
    int rise = 0, set = 0;
    ok(geo_sun_times(54.35, 18.65, 1785300000LL, 7200, &rise, &set), "sun returns");
    ok(rise > 4 * 60 && rise < 5 * 60 + 20, "sunrise plausible");
    ok(set > 20 * 60 + 30 && set < 21 * 60 + 40, "sunset plausible");

    /* polar night: Longyearbyen in January has no sunrise */
    ok(!geo_sun_times(78.2, 15.6, 1767225600LL, 3600, &rise, &set), "polar night");

    if (fails == 0) {
        printf("geo_math: all ok\n");
    }
    return fails;
}
EOF
cc -I main -o "$tmp/geo_tests" "$tmp/geo_tests.c" main/geo_math.c -lm
"$tmp/geo_tests"

# ---- HH:MM parser (extracted from ui_settings.c) ----
cat > "$tmp/hhmm_tests.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int parse_hhmm(const char *txt, int fallback);
int main(void)
{
    struct { const char *in; int want; } t[] = {
        {"21:00",1260},{"21.00",1260},{"2100",1260},{"830",510},{"06:30",390},
        {"0630",390},{"21",1260},{"",-1},{"25:00",-1},{"12:75",-1},
        {"abc",-1},{"12345",-1},{" 930 ",570},{"0",0},{"23:59",1439},{"2359",1439}
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        int got = parse_hhmm(t[i].in, -1);
        if (got != t[i].want) {
            printf("FAIL hhmm '%s': got %d want %d\n", t[i].in, got, t[i].want);
            fails++;
        }
    }
    if (fails == 0) {
        printf("parse_hhmm: all ok\n");
    }
    return fails;
}
EOF
sed -n '/^static int parse_hhmm/,/^}/p' main/ui_settings.c >> "$tmp/hhmm_tests.c"
cc -o "$tmp/hhmm_tests" "$tmp/hhmm_tests.c"
"$tmp/hhmm_tests"

# Exercise the production provider selection with fake HTTP and a fake clock.
cat > "$tmp/providers.c" <<'EOF'
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef struct { const char *source; unsigned count; } aircraft_list_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define FLIGHT_FETCH_DEFERRED 0x7f01
#define ESP_ERR_HTTP_BASE 1000
#define FETCH_BUF_SIZE 64
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

static int64_t clock_ms;
static int calls[2];
static int replies[2];
static int retry_after;
static struct { char flyitaly_key[128]; } test_settings;
#define settings_get() (&test_settings)

static int64_t esp_timer_get_time(void) { return clock_ms * 1000; }

static int http_get_keepalive(int slot, const char *url, char *buf,
                              size_t size, size_t *length)
{
    (void)length;
    assert(slot == 0 || slot == 1);
    assert(strstr(url, slot == 0 ? "opendata.adsb.fi/" : "api.adsb.lol/"));
    calls[slot]++;
    snprintf(buf, size, "empty aircraft list");
    return replies[slot];
}

static int http_get_rate_limited(const char *url, char *buf, size_t size, int *retry)
{
    *retry = retry_after;
    return http_get_keepalive(1, url, buf, size, NULL);
}

static int http_get_to_buffer_hdr(const char *url, char *buf, size_t size,
                                  size_t *length, const char *header, const char *key)
{
    (void)length;
    assert(strcmp(header, "X-Api-Key") == 0 && strcmp(key, "test-key") == 0);
    assert(strstr(url, "api.flyitalyadsb.com/v2/") && strstr(url, "/dist/46"));
    snprintf(buf, size, "empty aircraft list");
    return ESP_OK;
}

static int parse_point_response(const char *buf, double lat, double lon,
                                aircraft_list_t *out)
{
    (void)lat;
    (void)lon;
    assert(strcmp(buf, "empty aircraft list") == 0);
    out->count = 0;
    return ESP_OK;
}

EOF
sed -n '/^#define FALLBACK_INTERVAL_MS/,/^\/\* LAN receiver URL/{ /^\/\* LAN receiver URL/d; p; }' main/flight_data.c >> "$tmp/providers.c"
cat >> "$tmp/providers.c" <<'EOF'
int main(int argc, char **argv)
{
    char buf[FETCH_BUF_SIZE];
    aircraft_list_t out = {0};
    assert(argc == 2);
    if (strcmp(argv[1], "third") == 0) {
        strcpy(test_settings.flyitaly_key, "test-key");
        replies[0] = ESP_ERR_HTTP_BASE + 403;
        replies[1] = ESP_ERR_HTTP_BASE + 429;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(strcmp(out.source, "FlyItalyADSB") == 0);
        clock_ms = 5000;
        assert(fetch_internet(0, 0, 25, &out, buf) != ESP_OK);
        clock_ms = 10000;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
    } else if (strcmp(argv[1], "refused") == 0) {
        replies[0] = ESP_ERR_HTTP_BASE + 403;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(calls[0] == 1 && calls[1] == 1);
        assert(strcmp(out.source, "adsb.lol") == 0);
        clock_ms = 179999;
        assert(fetch_internet(0, 0, 25, &out, buf) != ESP_OK);
        assert(calls[0] == 1 && calls[1] == 1);
        clock_ms = 180000;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(calls[0] == 1 && calls[1] == 2);
        replies[1] = ESP_ERR_HTTP_BASE + 429;
        retry_after = 600;
        clock_ms = 360000;
        assert(fetch_internet(0, 0, 25, &out, buf) != ESP_OK);
        assert(calls[1] == 3);
        replies[1] = ESP_OK;
        clock_ms = 959999;
        assert(fetch_internet(0, 0, 25, &out, buf) != ESP_OK);
        assert(calls[1] == 3);
        clock_ms = 960000;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(calls[1] == 4);
    } else {
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(out.count == 0 && strcmp(out.source, "adsb.fi") == 0);
        assert(calls[0] == 1 && calls[1] == 0);
        replies[0] = ESP_ERR_TIMEOUT;
        clock_ms = 10000;
        assert(fetch_internet(0, 0, 25, &out, buf) != ESP_OK);
        assert(calls[1] == 0);
        clock_ms = 20000;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(calls[1] == 1 && strcmp(out.source, "adsb.lol") == 0);
        clock_ms = 30000;
        assert(fetch_internet(0, 0, 25, &out, buf) == FLIGHT_FETCH_DEFERRED);
        assert(calls[1] == 1);
        replies[0] = ESP_OK;
        clock_ms = 40000;
        assert(fetch_internet(0, 0, 25, &out, buf) == ESP_OK);
        assert(calls[1] == 1 && strcmp(out.source, "adsb.fi") == 0);
    }
    return 0;
}
EOF
cc -std=c11 -Wall -Wextra -Werror -o "$tmp/providers" "$tmp/providers.c"
"$tmp/providers" recovery
"$tmp/providers" refused
"$tmp/providers" third
echo "provider tests passed"

cat > "$tmp/preferences.c" <<'EOF'
#include <assert.h>
#include "radar_preferences.h"

int main(void)
{
    assert(radar_clamp_poll_seconds(0) == 5);
    assert(radar_clamp_poll_seconds(5) == 5);
    assert(radar_clamp_poll_seconds(10) == 10);
    assert(radar_clamp_poll_seconds(600) == 600);
    assert(radar_clamp_poll_seconds(999) == 600);
    assert(!radar_stale_expired(-1, 100000));
    assert(!radar_stale_expired(5000, 4999));
    assert(!radar_stale_expired(5000, 64999));
    assert(radar_stale_expired(5000, 65000));
    return 0;
}
EOF
cc -std=c11 -Wall -Wextra -Werror -I main -o "$tmp/preferences" "$tmp/preferences.c"
"$tmp/preferences"
echo "settings and stale cutoff tests passed"

cat > "$tmp/radmap_tests.c" <<'EOF'
#include <assert.h>
#include "radmap_archive.h"

int main(void)
{
    FILE *f = tmpfile();
    assert(f);
    static const unsigned char fixture[] = {
        'R','A','D','M','A','P','0','1', 1,0,0,0, 0,0,0,0, 0,1,0,0, 1,0,0,0,
        32,0,0,0, 24,0,0,0,
        0x89,'P','N','G',13,10,26,10, 0,0,0,13, 'I','H','D','R', 0,0,1,0, 0,0,1,0
    };
    unsigned char buffer[24];
    assert(fwrite(fixture, 1, sizeof(fixture), f) == sizeof(fixture));
    rewind(f);
    assert(radmap_read(f, 0, 0, 0, buffer, sizeof(buffer)) == 24);
    rewind(f);
    assert(radmap_read(f, 0, 1, 0, buffer, sizeof(buffer)) == 0);
    rewind(f);
    assert(radmap_read(f, 0, 0, 0, buffer, 23) == 0);
    assert(fseek(f, 24, SEEK_SET) == 0);
    assert(fputc(255, f) == 255);
    rewind(f);
    assert(radmap_read(f, 0, 0, 0, buffer, sizeof(buffer)) == 0);
    fclose(f);
    return 0;
}
EOF
cc -std=c11 -Wall -Wextra -Werror -I main -o "$tmp/radmap_tests" "$tmp/radmap_tests.c"
"$tmp/radmap_tests"
echo "SD archive reader tests passed"

echo "host tests passed"
