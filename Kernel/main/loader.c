/*
 * loader.c - kernel_load_relocatable_file(), ported from commands.c's load_relocatable_file()
 * (PurrOS/main/commands.c, Modules/SPEC.md section 7) since this kernel doesn't link
 * PurrOS/main at all. Same mechanism, same steps; only the filesystem/key-bag plumbing is
 * local to this project instead of the monolith's s_fs/purr_default_keys globals.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"

#include "purr_abi.h"
#include "purr_cfg.h"
#include "purr_cfgstore.h"
#include "purr_crypto_mbedtls.h"
#include "purr_fs.h"
#include "purr_keybag.h"
#include "purr_module.h"
#include "purr_relocate.h"
#include "purr_verify.h"

#include "loader.h"

static const char *TAG = "loader";

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
} read_ctx_t;

static int accumulate(void *ctx, const void *data, uint32_t len)
{
    read_ctx_t *c = ctx;
    if (c->len + len > c->cap) {
        return -1;
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    return 0;
}

kernel_load_result_t kernel_load_relocatable_file(purr_fs_t *fs, const char *path,
                                                   uint32_t read_cap, uint32_t max_pages,
                                                   uint8_t **out_databuf, void **out_exec_ptr,
                                                   uint32_t *out_entry_offset)
{
    uint8_t *rb = malloc(read_cap);
    if (rb == NULL) {
        return KERNEL_LOAD_TRANSIENT;
    }
    read_ctx_t rc = {rb, read_cap, 0};
    if (purr_fs_read(fs, path, accumulate, &rc) < 0) {
        free(rb);
        return KERNEL_LOAD_TRANSIENT;
    }

    purr_cfg_t cfg;
    purr_flash_t fl;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);
    purr_verify_env_t env = {.chip_id = PURR_CHIP_ESP32S3, .bag = &bag, .crypto = &purr_crypto_mbedtls};
    purr_mem_read_ctx_t mrc = {rb, rc.len};
    purr_image_header_t hdr;
    purr_verify_result_t vr = purr_image_verify(&env, purr_mem_read, &mrc, rc.len, &hdr);
    if (vr != PURR_V_OK || hdr.image_type != PURR_IMG_MODULE) {
        ESP_LOGW(TAG, "%s: %s", path, vr != PURR_V_OK ? purr_verify_name(vr) : "not a module image");
        free(rb);
        return KERNEL_LOAD_UNTRUSTED;
    }
    if ((uint64_t)hdr.payload_offset + hdr.payload_size > rc.len) {
        ESP_LOGW(TAG, "%s: payload runs past the file", path);
        free(rb);
        return KERNEL_LOAD_UNTRUSTED;
    }

    const uint8_t *payload = rb + hdr.payload_offset;
    purr_module_layout_t layout;
    purr_module_layout_result_t lr = purr_module_parse_layout(payload, hdr.payload_size, &layout);
    if (lr != PURR_MOD_LAYOUT_OK) {
        ESP_LOGW(TAG, "%s: bad layout: %s", path, purr_module_layout_result_name(lr));
        free(rb);
        return KERNEL_LOAD_UNTRUSTED;
    }

    uint32_t *data_offsets = NULL;
    if (layout.data_reloc_count > 0) {
        data_offsets = malloc(layout.data_reloc_count * sizeof(uint32_t));
        if (data_offsets == NULL) {
            free(rb);
            return KERNEL_LOAD_TRANSIENT;
        }
        purr_module_read_data_relocs(payload, &layout, data_offsets);
    }
    uint32_t *code_offsets = NULL;
    if (layout.code_reloc_count > 0) {
        code_offsets = malloc(layout.code_reloc_count * sizeof(uint32_t));
        if (code_offsets == NULL) {
            free(data_offsets);
            free(rb);
            return KERNEL_LOAD_TRANSIENT;
        }
        purr_module_read_code_relocs(payload, &layout, code_offsets);
    }

    uint32_t want = ((layout.code_size + CONFIG_MMU_PAGE_SIZE - 1) / CONFIG_MMU_PAGE_SIZE) *
                    CONFIG_MMU_PAGE_SIZE;
    if (want > (uint64_t)max_pages * CONFIG_MMU_PAGE_SIZE) {
        ESP_LOGW(TAG, "%s: too big (%u bytes, room for %u page(s))", path,
                (unsigned)layout.code_size, (unsigned)max_pages);
        free(data_offsets);
        free(code_offsets);
        free(rb);
        return KERNEL_LOAD_UNTRUSTED;
    }
    uint8_t *databuf = heap_caps_aligned_alloc(CONFIG_MMU_PAGE_SIZE, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (databuf == NULL) {
        free(data_offsets);
        free(code_offsets);
        free(rb);
        return KERNEL_LOAD_TRANSIENT;
    }
    memset(databuf, 0, want);
    memcpy(databuf, payload + layout.code_offset, layout.code_size);

    purr_reloc_result_t rr = purr_relocate(databuf, want, data_offsets, layout.data_reloc_count,
                                          (uint32_t)(uintptr_t)databuf);
    free(data_offsets);
    if (rr != PURR_RELOC_OK) {
        ESP_LOGW(TAG, "%s: data relocation failed: %s", path, purr_reloc_result_name(rr));
        free(code_offsets);
        free(rb);
        free(databuf);
        return KERNEL_LOAD_UNTRUSTED;
    }

    esp_err_t ee = esp_cache_msync(databuf, want, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_paddr_t paddr = 0;
    mmu_target_t target = 0;
    if (ee == ESP_OK) ee = esp_mmu_vaddr_to_paddr(databuf, &paddr, &target);
    void *exec_ptr = NULL;
    if (ee == ESP_OK) {
        ee = esp_mmu_map(paddr, want, MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ,
                         ESP_MMU_MMAP_FLAG_PADDR_SHARED, &exec_ptr);
    }
    if (ee != ESP_OK) {
        ESP_LOGW(TAG, "%s: PSRAM exec mapping failed: %s", path, esp_err_to_name(ee));
        free(code_offsets);
        free(rb);
        free(databuf);
        return KERNEL_LOAD_TRANSIENT;
    }

    rr = purr_relocate(databuf, want, code_offsets, layout.code_reloc_count,
                       (uint32_t)(uintptr_t)exec_ptr);
    free(code_offsets);
    free(rb);
    if (rr != PURR_RELOC_OK) {
        ESP_LOGW(TAG, "%s: code relocation failed: %s", path, purr_reloc_result_name(rr));
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return KERNEL_LOAD_UNTRUSTED;
    }

    ee = esp_cache_msync(databuf, want, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ee == ESP_OK) {
        ee = esp_cache_msync(exec_ptr, want,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }
    if (ee != ESP_OK) {
        ESP_LOGW(TAG, "%s: post-relocation cache sync failed: %s", path, esp_err_to_name(ee));
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return KERNEL_LOAD_TRANSIENT;
    }

    *out_databuf = databuf;
    *out_exec_ptr = exec_ptr;
    *out_entry_offset = layout.entry_offset;
    return KERNEL_LOAD_OK;
}
