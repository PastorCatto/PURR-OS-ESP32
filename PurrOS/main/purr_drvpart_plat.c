#include "purr_drvpart_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "purr_drvpart.h"
#include "purr_io.h"
#include "purr_util.h"

static const char *TAG = "drvpart";

static const esp_partition_t *find(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "drivers");
}

static int dp_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    return purr_io_partition_read((const esp_partition_t *)ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

int purr_drvpart_each(purr_drvpart_each_fn fn, void *ctx)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < PURR_DRVPART_SIZE) {
        return -1;
    }
    uint8_t *buf = heap_caps_malloc(PURR_DRVPART_MAX_CAT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(PURR_DRVPART_MAX_CAT);
    }
    if (buf == NULL) {
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < PURR_DRVPART_SLOTS && rc == 0; i++) {
        purr_drvslot_info_t info;
        uint32_t len = 0;
        int e = purr_drvpart_slot_read(dp_read, (void *)p, i, buf, PURR_DRVPART_MAX_CAT, &len, &info);
        if (e == PURR_DRVPART_EMPTY) {
            continue;
        }
        if (e != 0) {
            ESP_LOGW(TAG, "slot %d is damaged or unreadable (%d): skipped", i, e);
            continue;
        }
        rc = fn(ctx, info.file, buf, len);
    }
    free(buf);
    return rc;
}

static int count_cb(void *ctx, const char *file, const uint8_t *cat, uint32_t len)
{
    (void)file; (void)cat; (void)len;
    (*(int *)ctx)++;
    return 0;
}

int purr_drvpart_count(void)
{
    int n = 0;
    return purr_drvpart_each(count_cb, &n) == 0 ? n : 0;
}

int purr_drvpart_has_file(const char *file)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < PURR_DRVPART_SIZE || file == NULL) {
        return 0;
    }
    for (int i = 0; i < PURR_DRVPART_SLOTS; i++) {
        purr_drvslot_info_t info;
        if (purr_drvpart_slot_info(dp_read, (void *)p, i, &info) == 0 && strcmp(info.file, file) == 0) {
            return 1;
        }
    }
    return 0;
}

int purr_drvpart_present(void)
{
    const esp_partition_t *p = find();
    return p != NULL && p->size >= PURR_DRVPART_SIZE;
}

int purr_drvpart_matches(uint32_t size, const uint8_t sha256[32])
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < PURR_DRVPART_SIZE || size == 0 || size > PURR_DRVPART_SIZE) {
        return 0;
    }
    uint8_t chunk[512];
    purr_sha256_t sha;
    uint8_t digest[32];
    purr_sha256_init(&sha);
    for (uint32_t off = 0; off < size; off += sizeof(chunk)) {
        uint32_t n = size - off < sizeof(chunk) ? size - off : (uint32_t)sizeof(chunk);
        if (purr_io_partition_read(p, off, chunk, n) != ESP_OK) {
            return 0;
        }
        purr_sha256_update(&sha, chunk, n);
    }
    purr_sha256_final(&sha, digest);
    if (memcmp(digest, sha256, sizeof(digest)) != 0) {
        return 0;
    }
    for (uint32_t off = size; off < PURR_DRVPART_SIZE; off += sizeof(chunk)) {      /* a driver beyond the image is not "exactly this" */
        uint32_t n = PURR_DRVPART_SIZE - off < sizeof(chunk) ? PURR_DRVPART_SIZE - off : (uint32_t)sizeof(chunk);
        if (purr_io_partition_read(p, off, chunk, n) != ESP_OK) {
            return 0;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (chunk[i] != 0xFF) {
                return 0;
            }
        }
    }
    return 1;
}

static void why_set(char *why, size_t cap, const char *text)
{
    if (why != NULL && cap > 0) {
        snprintf(why, cap, "%s", text);
    }
}

int purr_drvpart_install(const uint8_t *image, uint32_t len, char *why, size_t cap)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < PURR_DRVPART_SIZE) {
        why_set(why, cap, "this board has no drivers partition");
        return -1;
    }
    int n = purr_drvpart_check_image(image, len);
    if (n < 0) {
        why_set(why, cap, n == PURR_DRVIMG_BAD_SIZE ? "the drivers image has the wrong size" : n == PURR_DRVIMG_DUPLICATE ? "the drivers image lists a driver twice" : "a driver slot in the image is damaged");
        return -1;
    }
    if (purr_io_partition_erase_range(p, 0, p->size) != ESP_OK || purr_io_partition_write(p, 0, image, len) != ESP_OK) {
        why_set(why, cap, "writing the drivers partition failed");
        return -1;
    }
    uint8_t chunk[512];
    for (uint32_t off = 0; off < len; off += sizeof(chunk)) {
        uint32_t m = len - off < sizeof(chunk) ? len - off : (uint32_t)sizeof(chunk);
        if (purr_io_partition_read(p, off, chunk, m) != ESP_OK || memcmp(chunk, image + off, m) != 0) {
            why_set(why, cap, "the drivers partition did not read back what was written");
            return -1;
        }
    }
    ESP_LOGI(TAG, "drivers partition written: %d driver(s)", n);
    return 0;
}
