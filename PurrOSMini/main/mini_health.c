/* mini_health.c - see mini_health.h. The bootloader's failure ladder (bootloader/SPEC.md section 6) climbs on every boot that never says it is healthy; with no loader slot to escalate to it
 * only logs, but a count that grows for ever is wrong and the full OS clears it. This is the same step over the same purrcfg code (coreos purr_cfg.c). */
#include "mini_health.h"

#include "esp_log.h"
#include "esp_partition.h"

#include "purr_cfg.h"

static const char *TAG = "mini-health";

static int rd(void *ctx, uint32_t a, void *b, uint32_t n) { return esp_partition_read(ctx, a, b, n) == ESP_OK ? 0 : 1; }
static int er(void *ctx, uint32_t a) { return esp_partition_erase_range(ctx, a, 4096) == ESP_OK ? 0 : 1; }
static int wr(void *ctx, uint32_t a, const void *b, uint32_t n) { return esp_partition_write(ctx, a, b, n) == ESP_OK ? 0 : 1; }

void mini_mark_healthy(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "purrcfg");
    if (p == NULL) {
        return;
    }
    purr_flash_t fl = {rd, er, wr, (void *)p, 0};
    purr_cfg_t cfg;
    if (purr_cfg_load(&fl, &cfg, NULL) < 0 || cfg.boot_fail_count == 0) {
        return;                                                  /* a read error leaves it alone; a count of 0 needs no write */
    }
    cfg.boot_fail_count = 0;
    if (purr_cfg_store(&fl, &cfg) == 0) {
        ESP_LOGI(TAG, "boot recorded as healthy");
    }
}
