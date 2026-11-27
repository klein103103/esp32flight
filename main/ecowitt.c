#include "ecowitt.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "fonts.h"
#include "theme.h"
#include "settings.h"
#include "http_util.h"

static atomic_bool active;
static atomic_uint session;
static lv_obj_t *panel, *status_label, *readings_label;
static const char station_title[] = "Weather Station";

bool ecowitt_active(void) { return atomic_load(&active); }

static void go_flight(lv_event_t *event)
{
    (void)event;
    atomic_store(&active, false);
    lv_obj_del(panel);
    panel = NULL;
}

void ecowitt_open(void)
{
    if (panel != NULL) return;
    panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_center(panel);
    lv_obj_set_style_pad_all(panel, UISX(16), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, app_theme()->bg, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(panel);
    lv_obj_set_style_text_font(title, UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(title, app_theme()->text, 0);
    lv_label_set_text(title, station_title);

    lv_obj_t *back = lv_btn_create(panel);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(back, go_flight, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(back);
    lv_label_set_text(label, LV_SYMBOL_LEFT " Flight");
    lv_obj_center(label);
    status_label = lv_label_create(panel);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 0, UISY(44));
    lv_obj_set_style_text_color(status_label, app_theme()->accent, 0);
    lv_label_set_text(status_label, "Connecting to Ecowitt on local Wi-Fi…");

    lv_obj_t *card = lv_obj_create(panel);
    lv_obj_set_size(card, lv_pct(100), LV_VER_RES - UISY(122));
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(card, app_theme()->panel, 0);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    readings_label = lv_label_create(card);
    lv_obj_set_width(readings_label, lv_pct(100));
    lv_obj_set_style_text_font(readings_label, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(readings_label, app_theme()->text, 0);
    lv_obj_set_style_text_line_space(readings_label, UISY(6), 0);
    lv_label_set_text(readings_label, "Waiting for sensor readings");
    atomic_fetch_add(&session, 1);
    atomic_store(&active, true);
}

static bool local_address(uint32_t ip, const esp_netif_ip_info_t *net)
{
    return ip != 0 && ip != net->ip.addr && net->netmask.addr != 0 &&
           (ip & net->netmask.addr) == (net->ip.addr & net->netmask.addr) &&
           ip != (net->ip.addr & net->netmask.addr) &&
           ip != (net->ip.addr | ~net->netmask.addr);
}

static bool discover(char ip[16], const esp_netif_ip_info_t *net)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    int broadcast = 1;
    const struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in source = {.sin_family = AF_INET, .sin_addr.s_addr = net->ip.addr};
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(46000),
                                 .sin_addr.s_addr = net->ip.addr | ~net->netmask.addr};
    const unsigned char command[] = {0xff, 0xff, 0x12, 0x03, 0x15};
    bool found = false;
    if (bind(fd, (struct sockaddr *)&source, sizeof(source)) == 0 &&
        sendto(fd, command, sizeof(command), 0, (struct sockaddr *)&target, sizeof(target)) ==
        (int)sizeof(command)) {
        int64_t deadline = esp_timer_get_time() + 1200000;
        while (!found && ecowitt_active() && esp_timer_get_time() < deadline) {
            unsigned char buf[128];
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
            if (n < 18 || buf[0] != 0xff || buf[1] != 0xff || buf[2] != 0x12 ||
                (buf[3] * 256 + buf[4]) != n - 2 ||
                !local_address(peer.sin_addr.s_addr, net)) continue;
            unsigned char checksum = 0;
            for (int i = 2; i < n - 1; i++) checksum += buf[i];
            if (checksum != buf[n-1]) continue;
            found = inet_ntop(AF_INET, &peer.sin_addr, ip, 16) != NULL;
        }
    }
    close(fd);
    return found;
}

static const char *reading_name(const char *id)
{
    static const char *const names[] = {
        NULL, "Indoor temperature", "Outdoor temperature", "Dew point",
        "Wind chill", "Heat index", "Indoor humidity", "Outdoor humidity",
        "Absolute pressure", "Relative pressure", "Wind direction", "Wind speed",
        "Wind gust", "Rain event", "Rain rate", NULL, "Rain today", "Rain this week",
        "Rain this month", "Rain this year", "Total rain", "Solar radiation",
        "UV", "UV index", "Station time", "Peak wind today"
    };
    if (strcmp(id, "3") == 0) return "Feels like";
    if (strncmp(id, "0x", 2) != 0) return id;
    char *end;
    unsigned long number = strtoul(id + 2, &end, 16);
    return *end == '\0' && number < sizeof(names)/sizeof(names[0]) && names[number]
           ? names[number] : id;
}

static const char *group_name(const char *key)
{
    static const struct { const char *key, *label; } groups[] = {
        {"common_list","Outdoor conditions and wind"}, {"rain","Rain gauge"},
        {"piezoRain","Piezo rain gauge"}, {"wh25","Indoor conditions and pressure"},
        {"ch_aisle","Temperature and humidity"}, {"ch_temp","Temperature sensors"},
        {"ch_soil","Soil moisture"}, {"ch_leaf","Leaf wetness"},
        {"ch_leak","Leak sensors"}, {"ch_lds","Water depth"}, {"ch_pm25","Air quality"},
        {"co2","Air quality and CO₂"}, {"lightning","Lightning"}
    };
    for (unsigned i = 0; i < sizeof(groups)/sizeof(groups[0]); i++)
        if (strcmp(key, groups[i].key) == 0) return groups[i].label;
    return key;
}

static const char *field_name(const char *key)
{
    static const struct { const char *key, *label; } fields[] = {
        {"temp","Temperature"}, {"intemp","Indoor temperature"}, {"humidity","Humidity"},
        {"inhum","Indoor humidity"}, {"inhumi","Indoor humidity"}, {"abs","Absolute pressure"},
        {"rel","Relative pressure"}, {"battery","Battery level"}, {"distance","Distance"},
        {"count","Strike count"}, {"status","Status"}, {"depth","Water depth"}
    };
    for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); i++)
        if (strcmp(key, fields[i].key) == 0) return fields[i].label;
    return key;
}

static void append(char *out, size_t size, size_t *used, const char *label,
                   const cJSON *value, const char *unit)
{
    if (*used >= size - 1 || !value) return;
    char number[24];
    const char *text;
    if (cJSON_IsString(value)) text = value->valuestring;
    else if (cJSON_IsNumber(value)) {
        snprintf(number, sizeof(number), "%.2f", value->valuedouble);
        text = number;
    } else if (cJSON_IsBool(value)) text = cJSON_IsTrue(value) ? "Yes" : "No";
    else return;
    if (!*text || strcmp(text, "None") == 0) return;
    int wrote = snprintf(out + *used, size - *used, "%.32s  %.48s%s%.8s\n",
                         label, text, *unit ? " " : "", unit);
    if (wrote > 0) *used += (size_t)wrote < size - *used ? (size_t)wrote : size - *used - 1;
}

static bool format_readings(cJSON *root, char *out, size_t size)
{
    const cJSON *group, *row, *field;
    size_t used = 0;
    bool found = false;
    if (!cJSON_IsObject(root)) return false;
    cJSON_ArrayForEach(group, root) {
        if (!cJSON_IsArray(group) || !group->child || !group->string) continue;
        if (used >= size - 2) break;
        int n = snprintf(out + used, size - used, "\n%.48s\n", group_name(group->string));
        if (n < 0 || (size_t)n >= size - used) break;
        used += n;
        cJSON_ArrayForEach(row, group) {
            if (!cJSON_IsObject(row)) continue;
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
            const cJSON *unit = cJSON_GetObjectItemCaseSensitive(row, "unit");
            const char *suffix = cJSON_IsString(unit) ? unit->valuestring : "";
            size_t initial = used;
            if (cJSON_IsString(id)) {
                append(out, size, &used, reading_name(id->valuestring),
                       cJSON_GetObjectItemCaseSensitive(row, "val"), suffix);
            } else {
                append(out, size, &used, "Channel", cJSON_GetObjectItem(row,"channel"), "");
                append(out, size, &used, "Sensor", cJSON_GetObjectItem(row,"name"), "");
                cJSON_ArrayForEach(field, row) {
                    const char *key = field->string;
                    if (!key || strcmp(key,"unit")==0 || strcmp(key,"channel")==0 ||
                        strcmp(key,"name")==0) continue;
                    bool temperature = strcmp(key,"temp")==0 || strcmp(key,"intemp")==0;
                    append(out, size, &used, field_name(key), field, temperature ? suffix : "");
                }
            }
            found |= used > initial;
        }
    }
    return found;
}

static bool fetch(const char *ip, char *out, size_t out_size)
{
    char url[64];
    char *body = heap_caps_malloc(24 * 1024, MALLOC_CAP_SPIRAM);
    if (!body) return false;
    snprintf(url, sizeof(url), "http://%s/get_livedata_info", ip);
    bool ok = false;
    if (http_get_local_to_buffer(url, body, 24 * 1024, 3000) == ESP_OK) {
        cJSON *root = cJSON_Parse(body);
        ok = format_readings(root, out, out_size);
        cJSON_Delete(root);
    }
    free(body);
    return ok;
}

void ecowitt_poll(void)
{
    static unsigned last_session;
    static int64_t next_poll, last_success;
    static char discovered[16];
    unsigned current = atomic_load(&session);
    int64_t now = esp_timer_get_time();
    if (last_session != current) {
        last_session = current;
        next_poll = last_success = 0;
        discovered[0] = '\0';
    }
    if (now < next_poll) return;
    next_poll = now + 15000000;
    char manual[16] = "", gateway[16] = "";
    if (lvgl_port_lock(250)) {
        strlcpy(manual, settings_get()->ecowitt_ip, sizeof(manual));
        lvgl_port_unlock();
    }
    const char *status = "Gateway unavailable. Check Wi-Fi or its IP in Settings.";
    esp_netif_t *wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t net;
    char *out = heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM);
    bool ok = false;
    if (out && wifi && esp_netif_get_ip_info(wifi, &net) == ESP_OK && net.ip.addr) {
        struct in_addr address;
        if (inet_pton(AF_INET, discovered, &address) != 1 ||
            !local_address(address.s_addr, &net)) discovered[0] = '\0';
        if (discovered[0] || discover(discovered, &net)) {
            strlcpy(gateway, discovered, sizeof(gateway));
            ok = fetch(gateway, out, 8192);
            if (!ok) discovered[0] = '\0';
        }
        if (!ok && inet_pton(AF_INET, manual, &address) == 1 &&
            local_address(address.s_addr, &net) && strcmp(gateway, manual) != 0 &&
            ecowitt_active()) {
            strlcpy(gateway, manual, sizeof(gateway));
            ok = fetch(gateway, out, 8192);
        }
    }
    if (lvgl_port_lock(500)) {
        if (panel && ecowitt_active() && atomic_load(&session) == current) {
            char message[100];
            if (ok) {
                last_success = esp_timer_get_time();
                snprintf(message, sizeof(message), "Ecowitt • %s • Live", gateway);
                status = message;
                if (strcmp(lv_label_get_text(readings_label), out) != 0)
                    lv_label_set_text(readings_label, out);
            } else if (last_success) {
                snprintf(message, sizeof(message), "Gateway unavailable • readings are %lld s old",
                         (long long)((esp_timer_get_time()-last_success)/1000000));
                status = message;
            }
            lv_label_set_text(status_label, status);
        }
        lvgl_port_unlock();
    }
    free(out);
}
