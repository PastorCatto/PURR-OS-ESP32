// Reads purrcfg (bootloader/SPEC.md section 5) and acts on the one-shot recovery request.
#include <stdint.h>
#include <string.h>

#include "bootloader_flash_priv.h"
/* Our own lines stay at INFO even though the rest of the bootloader logs at WARN. */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"

#include "purr_abi.h"
#include "purr_bootcfg.h"
#include "purr_bootpkg.h"
#include "purr_crypto_uecc.h"
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

/* purr_find_partition() reads from a RAM copy of the table (purr_bootpkg.c), so it is cheap
 * and never maps flash; no caching is needed here. */
static bool find_purrcfg(uint32_t *off, uint32_t *size)
{
    return purr_find_partition("purrcfg", off, size) && *size >= PURR_CFG_SECTORS * SECTOR;
}

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

static purr_cfg_t s_view;
static purr_trust_t s_trust;
static bool s_published;

void purr_bootcfg_publish(const purr_cfg_t *cfg)
{
    purr_trust_eval(cfg, purr_default_keys, purr_default_keys_count, &purr_crypto_uecc, &s_trust);
    purr_trust_view(cfg, &s_trust, &s_view);
    s_published = true;
}

static void ensure_published(void)
{
    if (!s_published) {
        purr_cfg_t cfg;
        purr_bootcfg_load(&cfg);        /* defaults (secure_mode = warn) if unreadable */
        purr_bootcfg_publish(&cfg);
    }
}

const purr_cfg_t *purr_bootcfg_view(void)
{
    ensure_published();
    return &s_view;
}

const purr_trust_t *purr_bootcfg_trust(void)
{
    ensure_published();
    return &s_trust;
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
