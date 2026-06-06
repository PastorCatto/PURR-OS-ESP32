#include "device_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "device_config";

static void apply_defaults(device_config_t* c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->device,      "jc3248w535", sizeof(c->device));
    strlcpy(c->display,     "axs15231b",  sizeof(c->display));
    strlcpy(c->touch,       "axs15231b",  sizeof(c->touch));
    strlcpy(c->lora_region, "US",         sizeof(c->lora_region));
    c->boot_splash[0]  = '\0';
    c->keymap[0]       = '\0';
    c->display_w       = 320;
    c->display_h       = 480;
    c->psram           = true;
    c->pi_slot         = false;
    c->verbose_boot    = true;
    c->cpu_max_mhz     = 240;
    c->friends_ram_threshold_kb = 64;
    c->flash_mb        = 16;
    c->ram_kb          = (uint16_t)(esp_get_free_heap_size() / 1024);
    c->psram_mb        = 8;
    c->wifi            = true;
    c->bt              = true;
    c->lora            = false;
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16384) { fclose(f); return NULL; }
    char* buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

static const char* json_str(const cJSON* root, const char* key, const char* def) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : def;
}
static bool json_bool(const cJSON* root, const char* key, bool def) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsBool(it) ? cJSON_IsTrue(it) : def;
}
static int json_int(const cJSON* root, const char* key, int def) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

bool device_config_load(const char* path, device_config_t* out) {
    if (!out) return false;
    apply_defaults(out);

    char* text = read_file(path);
    if (!text) {
        ESP_LOGW(TAG, "%s not found — using compiled defaults", path);
        return true;
    }

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGW(TAG, "device.json parse error — using defaults");
        return true;
    }

    strlcpy(out->device,      json_str(root, "device",      out->device),      sizeof(out->device));
    strlcpy(out->display,     json_str(root, "display",     out->display),     sizeof(out->display));
    strlcpy(out->touch,       json_str(root, "touch",       out->touch),       sizeof(out->touch));
    strlcpy(out->lora_region, json_str(root, "lora_region", out->lora_region), sizeof(out->lora_region));
    strlcpy(out->boot_splash, json_str(root, "boot_splash", out->boot_splash), sizeof(out->boot_splash));
    strlcpy(out->keymap,      json_str(root, "keymap",      out->keymap),      sizeof(out->keymap));

    const cJSON* res = cJSON_GetObjectItemCaseSensitive(root, "display_res");
    if (cJSON_IsArray(res) && cJSON_GetArraySize(res) >= 2) {
        out->display_w = (uint16_t)cJSON_GetArrayItem(res, 0)->valueint;
        out->display_h = (uint16_t)cJSON_GetArrayItem(res, 1)->valueint;
    }

    out->psram        = json_bool(root, "psram",   out->psram);
    out->pi_slot      = json_bool(root, "pi_slot", out->pi_slot);
    out->verbose_boot = json_bool(root, "verbose", out->verbose_boot);
    out->cpu_max_mhz  = json_int(root,  "cpu_max_mhz", out->cpu_max_mhz);
    out->friends_ram_threshold_kb =
        (uint16_t)json_int(root, "friends_ram_threshold_kb", out->friends_ram_threshold_kb);

    const char* flash_str = json_str(root, "flash", "16mb");
    int fmb = atoi(flash_str);
    if (fmb > 0) out->flash_mb = (uint8_t)fmb;

    // radios array
    out->wifi = out->bt = out->lora = false;
    const cJSON* radios = cJSON_GetObjectItemCaseSensitive(root, "radios");
    if (cJSON_IsArray(radios)) {
        const cJSON* r;
        cJSON_ArrayForEach(r, radios) {
            if (!cJSON_IsString(r)) continue;
            if (strcmp(r->valuestring, "wifi") == 0) out->wifi = true;
            if (strcmp(r->valuestring, "bt")   == 0) out->bt   = true;
            if (strcmp(r->valuestring, "lora") == 0) out->lora = true;
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded: device=%s display=%s %dx%d psram=%d wifi=%d",
             out->device, out->display, out->display_w, out->display_h, out->psram, out->wifi);
    return true;
}
