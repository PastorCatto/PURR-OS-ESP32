// moy_cart.c — find a cart on the SD card and load it (spec 3).
//
// A cart is a FOLDER, not an archive: manifest.json, main.lua, sprites.moygfx,
// map.moymap, sounds.json, config.json. Nothing is compiled and nothing is
// packed, so a cart is copied onto the card and played.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "moy.h"

static const char *TAG = "moy_cart";

const char *moy_err_str(moy_err_t e)
{
    switch (e) {
    case MOY_OK:            return "ok";
    case MOY_ERR_NO_DIR:    return "No " MOY_CART_DIR " - is the SD card in?";
    case MOY_ERR_NO_CART:   return "No .moy cart folder in " MOY_CART_DIR;
    case MOY_ERR_MANIFEST:  return "Cart has no valid manifest.json";
    case MOY_ERR_FORMAT:    return "Cart needs a newer moy than this one";
    case MOY_ERR_MAIN:      return "Cart's main script is missing";
    case MOY_ERR_MEM:       return "Not enough memory for this cart";
    case MOY_ERR_ASSET:     return "Cart art or map is malformed";
    }
    return "Unknown error";
}

// ── File helpers ────────────────────────────────────────────────────────────

char *moy_cart_read(const char *filename, size_t *out_len)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", g_moy.dir, filename);

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return NULL;

    // PSRAM: a main.lua is tens of KB and a sheet file is 33KB of text. None of
    // it belongs in internal DRAM, which is what speed demon exists to protect.
    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) { heap_caps_free(buf); return NULL; }

    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);

    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ── sprites.moygfx (spec 3.2) ───────────────────────────────────────────────
//
// PICO-8's __gfx__ extended downward: one hex nibble per pixel, 128 characters
// per line, up to 256 lines. Short sheets are legal and leave the rest blank,
// which is why the buffer is calloc'd and parsing simply stops at EOF rather
// than treating a short file as an error.
static bool parse_sheet(void)
{
    size_t len = 0;
    char *txt = moy_cart_read("sprites.moygfx", &len);
    if (!txt) {
        ESP_LOGW(TAG, "no sprites.moygfx - sheet left blank");
        return true;            // legal: a cart may draw only primitives
    }

    g_moy.sheet = heap_caps_calloc(1, MOY_SHEET_W * MOY_SHEET_H, MALLOC_CAP_SPIRAM);
    if (!g_moy.sheet) { heap_caps_free(txt); return false; }

    int row = 0, col = 0;
    for (size_t i = 0; i < len && row < MOY_SHEET_H; i++) {
        char ch = txt[i];
        if (ch == '\n') { row++; col = 0; continue; }
        if (ch == '\r') continue;
        int v = hexval(ch);
        if (v < 0) continue;                    // tolerate stray whitespace
        if (col < MOY_SHEET_W)
            g_moy.sheet[row * MOY_SHEET_W + col] = (uint8_t)v;
        col++;
    }

    heap_caps_free(txt);
    ESP_LOGI(TAG, "sheet: %d rows parsed", row);
    return true;
}

// ── map.moymap (spec 3.3) ───────────────────────────────────────────────────
//
// Header line "w h", then h rows of w*2 hex digits, one byte per cell.
// The file stores tile_id + 1 so that 00 is an empty cell; decoded here to
// 0xFF-means-empty so that tile 0 stays drawable and mget() can report -1.
static bool parse_map(void)
{
    size_t len = 0;
    char *txt = moy_cart_read("map.moymap", &len);
    if (!txt) {
        ESP_LOGW(TAG, "no map.moymap - no tilemap");
        return true;
    }

    int w = 0, h = 0;
    const char *p = txt;
    if (sscanf(p, "%d %d", &w, &h) != 2) {
        ESP_LOGE(TAG, "map header unreadable");
        heap_caps_free(txt);
        return false;
    }
    // Reject rather than clamp: the spec requires a host to refuse an
    // oversized map instead of allocating past its budget (3.3).
    if (w <= 0 || h <= 0 || w > MOY_MAP_MAX || h > MOY_MAP_MAX) {
        ESP_LOGE(TAG, "map %dx%d exceeds the %d cell limit", w, h, MOY_MAP_MAX);
        heap_caps_free(txt);
        return false;
    }

    g_moy.map = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
    if (!g_moy.map) { heap_caps_free(txt); return false; }
    memset(g_moy.map, 0xFF, (size_t)w * h);
    g_moy.map_w = w;
    g_moy.map_h = h;

    while (*p && *p != '\n') p++;               // skip the header line
    if (*p == '\n') p++;

    int cell = 0, hi = -1;
    for (; *p && cell < w * h; p++) {
        int v = hexval(*p);
        if (v < 0) continue;
        if (hi < 0) { hi = v; continue; }
        int byte = (hi << 4) | v;
        hi = -1;
        // 0 = empty; otherwise the stored value is tile_id + 1.
        g_moy.map[cell++] = (byte == 0) ? 0xFF : (uint8_t)(byte - 1);
    }

    heap_caps_free(txt);
    ESP_LOGI(TAG, "map: %dx%d, %d cells", w, h, cell);
    return true;
}

// ── manifest.json (spec 3.1) ────────────────────────────────────────────────

static moy_err_t parse_manifest(void)
{
    char *txt = moy_cart_read("manifest.json", NULL);
    if (!txt) return MOY_ERR_MANIFEST;

    cJSON *root = cJSON_Parse(txt);
    heap_caps_free(txt);
    if (!root) return MOY_ERR_MANIFEST;

    moy_err_t rc = MOY_OK;

    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (!cJSON_IsString(fmt)) { rc = MOY_ERR_MANIFEST; goto out; }
    // "moy-1" is what core 0.1 emits. Refuse anything else rather than trying
    // and failing obscurely later — a cart from a future revision should say so
    // on the splash, not crash in a verb that does not exist yet.
    if (strcmp(fmt->valuestring, "moy-1") != 0) {
        ESP_LOGE(TAG, "manifest format '%s' unsupported", fmt->valuestring);
        rc = MOY_ERR_FORMAT;
        goto out;
    }

    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    snprintf(g_moy.title, sizeof(g_moy.title), "%s",
             cJSON_IsString(title) ? title->valuestring : "moy cart");

    const cJSON *fps = cJSON_GetObjectItemCaseSensitive(root, "fps");
    g_moy.fps = cJSON_IsNumber(fps) ? fps->valueint : 30;
    // spec 5: 30 minimum, 60 optional. Anything else is a manifest mistake;
    // snap it rather than running the cart at a rate it never expected.
    if (g_moy.fps != 30 && g_moy.fps != 60) {
        ESP_LOGW(TAG, "manifest fps=%d is not 30 or 60 - using 30", g_moy.fps);
        g_moy.fps = 30;
    }

    const cJSON *main = cJSON_GetObjectItemCaseSensitive(root, "main");
    const char *mainname = cJSON_IsString(main) ? main->valuestring : "main.lua";
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", g_moy.dir, mainname);
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "main script '%s' not found", mainname);
        rc = MOY_ERR_MAIN;
        goto out;
    }

out:
    cJSON_Delete(root);
    return rc;
}

// ── Load ────────────────────────────────────────────────────────────────────

static bool ends_with_moy(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".moy") == 0;
}

// Every cart on the card, sorted by name so the list is stable between boots —
// readdir order is filesystem order and would otherwise shuffle the menu.
moy_err_t moy_cart_scan(moy_cart_list_t *out)
{
    out->n = 0;

    DIR *dir = opendir(MOY_CART_DIR);
    if (!dir) return MOY_ERR_NO_DIR;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && out->n < MOY_MAX_CARTS) {
        if (!ends_with_moy(ent->d_name)) continue;
        snprintf(out->name[out->n], sizeof(out->name[0]), "%s", ent->d_name);
        out->n++;
    }
    closedir(dir);

    for (int i = 1; i < out->n; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", out->name[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(out->name[j], tmp) > 0) {
            snprintf(out->name[j + 1], sizeof(out->name[0]), "%s", out->name[j]);
            j--;
        }
        snprintf(out->name[j + 1], sizeof(out->name[0]), "%s", tmp);
    }

    ESP_LOGI(TAG, "%d cart(s) on the card", out->n);
    return out->n ? MOY_OK : MOY_ERR_NO_CART;
}

moy_err_t moy_cart_load(const char *folder)
{
    if (!folder || !*folder) return MOY_ERR_NO_CART;

    memset(&g_moy, 0, sizeof(g_moy));
    moy_draw_reset();

    snprintf(g_moy.dir, sizeof(g_moy.dir), "%s/%s", MOY_CART_DIR, folder);

    moy_err_t rc = parse_manifest();
    if (rc != MOY_OK) return rc;

    if (!parse_sheet()) { moy_cart_free(); return MOY_ERR_MEM; }
    if (!parse_map())   { moy_cart_free(); return MOY_ERR_ASSET; }

    // config.json is optional and is handed to cfg() verbatim (spec 9).
    g_moy.config_json = moy_cart_read("config.json", NULL);

    ESP_LOGI(TAG, "loaded '%s' (%d fps) from %s", g_moy.title, g_moy.fps, g_moy.dir);
    return MOY_OK;
}

void moy_cart_free(void)
{
    if (g_moy.sheet)       { heap_caps_free(g_moy.sheet);       g_moy.sheet = NULL; }
    if (g_moy.map)         { heap_caps_free(g_moy.map);         g_moy.map = NULL; }
    if (g_moy.config_json) { heap_caps_free(g_moy.config_json); g_moy.config_json = NULL; }
}
