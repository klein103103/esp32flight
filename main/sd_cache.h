#pragma once
#include <stdbool.h>
#include <stddef.h>

void sd_cache_init(void);
bool sd_cache_ready(void);
bool sd_map_archive_available(void);
size_t sd_map_archive_read(unsigned z, unsigned x, unsigned y, void *buffer, size_t capacity);
const char *sd_cache_status(void);
unsigned sd_photo_cache_hits(void);
unsigned sd_photo_cache_saves(void);
void sd_photo_cache_record_hit(void);
size_t sd_cache_read(const char *path, void *buffer, size_t capacity);
bool sd_cache_write_details(const char *path, const void *data, size_t size);
bool sd_cache_write_photo(const char *path, const void *buffer, size_t size);
bool sd_cache_write_tile(const char *directory, const char *path,
                         const void *buffer, size_t size);
