#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Starts a temporary, password-protected setup network for one phone. */
esp_err_t wifi_setup_start(char *password, size_t capacity);
