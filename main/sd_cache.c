#include "sd_cache.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdkconfig.h"

static atomic_bool s_ready;

void sd_cache_init(void)
{
#if CONFIG_CANFLIGHT_BOARD_WAVESHARE_7B
    /* The 7B helper leaves EXIO4/DAT3 high. Use the vendor's native
     * one-bit SD wiring, so no SPI chip-select toggles are required. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_12;
    slot.cmd = GPIO_NUM_11;
    slot.d0 = GPIO_NUM_13;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t options = {
        .format_if_mount_failed = false,
        .max_files = 8,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &options, &card);
    if (err != ESP_OK) {
        ESP_LOGI("sd_cache", "SD cache unavailable: %s", esp_err_to_name(err));
        return;
    }
    mkdir("/sd/radar", 0775);
    mkdir("/sd/radar/tiles", 0775);
    mkdir("/sd/radar/photos", 0775);
    atomic_store(&s_ready, true);
    ESP_LOGI("sd_cache", "SD cache mounted");
#endif
}

bool sd_cache_ready(void)
{
    return atomic_load(&s_ready);
}

size_t sd_cache_read(const char *path, void *buffer, size_t capacity)
{
    if (!sd_cache_ready()) return 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    size_t count = fread(buffer, 1, capacity, file);
    if (ferror(file) || (count == capacity && fgetc(file) != EOF)) count = 0;
    fclose(file);
    return count;
}

bool sd_cache_write_tile(const char *directory, const char *path,
                         const void *buffer, size_t size)
{
    if (!sd_cache_ready()) return false;
    uint64_t total, free_bytes;
    if (esp_vfs_fat_info("/sd", &total, &free_bytes) != ESP_OK ||
        free_bytes < UINT64_C(512) * 1024 * 1024 + size) {
        return false;
    }
    mkdir(directory, 0775);
    char temporary[80];
    size_t len = strlen(path);
    if (len < 4 || len >= sizeof(temporary)) return false;
    memcpy(temporary, path, len - 4);
    memcpy(temporary + len - 4, ".tmp", 5);
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    bool written = fwrite(buffer, 1, size, file) == size;
    if (fclose(file) != 0) written = false;
    if (!written || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}
