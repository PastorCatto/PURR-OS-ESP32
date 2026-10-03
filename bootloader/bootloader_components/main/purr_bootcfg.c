// Reads purrcfg (bootloader/SPEC.md section 5) and acts on the one-shot recovery request.
#include <stdint.h>
#include <string.h>

#include "bootloader_flash_priv.h"
#include "esp_log.h"

#include "purr_abi.h"
#include "purr_bootcfg.h"
#include "purr_bootpkg.h"
#include "purr_cfg.h"

static const char *TAG = "purr_boot";

#define SECTOR 4096

// purrcfg's struct is packed, so it may sit at any address, but the flash routines want
// whole words at aligned addresses. Everything goes through this aligned buffer.
static uint32_t s_bounce[256];

static int fl_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (len > sizeof(s_bounce) || (len % 4) != 0) {
        return -1;
    }
    if (bootloader_flash_read(addr, s_bounce, len, false) != ESP_OK) {
        return -1;
    }
    memcpy(buf, s_bounce, len);
    return 0;
}

static int fl_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    return bootloader_flash_erase_sector(addr / SECTOR) == ESP_OK ? 0 : -1;
}

static int fl_write(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    (void)ctx;
    if (len > sizeof(s_bounce) || (len % 4) != 0) {
        return -1;
    }
    memcpy(s_bounce, buf, len);
    return bootloader_flash_write(addr, s_bounce, len, false) == ESP_OK ? 0 : -1;
}

/* purrcfg's own offset never moves during a boot, and purr_find_partition() maps and unmaps
 * the whole partition table through bootloader_mmap() -- which only ever holds one mapping at
 * a time in the bootloader stage (bootloader_flash.c). Every extra call this session added
 * (the boot_fail_count load/store, on top of the pre-existing FORCE_RECOVERY/FORCE_LOADER
 * checks and the boot package's own load) pushed the count of these calls per boot from 3 to
 * 5+, and one of them was landing while a previous mapping hadn't been torn down yet --
 * "tried to bootloader_mmap twice", found on real hardware, 2026-09-29. Finding the partition
 * once and caching it here, instead of on every call, is the fix: it also just avoids
 * rescanning the same unchanging partition table five times a boot. */
static uint32_t s_off, s_size;
static bool s_found, s_looked;

static bool find_purrcfg(uint32_t *off, uint32_t *size)
{
    if (!s_looked) {
        s_looked = true;
        s_found = purr_find_partition("purrcfg", &s_off, &s_size) &&
                 s_size >= PURR_CFG_SECTORS * SECTOR;
    }
    *off = s_off;
    *size = s_size;
    return s_found;
}

bool purr_bootcfg_load(purr_cfg_t *out)
{
    uint32_t off, size;
    if (!find_purrcfg(&off, &size)) {
        purr_cfg_defaults(out);
        return false;
    }
    purr_flash_t fl = {fl_read, fl_erase, fl_write, NULL, off};
    return purr_cfg_load(&fl, out, NULL) == 0;
}

bool purr_bootcfg_store(purr_cfg_t *cfg)
{
    uint32_t off, size;
    if (!find_purrcfg(&off, &size)) {
        return false;
    }
    purr_flash_t fl = {fl_read, fl_erase, fl_write, NULL, off};
    return purr_cfg_store(&fl, cfg) == 0;
}

bool purr_bootcfg_take_flag(uint32_t flag)
{
    uint32_t off, size;
    if (!find_purrcfg(&off, &size)) {
        return false;
    }
    purr_flash_t fl = {fl_read, fl_erase, fl_write, NULL, off};

    purr_cfg_t cfg;
    if (purr_cfg_load(&fl, &cfg, NULL) != 0) {
        return false;                       // unreadable, or nothing written yet: no request
    }
    if (!purr_cfg_take_flag(&cfg, flag)) {
        return false;
    }
    // Clear the request before acting on it, so a crash in what it starts cannot loop here.
    if (purr_cfg_store(&fl, &cfg) != 0) {
        ESP_LOGW(TAG, "purrcfg: could not clear a one-shot boot request");
    }
    return true;
}
