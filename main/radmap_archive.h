#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

enum { RADMAP_HEADER_BYTES = 24, RADMAP_RECORD_BYTES = 8 };

static inline uint32_t radmap_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline size_t radmap_read(FILE *file, unsigned z, unsigned x, unsigned y,
                                 void *buffer, size_t capacity)
{
    if (!file || z > 11) return 0;
    const uint32_t side = 1u << z;
    if (x >= side || y >= side) return 0;
    unsigned char header[RADMAP_HEADER_BYTES], record[RADMAP_RECORD_BYTES];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RADMAP01", 8) != 0 || radmap_u32(header + 8) != 1 ||
        radmap_u32(header + 12) != z || radmap_u32(header + 16) != 256 ||
        radmap_u32(header + 20) != side * side) return 0;
    const uint32_t record_at = RADMAP_HEADER_BYTES + RADMAP_RECORD_BYTES * (y * side + x);
    const uint32_t data_start = RADMAP_HEADER_BYTES + RADMAP_RECORD_BYTES * side * side;
    if (fseek(file, record_at, SEEK_SET) != 0 ||
        fread(record, 1, sizeof(record), file) != sizeof(record)) return 0;
    const uint32_t offset = radmap_u32(record), length = radmap_u32(record + 4);
    if (offset < data_start || length < 24 || length > capacity ||
        (uint64_t)offset + length > LONG_MAX ||
        fseek(file, (long)offset, SEEK_SET) != 0 ||
        fread(buffer, 1, length, file) != length) return 0;
    const unsigned char *png = buffer;
    return memcmp(png, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\1\0\0\0\1\0", 24) == 0
           ? length : 0;
}
