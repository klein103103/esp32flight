#pragma once
#include <stdbool.h>
#include <stddef.h>

void sd_cache_init(void);
bool sd_cache_ready(void);
size_t sd_cache_read(const char *path, void *buffer, size_t capacity);
bool sd_cache_write_tile(const char *directory, const char *path,
                         const void *buffer, size_t size);
