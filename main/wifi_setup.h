#pragma once

#include <stddef.h>
#include "esp_err.h"

#define WIFI_SETUP_IP "192.168.47.1"

/* Starts a temporary, password-protected setup network for one phone. */
esp_err_t wifi_setup_start(char *password, size_t capacity);
