// magicmac_cfg.c — load mac.conf from next to the binary

#include "magicmac_cfg.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "magicmac_cfg";

void magicmac_cfg_defaults(magicmac_cfg_t *out)
{
    strncpy(out->rom_path, MAGICMAC_DEFAULT_ROM_PATH, sizeof(out->rom_path) - 1);
    out->ram_kb     = MAGICMAC_DEFAULT_RAM_KB;
    out->scale_mode = MAGICMAC_DEFAULT_SCALE;
    out->display_w  = 0;
    out->display_h  = 0;
}

bool magicmac_cfg_load(const char *cfg_path, magicmac_cfg_t *out)
{
    magicmac_cfg_defaults(out);

    FILE *f = fopen(cfg_path, "r");
    if (!f) {
        ESP_LOGI(TAG, "no mac.conf at %s — using defaults", cfg_path);
        return false;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        // Trim leading spaces from both
        while (*key == ' ') key++;
        while (*val == ' ') val++;

        // Trim trailing spaces from key
        char *kend = key + strlen(key) - 1;
        while (kend > key && *kend == ' ') *kend-- = '\0';

        if (strcmp(key, "rom_path") == 0) {
            strncpy(out->rom_path, val, sizeof(out->rom_path) - 1);
        } else if (strcmp(key, "ram_kb") == 0) {
            out->ram_kb = (uint32_t)atoi(val);
            if (out->ram_kb < 128)  out->ram_kb = 128;
            if (out->ram_kb > 4096) out->ram_kb = 4096;
        } else if (strcmp(key, "scale_mode") == 0) {
            if (strcmp(val, "fit")     == 0) out->scale_mode = MAGICMAC_SCALE_FIT;
            else if (strcmp(val, "stretch") == 0) out->scale_mode = MAGICMAC_SCALE_STRETCH;
            else                               out->scale_mode = MAGICMAC_SCALE_NEAREST;
        } else if (strcmp(key, "display_w") == 0) {
            out->display_w = (uint16_t)atoi(val);
        } else if (strcmp(key, "display_h") == 0) {
            out->display_h = (uint16_t)atoi(val);
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "loaded mac.conf: rom=%s ram=%ukb scale=%d disp=%ux%u",
             out->rom_path, out->ram_kb, out->scale_mode,
             out->display_w, out->display_h);
    return true;
}
