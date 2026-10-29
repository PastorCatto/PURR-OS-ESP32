#include "esp_attr.h"
#include "purr_ui_assets.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_util.h"

static const char *TAG = "ui_assets";

#define MAGIC 0x54534150u   /* 'PAST', little-endian: "Purr ASset Table" */
#define MAX_ASSETS 128

/* On-flash layout (verified byte-for-byte against the real xtensa-esp32s3-elf cross-compiler
 * before writing purrstrap's packer to match -- the netrec record_t struct earlier in this
 * project got bitten by exactly the padding mistake this was checked against):
 *   purr_assets_header_t
 *   purr_assets_dir_entry_t[header.count]
 *   pixel data, concatenated, each entry's offset/length pointing into this region
 * crc32 covers everything from the end of the header to the end of the pixel data. */
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t crc32;
} header_t;

typedef struct {
    char name[24];
    uint32_t offset;       /* from the start of the pixel-data region */
    uint32_t length;
    uint16_t width;
    uint16_t height;
    uint8_t color_format;  /* PURR_ASSET_RGB565 or PURR_ASSET_RGB565A8 */
    uint8_t reserved[3];
} dir_entry_t;

static EXT_RAM_BSS_ATTR purr_asset_t s_assets[MAX_ASSETS];
static int s_count;
static esp_partition_mmap_handle_t s_mmap_handle;
static int s_mapped;
static volatile int s_bad;                  /* the background check found the image data damaged: hand out nothing more */

typedef struct {
    const void *dir;
    size_t dir_bytes;
    const void *pixels;
    size_t used;
    uint32_t want;
} crc_job_t;

/* The CRC over several MB of flash takes seconds. Done before the shell was drawn it left a white screen for all that time, so it runs
 * after, in small steps. The directory's bounds are checked up front (nothing can read outside the partition); the CRC only guards the
 * pixels themselves, so a bad one just means the pictures stop being offered. */
static void crc_task(void *arg)
{
    crc_job_t j = *(crc_job_t *)arg;
    free(arg);
    uint32_t crc = purr_crc32(0, j.dir, j.dir_bytes);
    const uint8_t *p = j.pixels;
    for (size_t off = 0; off < j.used; off += 16384) {
        size_t n = j.used - off < 16384 ? j.used - off : 16384;
        crc = purr_crc32(crc, p + off, n);
        vTaskDelay(1);
    }
    if (crc != j.want) {
        s_bad = 1;
        ESP_LOGW(TAG, "CRC mismatch (corrupt or partial write): the pictures are switched off");
    } else {
        ESP_LOGI(TAG, "image data checked");
    }
    vTaskDelete(NULL);
}

esp_err_t purr_ui_assets_init(void)
{
    s_count = 0;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ui_assets");
    if (part == NULL) {
        ESP_LOGW(TAG, "no ui_assets partition on this board");
        return ESP_ERR_NOT_FOUND;
    }

    const void *base;
    esp_err_t e = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                     &base, &s_mmap_handle);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mmap failed: %s", esp_err_to_name(e));
        return e;
    }
    s_mapped = 1;

    const header_t *hdr = (const header_t *)base;
    if (hdr->magic != MAGIC) {
        ESP_LOGW(TAG, "blank or unrecognized (magic %08x)", (unsigned)hdr->magic);
        return ESP_ERR_INVALID_CRC;
    }
    if (hdr->count > MAX_ASSETS) {
        ESP_LOGW(TAG, "%u assets, more than the %d this build can index", (unsigned)hdr->count,
                 MAX_ASSETS);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t dir_bytes = (size_t)hdr->count * sizeof(dir_entry_t);
    const dir_entry_t *dir = (const dir_entry_t *)((const uint8_t *)base + sizeof(header_t));
    const uint8_t *pixel_region = (const uint8_t *)dir + dir_bytes;

    /* CRC over the directory and every entry's declared pixel range -- not the whole
     * mapped partition (which is padded to the partition's full size with erased 0xFF
     * flash past whatever was actually written). */
    /* `used` is how far into the pixel region the last entry reaches. (It used to start at
     * dir_bytes and so measured the region from the wrong end: the CRC then covered dir_bytes too
     * few bytes and never matched the packer's, which is why a real pack read as "corrupt".) */
    size_t used = 0;
    for (uint32_t i = 0; i < hdr->count; i++) {
        uint64_t end = (uint64_t)dir[i].offset + dir[i].length;
        if (end > part->size - sizeof(header_t) - dir_bytes) {
            ESP_LOGW(TAG, "entry %u's pixel range runs past the partition", (unsigned)i);
            return ESP_ERR_INVALID_SIZE;
        }
        if (end > used) {
            used = (size_t)end;
        }
    }
    s_bad = 0;
    crc_job_t *job = malloc(sizeof(*job));
    bool check = false;
    if (job != NULL) {
        job->dir = dir;
        job->dir_bytes = dir_bytes;
        job->pixels = pixel_region;
        job->used = used;
        job->want = hdr->crc32;
        check = xTaskCreate(crc_task, "assets-crc", 3072, job, 1, NULL) == pdPASS;
        if (!check) {
            free(job);
        }
    }
    if (!check) {                                  /* no task: do it here, as before */
        uint32_t crc = purr_crc32(0, dir, dir_bytes);
        crc = purr_crc32(crc, pixel_region, used);
        if (crc != hdr->crc32) {
            ESP_LOGW(TAG, "CRC mismatch (corrupt or partial write)");
            return ESP_ERR_INVALID_CRC;
        }
    }

    for (uint32_t i = 0; i < hdr->count; i++) {
        purr_asset_t *a = &s_assets[s_count];
        memcpy(a->name, dir[i].name, sizeof(a->name));
        a->name[sizeof(a->name) - 1] = '\0';
        a->pixels = (const uint16_t *)(pixel_region + dir[i].offset);
        a->width = dir[i].width;
        a->height = dir[i].height;
        a->color_format = dir[i].color_format;
        s_count++;
    }
    ESP_LOGI(TAG, "up: %d asset(s)", s_count);
    return ESP_OK;
}

const purr_asset_t *purr_ui_assets_find(const char *name)
{
    if (!s_mapped || s_bad) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_assets[i].name, name) == 0) {
            return &s_assets[i];
        }
    }
    return NULL;
}
