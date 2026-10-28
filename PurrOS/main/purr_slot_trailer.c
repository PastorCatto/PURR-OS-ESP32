#include "purr_slot_trailer.h"

#include <string.h>

#include "purr_cfg.h"
#include "purr_cfgstore.h"

static int slot_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    const esp_partition_t *p = ctx;
    if ((uint64_t)offset + len > p->size) {
        return -1;
    }
    return esp_partition_read(p, offset, buf, len) == ESP_OK ? 0 : -1;
}

int purr_slot_trailer_fits(const esp_partition_t *slot, uint32_t payload_size)
{
    return slot->size > PURR_TRAILER_SIZE && payload_size <= PURR_TRAILER_MAX_IMAGE(slot->size);
}

static uint32_t bad_bit(const esp_partition_t *slot)
{
    if (slot == NULL) {
        return 0;
    }
    return strcmp(slot->label, "kernel") == 0 ? PURR_CFGF_BAD_KERNEL : strcmp(slot->label, "kittenos") == 0 ? PURR_CFGF_BAD_KITTENOS : 0;
}

int purr_slot_marked_bad(const esp_partition_t *slot)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    uint32_t bit = bad_bit(slot);
    return bit != 0 && purr_cfgstore_open(&fl) == 0 && purr_cfg_load(&fl, &cfg, NULL) >= 0 && (cfg.flags & bit) != 0;
}

static void clear_bad(const esp_partition_t *slot)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    uint32_t bit = bad_bit(slot);
    if (bit != 0 && purr_cfgstore_open(&fl) == 0 && purr_cfg_load(&fl, &cfg, NULL) >= 0 && (cfg.flags & bit) != 0) {
        cfg.flags &= ~bit;
        purr_cfg_store(&fl, &cfg);
    }
}

int purr_slot_trailer_write(const esp_partition_t *slot, const purr_image_header_t *hdr)
{
    if (slot->size <= PURR_TRAILER_SIZE) {
        return -1;
    }
    uint32_t off = PURR_TRAILER_OFFSET(slot->size);
    if (esp_partition_write(slot, off, hdr, sizeof(*hdr)) != ESP_OK) {
        return -1;
    }
    purr_image_header_t back;
    if (esp_partition_read(slot, off, &back, sizeof(back)) != ESP_OK ||
        memcmp(&back, hdr, sizeof(back)) != 0) {
        return -1;
    }
    clear_bad(slot);                          /* the new image is in: the reset's mark on this slot is done */
    return 0;
}

int purr_slot_trailer_header(const esp_partition_t *slot, purr_image_header_t *out)
{
    if (slot->size <= PURR_TRAILER_SIZE) {
        return -1;
    }
    if (esp_partition_read(slot, PURR_TRAILER_OFFSET(slot->size), out, sizeof(*out)) != ESP_OK) {
        return -1;
    }
    return out->magic == PURR_IMAGE_MAGIC ? 0 : -1;
}

purr_verify_result_t purr_slot_trailer_check(const purr_verify_env_t *env,
                                             const esp_partition_t *slot,
                                             uint8_t want_type, uint8_t want_subtype)
{
    return purr_trailer_verify(env, slot_read, (void *)slot, slot->size, want_type, want_subtype, NULL);
}
