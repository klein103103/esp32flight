#include "ui_photo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "fonts.h"
#include "http_util.h"
#include "jpeg_dec.h"
#include "lang.h"
#include "lvgl_port.h"
#include "theme.h"
#include "routes.h"
#include "geo_math.h"
#ifndef APKFLIGHT
#include "sd_cache.h"
#endif

#define COL_BG     (app_theme()->bg)
#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)

#define PHOTO_MAX_BYTES (512 * 1024)

static lv_obj_t *s_overlay;
static lv_obj_t *s_img;
static lv_obj_t *s_photo_view;
static lv_obj_t *s_msg;
static lv_obj_t *s_credit;
static lv_obj_t *s_aircraft_details;
static uint8_t *s_photo_buf;
static char s_photo_hex[8];
static char s_photo_credit[128];
static lv_img_dsc_t s_photo_dsc;
static bool s_busy;

static void close_cb(lv_event_t *e)
{
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    /* keep s_photo_buf until next fetch: the img widget is gone with overlay */
}

typedef struct {
    char hex[8];
    aircraft_t aircraft;
    bool show_details;
    bool photo_cached;
} photo_req_t;

static const char *TAG = "photo";

/* Source 1: planespotters.net public API (no key). Their terms ask for
 * the photographer's credit next to the picture. */
static bool from_planespotters(const char *kind, const char *id, char *url, size_t un,
                               char *credit, size_t cn, char *json, size_t jn)
{
    char api[96];
    snprintf(api, sizeof(api), "https://api.planespotters.net/pub/photos/%s/%s", kind, id);
    esp_err_t err = http_get_to_buffer_t(api, json, jn, NULL, 3500);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "planespotters %s: %s", id, esp_err_to_name(err));
        return false;
    }
    bool ok = false;
    cJSON *root = cJSON_Parse(json);
    const cJSON *photos = root ? cJSON_GetObjectItem(root, "photos") : NULL;
    const cJSON *ph = cJSON_IsArray(photos) ? cJSON_GetArrayItem(photos, 0) : NULL;
    if (ph != NULL) {
        const cJSON *tl = cJSON_GetObjectItem(ph, "thumbnail");
        if (tl == NULL) {
            tl = cJSON_GetObjectItem(ph, "thumbnail_large");
        }
        const cJSON *src = tl ? cJSON_GetObjectItem(tl, "src") : NULL;
        const cJSON *who = cJSON_GetObjectItem(ph, "photographer");
        if (cJSON_IsString(src)) {
            strlcpy(url, src->valuestring, un);
            snprintf(credit, cn, "photo: %s / planespotters.net",
                     cJSON_IsString(who) ? who->valuestring : "unknown");
            ok = true;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "planespotters %s: %s", id, ok ? "photo found" : "no photo");
    return ok;
}

/* Source 2: adsbdb's aircraft record, whose photo links point at
 * airport-data.com (thin coverage). Full photo first, then thumbnail. */
static int from_adsbdb(const char *hex, char urls[2][256], char *json, size_t jn)
{
    char api[80];
    snprintf(api, sizeof(api), "https://api.adsbdb.com/v0/aircraft/%s", hex);
    esp_err_t err = http_get_to_buffer(api, json, jn, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adsbdb %s: %s", hex, esp_err_to_name(err));
        return 0;
    }
    int n = 0;
    cJSON *root = cJSON_Parse(json);
    const cJSON *resp = root ? cJSON_GetObjectItem(root, "response") : NULL;
    const cJSON *acj = resp ? cJSON_GetObjectItem(resp, "aircraft") : NULL;
    const char *keys[2] = { "url_photo_thumbnail", "url_photo" };
    for (int i = 0; i < 2 && acj != NULL; i++) {
        const cJSON *ju = cJSON_GetObjectItem(acj, keys[i]);
        if (cJSON_IsString(ju) && ju->valuestring[0]) {
            strlcpy(urls[n++], ju->valuestring, 256);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "adsbdb %s: %d photo link(s)", hex, n);
    return n;
}

static uint16_t *fetch_decode(const char *url, uint8_t *raw, int *w, int *h, size_t *decoded_bytes)
{
    size_t len = 0;
    esp_err_t err = http_get_to_buffer_t(url, (char *)raw, PHOTO_MAX_BYTES, &len, 6500);
    if (err != ESP_OK || len <= 4) {
        ESP_LOGW(TAG, "image fetch failed (%s): %s", esp_err_to_name(err), url);
        return NULL;
    }
    uint16_t *px = jpeg_decode_rgb565(raw, len, 700, 380, w, h);
    if (px != NULL) *decoded_bytes = len;
    ESP_LOGI(TAG, "image %u B -> %s", (unsigned)len, px ? "decoded" : "decode failed");
    return px;
}

static const char *commons_metadata(const cJSON *metadata, const char *key)
{
    const cJSON *value = cJSON_GetObjectItem(cJSON_GetObjectItem(metadata, key), "value");
    return cJSON_IsString(value) ? value->valuestring : "";
}

static uint16_t *fetch_representative_photo(const aircraft_t *aircraft, char *json,
                                            uint8_t *raw, int *w, int *h, size_t *bytes,
                                            char *credit, size_t credit_size)
{
    const char *type = aircraft->type_icao;
    size_t len = strlen(type);
    if (len < 2 || len > 4 || strspn(type, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != len) {
        return NULL;
    }

    const char *search = type;
    if (strcmp(type, "A21N") == 0) search = "Airbus%20A321neo";
    if (strcmp(type, "B739") == 0) search = "Boeing%20737-900";
    if (strcmp(type, "B39M") == 0) search = "Boeing%20737%20MAX%209";
    char api[512];
    snprintf(api, sizeof(api),
             "https://commons.wikimedia.org/w/api.php?action=query&format=json&formatversion=2"
             "&generator=search&gsrnamespace=6&gsrlimit=2&gsrsearch=filetype%%3Abitmap%%20%%22%s%%22%%20aircraft"
             "&prop=imageinfo&iiprop=url%%7Cthumbmime%%7Cextmetadata&iiurlwidth=500"
             "&iiextmetadatafilter=Artist%%7CLicenseShortName", search);
    if (http_get_to_buffer_t(api, json, 8192, NULL, 3500) != ESP_OK) return NULL;

    cJSON *root = cJSON_Parse(json);
    const cJSON *pages = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "query"), "pages");
    const cJSON *page;
    uint16_t *pixels = NULL;
    cJSON_ArrayForEach(page, pages) {
        const cJSON *info = cJSON_GetArrayItem(cJSON_GetObjectItem(page, "imageinfo"), 0);
        const cJSON *thumb = cJSON_GetObjectItem(info, "thumburl");
        const cJSON *mime = cJSON_GetObjectItem(info, "thumbmime");
        const cJSON *metadata = cJSON_GetObjectItem(info, "extmetadata");
        const char *license = commons_metadata(metadata, "LicenseShortName");
        const char *artist = commons_metadata(metadata, "Artist");
        if (!cJSON_IsString(thumb) || !cJSON_IsString(mime) ||
            strcmp(mime->valuestring, "image/jpeg") != 0 ||
            strncmp(thumb->valuestring, "https://upload.wikimedia.org/", 29) != 0 ||
            (strncmp(license, "CC BY", 5) != 0 && strcmp(license, "CC0") != 0 &&
             strcmp(license, "Public domain") != 0)) {
            continue;
        }

        char author[49];
        size_t used = 0;
        bool in_tag = false;
        for (const char *p = artist; *p && used < sizeof(author) - 1; p++) {
            if (*p == '<') in_tag = true;
            else if (*p == '>') in_tag = false;
            else if (!in_tag && (unsigned char)*p >= 32 && (unsigned char)*p < 127) {
                author[used++] = *p;
            }
        }
        author[used] = '\0';
        if (author[0] == '\0' && strncmp(license, "CC BY", 5) == 0) continue;
        pixels = fetch_decode(thumb->valuestring, raw, w, h, bytes);
        if (pixels != NULL) {
            snprintf(credit, credit_size, "Representative %s • Wikimedia Commons\n%.48s%s%.16s",
                     type, author, author[0] ? " / " : "", license);
            break;
        }
    }
    cJSON_Delete(root);
    return pixels;
}

static void show_route_details(const photo_req_t *req, char *buffer, size_t size)
{
    route_info_t route;
    char route_text[216] = "Departure and destination unavailable";
    if (routes_lookup_details(&req->aircraft, &route, buffer)) {
        snprintf(route_text, sizeof(route_text), "From: %s  %.44s\nTo: %s  %.44s\n%.42s",
                 route.origin.iata[0] ? route.origin.iata : route.origin.icao,
                 route.origin.city[0] ? route.origin.city : route.origin.name,
                 route.destination.iata[0] ? route.destination.iata : route.destination.icao,
                 route.destination.city[0] ? route.destination.city : route.destination.name,
                 route.airline_name);
        if (req->aircraft.has_pos) {
            double distance_nm = geo_haversine_km(req->aircraft.lat, req->aircraft.lon,
                                                   route.destination.lat, route.destination.lon) / 1.852;
            size_t used = strlen(route_text);
            snprintf(route_text + used, sizeof(route_text) - used, "  |  %.0f NM to destination", distance_nm);
        }
    }
    char identity[118];
    const char *reg = req->aircraft.reg;
    const char *type = req->aircraft.type_desc[0] ? req->aircraft.type_desc : req->aircraft.type_icao;
    snprintf(identity, sizeof(identity), "Registration: %.18s  |  Aircraft: %.61s",
             reg[0] ? reg : "—", type[0] ? type : "—");
    if (lvgl_port_lock(-1)) {
        if (s_overlay != NULL && s_aircraft_details != NULL) {
            lv_label_set_text_fmt(s_aircraft_details, "%s\n%s", route_text, identity);
        }
        lvgl_port_unlock();
    }
    if (reg[0] && type[0]) return;
    if (strlen(req->hex) != 6 || strspn(req->hex, "0123456789abcdefABCDEF") != 6) return;

    char url[96];
    snprintf(url, sizeof(url), "https://hexdb.io/api/v2/aircraft/%s", req->hex);
    if (http_get_to_buffer_t(url, buffer, size, NULL, 3000) != ESP_OK) return;
    cJSON *root = cJSON_Parse(buffer);
    const cJSON *remote_reg = cJSON_GetObjectItem(root, "registration");
    const cJSON *remote_type = cJSON_GetObjectItem(root, "type");
    snprintf(identity, sizeof(identity), "Registration: %.18s  |  Aircraft: %.61s",
             reg[0] ? reg : (cJSON_IsString(remote_reg) ? remote_reg->valuestring : "—"),
             type[0] ? type : (cJSON_IsString(remote_type) ? remote_type->valuestring : "—"));
    cJSON_Delete(root);
    if (lvgl_port_lock(-1)) {
        if (s_overlay != NULL && s_aircraft_details != NULL) {
            lv_label_set_text_fmt(s_aircraft_details, "%s\n%s", route_text, identity);
        }
        lvgl_port_unlock();
    }
}

static void display_cached_photo(void)
{
    lv_img_set_src(s_img, &s_photo_dsc);
    unsigned zoom = s_photo_dsc.header.w <= 390 && s_photo_dsc.header.h <= 200 ? 512 : 256;
    if (s_photo_view != NULL) {
        lv_obj_update_layout(s_photo_view);
        unsigned fit_width = (unsigned)lv_obj_get_content_width(s_photo_view) * 256U / s_photo_dsc.header.w;
        unsigned fit_height = (unsigned)lv_obj_get_content_height(s_photo_view) * 256U / s_photo_dsc.header.h;
        if (zoom > fit_width) zoom = fit_width;
        if (zoom > fit_height) zoom = fit_height;
    }
    lv_img_set_zoom(s_img, zoom);
    lv_obj_center(s_img);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_credit, s_photo_credit);
}

static void photo_task(void *arg)
{
    photo_req_t *req = arg;
    char credit[128] = "";
    uint16_t *pixels = NULL;
    int w = 0, h = 0;
    size_t downloaded_bytes = 0;

    char *json = malloc(8192);
    uint8_t *raw = req->photo_cached ? NULL : heap_caps_malloc(PHOTO_MAX_BYTES, MALLOC_CAP_SPIRAM);
#ifndef APKFLIGHT
    char cached_image[48] = "", cached_credit[48];
    if (raw != NULL && strlen(req->hex) == 6 && strspn(req->hex, "0123456789abcdefABCDEF") == 6) {
        char path[48];
        char hex[7];
        for (size_t i = 0; i < 6; i++) {
            char c = req->hex[i];
            hex[i] = c >= 'A' && c <= 'F' ? (char)(c + 'a' - 'A') : c;
        }
        hex[6] = '\0';
        snprintf(path, sizeof(path), "/sd/radar/photos/%s.jpg", hex);
        size_t size = sd_cache_read(path, raw, PHOTO_MAX_BYTES);
        if (size > 4) {
            pixels = jpeg_decode_rgb565(raw, size, 700, 380, &w, &h);
            if (pixels != NULL) strlcpy(credit, "Photo: SD card", sizeof(credit));
        }
        if (pixels == NULL) {
            snprintf(cached_image, sizeof(cached_image), "/sd/radar/cachep/%s.jpg", hex);
            snprintf(cached_credit, sizeof(cached_credit), "/sd/radar/cachep/%s.txt", hex);
            size_t credit_size = sd_cache_read(cached_credit, credit, sizeof(credit) - 1);
            credit[credit_size] = '\0';
            if (credit_size > 0) {
                size = sd_cache_read(cached_image, raw, PHOTO_MAX_BYTES);
                if (size > 4) pixels = jpeg_decode_rgb565(raw, size, 700, 380, &w, &h);
                if (pixels != NULL) sd_photo_cache_record_hit();
            }
        }
    }
#endif
    bool route_first = req->show_details && json != NULL && pixels == NULL && !req->photo_cached;
    if (route_first) show_route_details(req, json, 8192);
    if (pixels == NULL && json != NULL && raw != NULL) {
        char url[256];
        if (from_planespotters("hex", req->hex, url, sizeof(url), credit, sizeof(credit),
                               json, 8192)) {
            pixels = fetch_decode(url, raw, &w, &h, &downloaded_bytes);
        }
        const char *reg = req->aircraft.reg;
        if (pixels == NULL && reg[0] &&
            strspn(reg, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") == strlen(reg) &&
            from_planespotters("reg", reg, url, sizeof(url), credit, sizeof(credit), json, 8192)) {
            pixels = fetch_decode(url, raw, &w, &h, &downloaded_bytes);
        }
        if (pixels == NULL) {
            char urls[2][256];
            int n = from_adsbdb(req->hex, urls, json, 8192);
            for (int i = 0; i < n && pixels == NULL; i++) {
                pixels = fetch_decode(urls[i], raw, &w, &h, &downloaded_bytes);
            }
            if (pixels != NULL) {
                strlcpy(credit, "photo: airport-data.com", sizeof(credit));
            }
        }
        if (pixels == NULL && req->aircraft.type_icao[0]) {
            if (lvgl_port_lock(-1)) {
                if (s_overlay != NULL) lv_label_set_text(s_msg, "Looking for a representative photo…");
                lvgl_port_unlock();
            }
            pixels = fetch_representative_photo(&req->aircraft, json, raw, &w, &h,
                                                &downloaded_bytes, credit, sizeof(credit));
        }
    }
    if (pixels == NULL && !req->photo_cached) {
        ESP_LOGI(TAG, "%s: no photo from any source", req->hex);
    }

    if (!req->photo_cached && lvgl_port_lock(-1)) {
        if (s_overlay != NULL) {
            if (pixels != NULL) {
                free(s_photo_buf);
                s_photo_buf = (uint8_t *)pixels;
                pixels = NULL;
                memset(&s_photo_dsc, 0, sizeof(s_photo_dsc));
                s_photo_dsc.header.always_zero = 0;
                s_photo_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                s_photo_dsc.header.w = w;
                s_photo_dsc.header.h = h;
                s_photo_dsc.data = s_photo_buf;
                s_photo_dsc.data_size = (uint32_t)w * h * 2;
                strlcpy(s_photo_hex, req->hex, sizeof(s_photo_hex));
                strlcpy(s_photo_credit, credit, sizeof(s_photo_credit));
                display_cached_photo();
            } else {
                lv_label_set_text(s_msg, L()->no_photo);
            }
        }
        lvgl_port_unlock();
    }
    free(pixels);
#ifndef APKFLIGHT
    if (downloaded_bytes > 0 && cached_image[0] && credit[0] &&
        sd_cache_write_photo(cached_credit, credit, strlen(credit))) {
        sd_cache_write_photo(cached_image, raw, downloaded_bytes);
    }
#endif
    free(raw);
    if (req->show_details && json != NULL && !route_first) show_route_details(req, json, 8192);
    if (req->show_details && json == NULL && lvgl_port_lock(-1)) {
        if (s_overlay != NULL && s_aircraft_details != NULL) {
            lv_label_set_text(s_aircraft_details, "Extra aircraft and route details unavailable");
        }
        lvgl_port_unlock();
    }
    free(json);
    free(req);
    s_busy = false;
    vTaskDelete(NULL);
}

static void open_photo_panel(const char *hex, const char *callsign, const char *live_details,
                             const aircraft_t *aircraft)
{
    bool show_details = live_details != NULL;
    if (s_overlay != NULL || s_busy) {
        return;
    }

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_label_set_text_fmt(title, "%s  -  %s", callsign, hex);
    lv_obj_set_width(title, LV_HOR_RES - UISX(96));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, UISX(16), UISY(16));

    lv_obj_t *btn_close = lv_btn_create(s_overlay);
    lv_obj_set_size(btn_close, UISX(52), UISY(40));
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -UISX(12), UISY(8));
    lv_obj_set_style_bg_color(btn_close, COL_PANEL, 0);
    lv_obj_set_style_border_color(btn_close, COL_DIM, 0);
    lv_obj_set_style_border_width(btn_close, 1, 0);
    lv_obj_add_event_cb(btn_close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(btn_close);
    lv_obj_set_style_text_font(xl, UIFONT(&lv_font_montserrat_16, &lv_font_montserrat_10), 0);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);

    s_aircraft_details = NULL;
    s_photo_view = NULL;
    lv_obj_t *photo_parent = s_overlay;
    if (show_details) {
        const int margin = UISX(12);
        const int body_top = UISY(62);
        const int body_height = LV_VER_RES - body_top - UISY(12);
        const int details_width = LV_HOR_RES * 37 / 100;
        const int photo_width = LV_HOR_RES - details_width - 3 * margin;

        lv_obj_t *details = lv_obj_create(s_overlay);
        lv_obj_set_size(details, details_width, body_height);
        lv_obj_set_pos(details, margin, body_top);
        lv_obj_set_style_bg_color(details, COL_PANEL, 0);
        lv_obj_set_style_border_width(details, 0, 0);
        lv_obj_set_style_pad_all(details, UISX(12), 0);
        lv_obj_set_style_pad_row(details, UISY(12), 0);
        lv_obj_set_scroll_dir(details, LV_DIR_VER);
        lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *heading = lv_label_create(details);
        lv_obj_set_style_text_font(heading, UIFONT(&font_pl_20, &font_pl_12), 0);
        lv_obj_set_style_text_color(heading, COL_ACCENT, 0);
        lv_label_set_text(heading, "Live flight");

        lv_obj_t *live = lv_label_create(details);
        lv_obj_set_style_text_font(live, UIFONT(&font_pl_14, &font_pl_8), 0);
        lv_obj_set_style_text_color(live, COL_TEXT, 0);
        lv_obj_set_style_text_line_space(live, UISY(5), 0);
        lv_obj_set_width(live, lv_pct(100));
        lv_label_set_text_fmt(live, "%s\nSquawk %s\nVertical rate %+d ft/min",
                              live_details, aircraft->squawk[0] ? aircraft->squawk : "—",
                              aircraft->baro_rate_fpm);

        heading = lv_label_create(details);
        lv_obj_set_style_text_font(heading, UIFONT(&font_pl_20, &font_pl_12), 0);
        lv_obj_set_style_text_color(heading, COL_ACCENT, 0);
        lv_obj_set_width(heading, lv_pct(100));
        lv_label_set_text(heading, "Route and aircraft");

        s_aircraft_details = lv_label_create(details);
        lv_obj_set_style_text_font(s_aircraft_details, UIFONT(&font_pl_14, &font_pl_8), 0);
        lv_obj_set_style_text_color(s_aircraft_details, COL_TEXT, 0);
        lv_obj_set_style_text_line_space(s_aircraft_details, UISY(5), 0);
        lv_obj_set_width(s_aircraft_details, lv_pct(100));
        lv_label_set_text(s_aircraft_details, "Looking up route and aircraft details…");

        s_photo_view = lv_obj_create(s_overlay);
        lv_obj_set_size(s_photo_view, photo_width, body_height - UISY(58));
        lv_obj_set_pos(s_photo_view, details_width + 2 * margin, body_top);
        lv_obj_set_style_bg_color(s_photo_view, COL_PANEL, 0);
        lv_obj_set_style_border_width(s_photo_view, 0, 0);
        lv_obj_set_style_pad_all(s_photo_view, 0, 0);
        lv_obj_clear_flag(s_photo_view, LV_OBJ_FLAG_SCROLLABLE);
        photo_parent = s_photo_view;
    }
    s_msg = lv_label_create(photo_parent);
    lv_obj_set_style_text_font(s_msg, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(s_msg, COL_DIM, 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_msg, lv_pct(90));
    lv_label_set_text(s_msg, L()->loading_photo);
    lv_obj_center(s_msg);

    s_img = lv_img_create(photo_parent);
    lv_obj_center(s_img);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    s_credit = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_credit, UIFONT(&font_pl_14, &font_pl_8), 0);
    lv_obj_set_style_text_color(s_credit, COL_DIM, 0);
    lv_label_set_text(s_credit, "");
    if (show_details) {
        const int margin = UISX(12);
        const int details_width = LV_HOR_RES * 37 / 100;
        lv_obj_set_width(s_credit, LV_HOR_RES - details_width - 3 * margin);
        lv_obj_set_style_text_align(s_credit, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_credit, details_width + 2 * margin, LV_VER_RES - UISY(62));
    } else {
        lv_obj_align(s_credit, LV_ALIGN_BOTTOM_LEFT, UISX(16), -UISY(8));
    }

    bool photo_cached = s_photo_buf != NULL && strcmp(s_photo_hex, hex) == 0;
    if (photo_cached) {
        display_cached_photo();
        if (!show_details) return;
    }
    photo_req_t *req = malloc(sizeof(photo_req_t));
    if (req != NULL) {
        strlcpy(req->hex, hex, sizeof(req->hex));
        if (aircraft != NULL) req->aircraft = *aircraft;
        req->show_details = show_details;
        req->photo_cached = photo_cached;
        s_busy = true;
        if (xTaskCreate(photo_task, "photo", 8192, req, 3, NULL) == pdPASS) {
            return;
        }
        s_busy = false;
        free(req);
    }
    if (s_aircraft_details != NULL) {
        lv_label_set_text(s_aircraft_details, "Aircraft and route details unavailable");
    }
    if (!photo_cached) lv_label_set_text(s_msg, L()->no_photo);
}

void ui_photo_open(const aircraft_t *aircraft)
{
    open_photo_panel(aircraft->hex, aircraft->callsign[0] ? aircraft->callsign : aircraft->hex,
                     NULL, aircraft);
}

void ui_aircraft_more_open(const aircraft_t *aircraft, const char *live_details)
{
    open_photo_panel(aircraft->hex, aircraft->callsign[0] ? aircraft->callsign : aircraft->hex,
                     live_details, aircraft);
}
