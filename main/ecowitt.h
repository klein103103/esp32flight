#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ECOWITT_READINGS_SIZE 8192

typedef struct {
    bool connected;
    int64_t age_seconds;
    char gateway[16];
} ecowitt_status_t;

bool ecowitt_read(ecowitt_status_t *status, char *out, size_t capacity);

bool ecowitt_active(void);
void ecowitt_open(void);
void ecowitt_poll(void);
