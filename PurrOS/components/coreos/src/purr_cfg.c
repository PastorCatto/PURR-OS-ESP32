#include "purr_cfg.h"

#include <string.h>

#include "purr_util.h"

#define CFG_CRC_LEN  (sizeof(purr_cfg_t) - sizeof(uint32_t))

void purr_cfg_seal(purr_cfg_t *cfg)
{
    cfg->crc32 = purr_crc32(0, cfg, CFG_CRC_LEN);
}

void purr_cfg_defaults(purr_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = PURR_CFG_MAGIC;
    cfg->version = PURR_CFG_VERSION;
    cfg->size = (uint16_t)sizeof(purr_cfg_t);
    cfg->secure_mode = PURR_SECURE_WARN;
    cfg->boot_target = PURR_TARGET_NORMAL;
    purr_cfg_seal(cfg);
}

int purr_cfg_valid(const purr_cfg_t *cfg)
{
    if (cfg->magic != PURR_CFG_MAGIC || cfg->version != PURR_CFG_VERSION ||
        cfg->size != sizeof(purr_cfg_t)) {
        return 0;
    }
    return cfg->crc32 == purr_crc32(0, cfg, CFG_CRC_LEN);
}

static uint32_t sector_addr(const purr_flash_t *fl, int idx)
{
    return fl->base + (uint32_t)idx * PURR_CFG_SECTOR_SIZE;
}

/* Read both sectors. Returns -1 on read error, else fills ok[] and copy[]. */
static int read_both(const purr_flash_t *fl, purr_cfg_t copy[2], int ok[2])
{
    for (int i = 0; i < PURR_CFG_SECTORS; i++) {
        if (fl->read(fl->ctx, sector_addr(fl, i), &copy[i], sizeof(purr_cfg_t)) != 0) {
            return -1;
        }
        ok[i] = purr_cfg_valid(&copy[i]);
    }
    return 0;
}

static int newest(const purr_cfg_t copy[2], const int ok[2])
{
    if (ok[0] && ok[1]) {
        return (copy[1].seq > copy[0].seq) ? 1 : 0;
    }
    if (ok[0]) {
        return 0;
    }
    if (ok[1]) {
        return 1;
    }
    return -1;
}

int purr_cfg_load(const purr_flash_t *fl, purr_cfg_t *out, int *which)
{
    purr_cfg_t copy[2];
    int ok[2];

    if (read_both(fl, copy, ok) != 0) {
        return -1;
    }
    int n = newest(copy, ok);
    if (which) {
        *which = n;
    }
    if (n < 0) {
        purr_cfg_defaults(out);
        return 1;
    }
    *out = copy[n];
    return 0;
}

int purr_cfg_store(const purr_flash_t *fl, purr_cfg_t *cfg)
{
    purr_cfg_t copy[2], verify;
    int ok[2];

    if (read_both(fl, copy, ok) != 0) {
        return -1;
    }
    int n = newest(copy, ok);
    int target = (n == 0) ? 1 : 0;              /* the other sector, or A if none */
    uint32_t next_seq = (n < 0) ? 1u : copy[n].seq + 1u;

    cfg->magic = PURR_CFG_MAGIC;
    cfg->version = PURR_CFG_VERSION;
    cfg->size = (uint16_t)sizeof(purr_cfg_t);
    cfg->seq = next_seq;
    purr_cfg_seal(cfg);

    uint32_t addr = sector_addr(fl, target);
    if (fl->erase(fl->ctx, addr) != 0) {
        return -1;
    }
    if (fl->write(fl->ctx, addr, cfg, sizeof(purr_cfg_t)) != 0) {
        return -1;
    }
    if (fl->read(fl->ctx, addr, &verify, sizeof(verify)) != 0) {
        return -1;
    }
    if (memcmp(&verify, cfg, sizeof(verify)) != 0) {
        return -1;
    }
    return 0;
}
