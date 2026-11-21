#include "wifi_setup.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "settings.h"

static esp_netif_t *s_ap;
static httpd_handle_t s_server;
static atomic_bool s_active;
static char s_password[13];

static bool is_setup_client(httpd_req_t *req)
{
    esp_netif_ip_info_t address;
    struct sockaddr_in local = {0};
    socklen_t size = sizeof(local);
    return s_ap != NULL && esp_netif_get_ip_info(s_ap, &address) == ESP_OK &&
           getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) == 0 &&
           local.sin_family == AF_INET && local.sin_addr.s_addr == address.ip.addr;
}

static esp_err_t page(httpd_req_t *req)
{
    if (!is_setup_client(req)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Setup is available on the radar Wi-Fi only.");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Radar Wi-Fi setup</title><h1>Connect the radar to Wi-Fi</h1>"
        "<form method=post autocomplete=off><p><label>Network name "
        "<input name=ssid maxlength=32 required></label></p>"
        "<p><label>Password <input name=pass type=password maxlength=64></label></p>"
        "<button>Save and restart radar</button></form>");
}

static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (unsigned char)tolower(ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static bool form_field(const char *body, const char *key, char *out, size_t capacity)
{
    size_t key_len = strlen(key);
    while (*body != '\0') {
        const char *end = strchr(body, '&');
        if (end == NULL) end = body + strlen(body);
        if ((size_t)(end - body) > key_len && body[key_len] == '=' &&
            strncmp(body, key, key_len) == 0) {
            size_t count = 0;
            for (const char *p = body + key_len + 1; p < end; p++) {
                unsigned char ch = (unsigned char)*p;
                if (ch == '+') {
                    ch = ' ';
                } else if (ch == '%') {
                    if (end - p < 3) return false;
                    int first = hex_digit((unsigned char)p[1]);
                    int second = hex_digit((unsigned char)p[2]);
                    if (first < 0 || second < 0) return false;
                    ch = (unsigned char)(first * 16 + second);
                    p += 2;
                }
                if (ch == '\0' || count + 1 >= capacity) return false;
                out[count++] = (char)ch;
            }
            out[count] = '\0';
            return true;
        }
        body = *end == '&' ? end + 1 : end;
    }
    return false;
}

static esp_err_t submit(httpd_req_t *req)
{
    if (!is_setup_client(req)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Setup is available on the radar Wi-Fi only.");
    }
    char origin[64];
    size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_len > 0 && (origin_len >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        strcmp(origin, "http://" WIFI_SETUP_IP ":8081") != 0)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open the setup form on the radar network.");
    }
    char body[330];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form.");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    char ssid[33], pass[65];
    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0' ||
        !form_field(body, "pass", pass, sizeof(pass)) || (pass[0] && strlen(pass) < 8)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Check the network name and password.");
    }
    settings_t *cfg = settings_get();
    char previous_ssid[sizeof(cfg->wifi_ssid)], previous_pass[sizeof(cfg->wifi_pass)];
    strlcpy(previous_ssid, cfg->wifi_ssid, sizeof(previous_ssid));
    strlcpy(previous_pass, cfg->wifi_pass, sizeof(previous_pass));
    strlcpy(cfg->wifi_ssid, ssid, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, pass, sizeof(cfg->wifi_pass));
    if (settings_save() != ESP_OK) {
        strlcpy(cfg->wifi_ssid, previous_ssid, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_pass, previous_pass, sizeof(cfg->wifi_pass));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save Wi-Fi settings.");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Saved. Radar is restarting. Reconnect your phone to your usual Wi-Fi.");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static void stop_setup(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    atomic_store(&s_active, false);
}

static void setup_expiry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    stop_setup();
    vTaskDelete(NULL);
}

esp_err_t wifi_setup_start(char *password, size_t capacity)
{
    if (atomic_exchange(&s_active, true)) {
        strlcpy(password, s_password, capacity);
        return ESP_OK;
    }
    if (s_ap == NULL) {
        s_ap = esp_netif_create_default_wifi_ap();
        if (s_ap == NULL) {
            atomic_store(&s_active, false);
            return ESP_FAIL;
        }
        esp_netif_ip_info_t address = {0};
        address.ip.addr = inet_addr(WIFI_SETUP_IP);
        address.gw.addr = address.ip.addr;
        address.netmask.addr = inet_addr("255.255.255.0");
        esp_netif_dhcps_stop(s_ap);
        esp_err_t ip_error = esp_netif_set_ip_info(s_ap, &address);
        if (ip_error != ESP_OK) {
            esp_netif_destroy_default_wifi(s_ap);
            s_ap = NULL;
            atomic_store(&s_active, false);
            return ip_error;
        }
    }
    snprintf(s_password, sizeof(s_password), "%08lx%04lx",
             (unsigned long)esp_random(), (unsigned long)(esp_random() & 0xffff));
    wifi_config_t config = {0};
    strlcpy((char *)config.ap.ssid, "Radar-Setup", sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, s_password, sizeof(config.ap.password));
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 1;
    config.ap.channel = 1;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &config);
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.server_port = 8081;
    http.ctrl_port = 32769;
    if (err == ESP_OK) err = httpd_start(&s_server, &http);
    static const httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = page };
    static const httpd_uri_t post = { .uri = "/", .method = HTTP_POST, .handler = submit };
    if (err == ESP_OK) err = httpd_register_uri_handler(s_server, &get);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_server, &post);
    if (err == ESP_OK && xTaskCreate(setup_expiry_task, "setup_expiry", 3072, NULL, 2, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        stop_setup();
        return err;
    }
    strlcpy(password, s_password, capacity);
    return ESP_OK;
}
