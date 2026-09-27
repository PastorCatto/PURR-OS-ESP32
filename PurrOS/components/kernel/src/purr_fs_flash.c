/* The root filesystem's block device: a flash partition. */
#include "esp_partition.h"

#include "purr_fs.h"

#define BLOCK 4096

static int fl_read(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t size)
{
    return esp_partition_read(ctx, block * BLOCK + off, buf, size) == ESP_OK ? 0 : -1;
}

static int fl_prog(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t size)
{
    return esp_partition_write(ctx, block * BLOCK + off, buf, size) == ESP_OK ? 0 : -1;
}

static int fl_erase(void *ctx, uint32_t block)
{
    return esp_partition_erase_range(ctx, block * BLOCK, BLOCK) == ESP_OK ? 0 : -1;
}

int purr_fs_flash_bd(const char *label, purr_bd_t *bd)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (p == NULL) {
        return -1;
    }
    bd->read = fl_read;
    bd->prog = fl_prog;
    bd->erase = fl_erase;
    bd->sync = NULL;
    bd->ctx = (void *)p;
    bd->block_size = BLOCK;
    bd->block_count = p->size / BLOCK;
    return 0;
}
