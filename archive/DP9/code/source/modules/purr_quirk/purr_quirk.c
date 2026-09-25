// purr_quirk.c — see purr_quirk.h for the full picture.
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "purr_quirk_pkg.h"
#include "purr_quirk.h"

static const char *TAG = "purr_quirk";

static uint8_t *s_buf      = NULL;   // whole file, header + all block payloads
static size_t   s_buf_size = 0;

static const purr_quirk_pkg_header_t *header(void)
{
    return (const purr_quirk_pkg_header_t *)s_buf;
}

void purr_quirk_unload(void)
{
    if (s_buf) heap_caps_free(s_buf);
    s_buf      = NULL;
    s_buf_size = 0;
}

bool purr_quirk_loaded(void)
{
    return s_buf != NULL;
}

bool purr_quirk_load(const char *path)
{
    // NOTE: do NOT unload the currently-loaded package here. Callers probe
    // multiple candidate paths in sequence (e.g. /flash then /sdcard) and
    // expect a later miss to leave an earlier hit intact — the previously
    // loaded package is only replaced once a new one actually validates
    // successfully, right before it's installed below.

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "no quirk package at %s (not an error — using compiled-in defaults)", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof(purr_quirk_pkg_header_t)) {
        ESP_LOGE(TAG, "%s too small to hold a valid header (%ld bytes)", path, fsize);
        fclose(f);
        return false;
    }

    uint8_t *buf = heap_caps_malloc((size_t)fsize, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "alloc failed (%ld bytes) for %s", fsize, path);
        fclose(f);
        return false;
    }
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (read_n != (size_t)fsize) {
        ESP_LOGE(TAG, "short read of %s (%u of %ld bytes)", path, (unsigned)read_n, fsize);
        heap_caps_free(buf);
        return false;
    }

    const purr_quirk_pkg_header_t *hdr = (const purr_quirk_pkg_header_t *)buf;
    if (hdr->magic != PURR_QUIRK_MAGIC) {
        ESP_LOGE(TAG, "%s: bad magic (0x%08x) — not a quirk package (wrong file, or "
                      "it's a Gen-1 .purr code module, a different format entirely)",
                 path, (unsigned)hdr->magic);
        heap_caps_free(buf);
        return false;
    }
    if (hdr->abi_version != PURR_QUIRK_ABI_VERSION) {
        ESP_LOGE(TAG, "%s: ABI mismatch (package=%u, loader=%u)",
                 path, (unsigned)hdr->abi_version, (unsigned)PURR_QUIRK_ABI_VERSION);
        heap_caps_free(buf);
        return false;
    }
    if (hdr->block_count > PURR_QUIRK_MAX_BLOCKS) {
        ESP_LOGE(TAG, "%s: block_count %u exceeds PURR_QUIRK_MAX_BLOCKS %d",
                 path, (unsigned)hdr->block_count, PURR_QUIRK_MAX_BLOCKS);
        heap_caps_free(buf);
        return false;
    }
    // Every declared block's payload must actually fit inside the file we
    // just read — a corrupt or hand-edited package could otherwise point
    // purr_quirk_get_block() at memory past the end of this buffer.
    for (int i = 0; i < hdr->block_count; i++) {
        const purr_quirk_block_t *b = &hdr->blocks[i];
        if (b->size == 0) continue;   // unused slot
        uint64_t end = (uint64_t)b->offset + (uint64_t)b->size;
        if (end > (uint64_t)fsize) {
            ESP_LOGE(TAG, "%s: block '%.24s' [%u..%u) runs past end of file (%ld bytes)",
                     path, b->name, (unsigned)b->offset, (unsigned)end, fsize);
            heap_caps_free(buf);
            return false;
        }
    }

    purr_quirk_unload();   // new package validated — now safe to drop the old one
    s_buf      = buf;
    s_buf_size = (size_t)fsize;
    ESP_LOGI(TAG, "loaded %s: device='%.32s' %u block(s), %u bytes",
             path, hdr->device_name, (unsigned)hdr->block_count, (unsigned)fsize);
    return true;
}

const void *purr_quirk_get_block(const char *name, size_t *out_size)
{
    if (!s_buf || !name) return NULL;
    const purr_quirk_pkg_header_t *hdr = header();
    for (int i = 0; i < hdr->block_count; i++) {
        const purr_quirk_block_t *b = &hdr->blocks[i];
        if (b->size == 0) continue;
        if (strncmp(b->name, name, PURR_QUIRK_NAME_MAX) == 0) {
            if (out_size) *out_size = b->size;
            return s_buf + b->offset;
        }
    }
    return NULL;
}
