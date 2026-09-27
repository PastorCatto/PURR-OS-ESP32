#include "purr_cfgstore.h"

#include "esp_partition.h"

#define SECTOR 4096

static int cs_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    return esp_partition_read(ctx, addr, buf, len) == ESP_OK ? 0 : -1;
}

static int cs_erase(void *ctx, uint32_t addr)
{
    return esp_partition_erase_range(ctx, addr, SECTOR) == ESP_OK ? 0 : -1;
}

static int cs_write(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    return esp_partition_write(ctx, addr, buf, len) == ESP_OK ? 0 : -1;
}

int purr_cfgstore_open(purr_flash_t *fl)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "purrcfg");
    if (p == NULL || p->size < PURR_CFG_SECTORS * SECTOR) {
        return -1;
    }
    fl->read = cs_read;
    fl->erase = cs_erase;
    fl->write = cs_write;
    fl->ctx = (void *)p;
    fl->base = 0;
    return 0;
}
