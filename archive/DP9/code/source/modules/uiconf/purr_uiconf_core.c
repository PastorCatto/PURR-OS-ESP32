// purr_uiconf_core.c — see purr_uiconf_core.h for the full picture.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "purr_uiconf_core.h"
#include "../../kernel/core/purr_kernel.h"
#include "../user_mgr/user_mgr.h"
// app_manager.h intentionally NOT included yet — list `source = "app_list"`
// resolution (app_manager_count()/_entry_name()) is renderer-side, added
// once the first vertical slice's list-widget support lands; this core
// file only resolves scalar `bind` values so far.

static const char *TAG = "uiconf";

// ── Fixed vocabulary tables ──────────────────────────────────────────
// MUST stay index-identical to catstrap/uiconf.py's own CONDITIONS/
// BINDINGS/EVENTS lists — see that file's top comment for the reverse
// pointer back here. Both sides are hand-maintained in lockstep,
// same discipline UI_BACKEND_MAP already carries elsewhere.
static const char *s_conditions[] = {
    "wifi_available", "lora_available", "is_admin", "sd_available", "flash_available",
};
#define CONDITION_COUNT (int)(sizeof(s_conditions) / sizeof(s_conditions[0]))

static const char *s_events[] = { "select", "back", "activate", "change" };
#define EVENT_COUNT (int)(sizeof(s_events) / sizeof(s_events[0]))

// ── File loading ─────────────────────────────────────────────────────
// Real, hardware-found bug (2026-09-13, T-Deck Plus): a plain fopen()/
// fread() against /flash (SPIFFS, backed by the same internal SPI flash
// chip as code/NVS) hits `assert(esp_task_stack_is_sane_cache_disabled())`
// in ESP-IDF's own spi_flash_disable_interrupts_caches_and_other_cpu()
// (cache_utils.c) — EVERY raw flash op on this chip family briefly
// disables the instruction/data cache, which requires the CALLING task's
// own stack to be entirely in internal DRAM at that moment. app_manager.c
// deliberately launches native app tasks on a PSRAM-backed stack
// (xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM) — its own
// established, resource-conscious default for "most apps"), so a claw
// app's own task calling purr_uiconf_open() straight into this crashes
// immediately on ANY device that hasn't happened to stage the screen onto
// SD (SD I/O uses a completely different peripheral, no such constraint —
// which is exactly why this bug hid until a screen was staged flash-only,
// as diagnostics' own was). Same root cause miniwin_module.c's own
// miniwin_task() comment already documents for mw_settings_save() — that
// fix was "give MiniWin's own task a plain internal-DRAM stack"; the
// equivalent fix here, since this core has no task of its own and must
// stay safe for ANY caller regardless of that caller's own stack policy,
// is to hop the actual file I/O onto a short-lived helper task that
// guarantees one (plain xTaskCreate(), no MALLOC_CAP_SPIRAM) — applied to
// BOTH the SD and flash paths uniformly rather than only the flash one,
// so this stays one single code path no future change can accidentally
// un-safe by re-ordering the SD/flash fallback.
typedef struct {
    const char *path;
    uint8_t    *data;   // out: malloc'd buffer, caller frees
    long        size;   // out
    bool        ok;     // out
    SemaphoreHandle_t done;
} safe_read_ctx_t;

static void safe_read_task(void *arg)
{
    safe_read_ctx_t *ctx = (safe_read_ctx_t *)arg;
    FILE *f = fopen(ctx->path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            uint8_t *buf = malloc((size_t)size);
            if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
                ctx->data = buf;
                ctx->size = size;
                ctx->ok = true;
            } else if (buf) {
                free(buf);
            }
        }
        fclose(f);
    }
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

// Reads a whole file from a task with a guaranteed internal-DRAM stack,
// regardless of what kind of stack the CALLING task itself has. See this
// section's own top comment for why that matters on this chip family.
static bool safe_read_file(const char *path, uint8_t **out_data, long *out_size)
{
    safe_read_ctx_t ctx = { .path = path, .data = NULL, .size = 0, .ok = false, .done = NULL };
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) return false;
    // Plain xTaskCreate() — internal-DRAM stack, the same deliberate
    // choice miniwin_module.c's own miniwin_task() already documents for
    // the identical reason. 3KB comfortably covers fopen/fread's own
    // stack use; this task does nothing else.
    BaseType_t ok = xTaskCreate(safe_read_task, "uiconf_fread", 3072, &ctx, 3, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(ctx.done);
        return false;
    }
    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);
    *out_data = ctx.data;
    *out_size = ctx.size;
    return ctx.ok;
}

bool purr_uiconf_load_screen(const char *app_name, const char *screen_name, purr_uiconf_screen_t *out)
{
    memset(out, 0, sizeof(*out));

    char path[128];
    // /sdcard is an override, /flash is the shipped default — same
    // precedence claw_loader.c's own system_root() already established
    // for sysclaw packages.
    uint8_t *data = NULL;
    long size = 0;
    bool found = false;
    if (purr_kernel_sd_available()) {
        snprintf(path, sizeof(path), "/sdcard/ui/%s/%s.puib", app_name, screen_name);
        found = safe_read_file(path, &data, &size);
    }
    if (!found) {
        snprintf(path, sizeof(path), "/flash/ui/%s/%s.puib", app_name, screen_name);
        found = safe_read_file(path, &data, &size);
    }
    if (!found) {
        ESP_LOGW(TAG, "screen not found: %s/%s.puib", app_name, screen_name);
        return false;
    }

    if (size < (long)sizeof(pui_header_t)) { free(data); return false; }

    const pui_header_t *hdr = (const pui_header_t *)data;
    if (memcmp(hdr->magic, PUI_MAGIC, 4) != 0 || hdr->version != PUI_FORMAT_VERSION) {
        free(data);
        return false;
    }
    size_t expect = sizeof(pui_header_t) + (size_t)hdr->node_count * sizeof(pui_node_t) + hdr->pool_size;
    if ((size_t)size < expect) { free(data); return false; }

    out->data   = data;
    out->size   = (size_t)size;
    out->header = hdr;
    out->nodes  = (const pui_node_t *)(data + sizeof(pui_header_t));
    out->pool   = data + sizeof(pui_header_t) + (size_t)hdr->node_count * sizeof(pui_node_t);
    return true;
}

void purr_uiconf_free_screen(purr_uiconf_screen_t *s)
{
    if (s->data) free(s->data);
    memset(s, 0, sizeof(*s));
}

// ── Node tree access ─────────────────────────────────────────────────

pui_kind_t purr_uiconf_kind(const purr_uiconf_screen_t *s, int node)
{
    if (node < 0 || node >= s->header->node_count) return (pui_kind_t)-1;
    return (pui_kind_t)s->nodes[node].kind;
}

int purr_uiconf_first_child(const purr_uiconf_screen_t *s, int node)
{
    if (node < 0 || node >= s->header->node_count) return PUI_NONE;
    return s->nodes[node].first_child;
}

int purr_uiconf_next_sibling(const purr_uiconf_screen_t *s, int node)
{
    if (node < 0 || node >= s->header->node_count) return PUI_NONE;
    return s->nodes[node].next_sibling;
}

// One attr entry, parsed from the pool at `off`. Layout (see uiconf.py's
// own _encode_attrs()): key_off(u32) kind(u8) [value...]. Advances `off`
// past this entry, returns false if `off` runs past the pool (shouldn't
// happen post-validation, but bounds-checked regardless — this reads a
// file, not trusted memory).
typedef struct {
    const char *key;
    pui_attr_kind_t kind;
    uint32_t str_off;     // kind == STRING
    float    num;         // kind == NUMBER
    bool     boolean;     // kind == BOOL
    // kind == CONDLIST:
    uint8_t  cond_ids[8];
    bool     cond_neg[8];
    int      cond_count;
} pui_attr_entry_t;

static bool read_attr_entry(const purr_uiconf_screen_t *s, uint32_t *off, pui_attr_entry_t *out)
{
    uint32_t o = *off;
    if (o + 5 > s->header->pool_size) return false;
    uint32_t key_off;
    memcpy(&key_off, s->pool + o, 4);
    uint8_t kind = s->pool[o + 4];
    o += 5;
    out->key = (const char *)(s->pool + key_off);
    out->kind = (pui_attr_kind_t)kind;
    switch (kind) {
        case PUI_ATTR_STRING: {
            uint32_t val_off;
            if (o + 4 > s->header->pool_size) return false;
            memcpy(&val_off, s->pool + o, 4);
            out->str_off = val_off;
            o += 4;
            break;
        }
        case PUI_ATTR_NUMBER: {
            if (o + 4 > s->header->pool_size) return false;
            memcpy(&out->num, s->pool + o, 4);
            o += 4;
            break;
        }
        case PUI_ATTR_BOOL: {
            if (o + 1 > s->header->pool_size) return false;
            out->boolean = s->pool[o] != 0;
            o += 1;
            break;
        }
        case PUI_ATTR_CONDLIST: {
            if (o + 1 > s->header->pool_size) return false;
            uint8_t n = s->pool[o];
            o += 1;
            if (n > 8 || o + (uint32_t)n * 2 > s->header->pool_size) return false;
            out->cond_count = n;
            for (int i = 0; i < n; i++) {
                out->cond_ids[i] = s->pool[o];
                out->cond_neg[i] = s->pool[o + 1] != 0;
                o += 2;
            }
            break;
        }
        default:
            return false;
    }
    *off = o;
    return true;
}

static bool find_attr(const purr_uiconf_screen_t *s, int node, const char *key, pui_attr_entry_t *out)
{
    if (node < 0 || node >= s->header->node_count) return false;
    uint32_t off = s->nodes[node].attr_off;
    uint16_t count = s->nodes[node].attr_count;
    for (uint16_t i = 0; i < count; i++) {
        pui_attr_entry_t e;
        if (!read_attr_entry(s, &off, &e)) return false;
        if (strcmp(e.key, key) == 0) {
            *out = e;
            return true;
        }
    }
    return false;
}

bool purr_uiconf_attr_str(const purr_uiconf_screen_t *s, int node, const char *key,
                           char *out_str, size_t out_sz)
{
    pui_attr_entry_t e;
    if (!find_attr(s, node, key, &e) || e.kind != PUI_ATTR_STRING) return false;
    snprintf(out_str, out_sz, "%s", (const char *)(s->pool + e.str_off));
    return true;
}

bool purr_uiconf_attr_num(const purr_uiconf_screen_t *s, int node, const char *key, float *out)
{
    pui_attr_entry_t e;
    if (!find_attr(s, node, key, &e) || e.kind != PUI_ATTR_NUMBER) return false;
    *out = e.num;
    return true;
}

bool purr_uiconf_attr_bool(const purr_uiconf_screen_t *s, int node, const char *key, bool *out)
{
    pui_attr_entry_t e;
    if (!find_attr(s, node, key, &e) || e.kind != PUI_ATTR_BOOL) return false;
    *out = e.boolean;
    return true;
}

// ── Conditions ───────────────────────────────────────────────────────
// Each condition name resolves to a real, already-existing capability
// check — see the plan's own "Kernel-side additions" section for the
// one real gap this pass fills (purr_kernel_wifi_available()).
static bool eval_condition(int cond_id)
{
    if (cond_id < 0 || cond_id >= CONDITION_COUNT) return false;
    const char *name = s_conditions[cond_id];
    if (strcmp(name, "wifi_available") == 0) return purr_kernel_wifi_available();
    if (strcmp(name, "lora_available") == 0) return purr_kernel_lora_available();
    if (strcmp(name, "is_admin") == 0)       return user_mgr_is_admin(user_mgr_current_user());
    if (strcmp(name, "sd_available") == 0)   return purr_kernel_sd_available();
    if (strcmp(name, "flash_available") == 0) return purr_kernel_flash_available();
    return false;   // unknown at runtime should never happen post-validation
}

static bool check_condlist_attr(const purr_uiconf_screen_t *s, int node, const char *key)
{
    pui_attr_entry_t e;
    if (!find_attr(s, node, key, &e)) return true;   // absent == always visible/enabled
    if (e.kind != PUI_ATTR_CONDLIST) return true;
    for (int i = 0; i < e.cond_count; i++) {
        bool v = eval_condition(e.cond_ids[i]);
        if (e.cond_neg[i]) v = !v;
        if (!v) return false;   // comma-separated == AND
    }
    return true;
}

bool purr_uiconf_visible(const purr_uiconf_screen_t *s, int node)
{
    return check_condlist_attr(s, node, "visible_if");
}

bool purr_uiconf_enabled(const purr_uiconf_screen_t *s, int node)
{
    return check_condlist_attr(s, node, "enabled_if");
}

// ── Bindings ─────────────────────────────────────────────────────────
// Deliberately a small, fixed printf-style subset, not a real printf —
// each binding's own value type is known ahead of time (int, or string),
// so this only ever needs %d / %s / a literal-percent passthrough. A
// full vprintf on an attacker- or config-authored format string is a
// real, well-known footgun this sidesteps entirely by construction.
static void format_int(const char *format, int value, char *out, size_t out_sz)
{
    if (!format) { snprintf(out, out_sz, "%d", value); return; }
    // Replace the first "%d" with the value; anything else in `format`
    // (including a literal "%%") is copied through verbatim.
    const char *pct = strstr(format, "%d");
    if (!pct) { snprintf(out, out_sz, "%s", format); return; }
    char prefix[32], suffix[32];
    size_t pre_len = (size_t)(pct - format);
    if (pre_len >= sizeof(prefix)) pre_len = sizeof(prefix) - 1;
    memcpy(prefix, format, pre_len); prefix[pre_len] = 0;
    snprintf(suffix, sizeof(suffix), "%s", pct + 2);
    snprintf(out, out_sz, "%s%d%s", prefix, value, suffix);
}

static void format_str(const char *format, const char *value, char *out, size_t out_sz)
{
    if (!format) { snprintf(out, out_sz, "%s", value); return; }
    const char *pct = strstr(format, "%s");
    if (!pct) { snprintf(out, out_sz, "%s", format); return; }
    char prefix[32], suffix[32];
    size_t pre_len = (size_t)(pct - format);
    if (pre_len >= sizeof(prefix)) pre_len = sizeof(prefix) - 1;
    memcpy(prefix, format, pre_len); prefix[pre_len] = 0;
    snprintf(suffix, sizeof(suffix), "%s", pct + 2);
    snprintf(out, out_sz, "%s%s%s", prefix, value, suffix);
}

bool purr_uiconf_resolve_bind(const char *bind_name, const char *format, char *out, size_t out_sz)
{
    if (strcmp(bind_name, "clock_hhmm") == 0) {
        char buf[8];
        purr_kernel_time_hhmm(buf, sizeof(buf));
        format_str(format, buf, out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "battery_percent") == 0) {
        int pct = purr_kernel_battery_percent();
        if (pct < 0) { format_str(format, "?", out, out_sz); return true; }
        format_int(format, pct, out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "battery_voltage_mv") == 0) {
        format_int(format, purr_kernel_battery_voltage_mv(), out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "free_ram_kb") == 0) {
        format_int(format, (int)(purr_kernel_free_ram() / 1024), out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "uptime_ms") == 0) {
        format_int(format, (int)purr_kernel_uptime_ms(), out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "wifi_connected") == 0) {
        format_str(format, purr_kernel_wifi_connected() ? "yes" : "no", out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "version") == 0) {
        format_str(format, PURR_KERNEL_VERSION, out, out_sz);
        return true;
    }
    if (strcmp(bind_name, "current_user") == 0) {
        format_str(format, user_mgr_current_user(), out, out_sz);
        return true;
    }
    return false;
}

// ── Navigation ───────────────────────────────────────────────────────

bool purr_uiconf_open(purr_uiconf_state_t *state, const char *app_name, const char *screen_name)
{
    purr_uiconf_free_screen(&state->screen);
    state->selected_index = 0;
    return purr_uiconf_load_screen(app_name, screen_name, &state->screen);
}

void purr_uiconf_close(purr_uiconf_state_t *state)
{
    purr_uiconf_free_screen(&state->screen);
    state->selected_index = 0;
}

bool purr_uiconf_find_handler(const purr_uiconf_screen_t *s, int node, const char *event_name,
                               char *out_action, size_t action_sz,
                               char *out_target, size_t target_sz)
{
    int eid = -1;
    for (int i = 0; i < EVENT_COUNT; i++) {
        if (strcmp(s_events[i], event_name) == 0) { eid = i; break; }
    }
    if (eid < 0) return false;

    for (int child = purr_uiconf_first_child(s, node); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        if (purr_uiconf_kind(s, child) != PUI_KIND_ON_HANDLER) continue;
        float ev = -1;
        if (!purr_uiconf_attr_num(s, child, "_event", &ev) || (int)ev != eid) continue;
        if (out_action) purr_uiconf_attr_str(s, child, "action", out_action, action_sz);
        if (out_target) purr_uiconf_attr_str(s, child, "target", out_target, target_sz);
        return true;
    }
    return false;
}
