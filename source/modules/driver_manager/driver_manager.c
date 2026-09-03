// driver_manager.c — PURR OS driver manager

#include "driver_manager.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — see s_drivers's own comment below
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>

static const char *TAG = "drv_mgr";

#define MAX_DRIVERS 32

// Paths scanned in order. First match per driver name wins.
static const char *s_scan_paths[] = {
    "/flash/drivers",
    "/sdcard/drivers",
    NULL
};

// EXT_RAM_BSS_ATTR (PSRAM, not internal DRAM) — a passive registry, scanned
// late in boot (Phase 2 "SD extras", well after PSRAM init — see
// kernel_tdp_boot.c), not touched by anything earlier. Same class MiniWin's
// own control/list arrays already use this for — real, confirmed via
// purr_os.map: a genuine link-time DRAM overflow once the current app set
// was all compiled in together (see msn.c's matching comment, 2026-09-01).
static EXT_RAM_BSS_ATTR drv_entry_t s_drivers[MAX_DRIVERS];
static int         s_driver_count = 0;

// ── Compat check ──────────────────────────────────────────────────────────────
// For now: required_catcalls is a bitmask. Check each flag against what the
// kernel has registered. Returns true if all required catcalls are present.

static bool compat_check(uint32_t required_catcalls, char *fail_reason, size_t reason_sz) {
    if ((required_catcalls & CATCALL_FLAG_DISPLAY) && !purr_kernel_display()) {
        snprintf(fail_reason, reason_sz, "missing: catcall_display");
        return false;
    }
    if ((required_catcalls & CATCALL_FLAG_TOUCH) && !purr_kernel_touch()) {
        snprintf(fail_reason, reason_sz, "missing: catcall_touch");
        return false;
    }
    if ((required_catcalls & CATCALL_FLAG_INPUT) && !purr_kernel_input()) {
        snprintf(fail_reason, reason_sz, "missing: catcall_input");
        return false;
    }
    if ((required_catcalls & CATCALL_FLAG_RADIO) && !purr_kernel_radio()) {
        snprintf(fail_reason, reason_sz, "missing: catcall_radio");
        return false;
    }
    if ((required_catcalls & CATCALL_FLAG_GPS) && !purr_kernel_gps()) {
        snprintf(fail_reason, reason_sz, "missing: catcall_gps");
        return false;
    }
    return true;
}

// ── Version helpers ───────────────────────────────────────────────────────────

static int version_cmp(const char *a, const char *b) {
    unsigned ma=0,mi=0,pa=0, mb=0,mib=0,pb=0;
    sscanf(a, "%u.%u.%u", &ma, &mi, &pa);
    sscanf(b, "%u.%u.%u", &mb, &mib, &pb);
    if (ma != mb) return ma < mb ? -1 : 1;
    if (mi != mib) return mi < mib ? -1 : 1;
    if (pa != pb) return pa < pb ? -1 : 1;
    return 0;
}

// ── Load one driver .purr ─────────────────────────────────────────────────────

// Returns the s_drivers[] index already holding this name, or -1. Closes
// the gap where the "First match per driver name wins" comment above used
// to be aspirational only — every .purr found in both /flash/drivers and
// /sdcard/drivers was previously loaded (and init()'d) unconditionally.
static int find_driver_slot_by_name(const char *name) {
    for (int i = 0; i < s_driver_count; i++) {
        if (strncmp(s_drivers[i].name, name, sizeof(s_drivers[i].name)) == 0) {
            return i;
        }
    }
    return -1;
}

static void load_driver(const char *path) {
    purr_module_header_t hdr;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < sizeof(hdr) || hdr.magic != PURR_MODULE_MAGIC) return;

    // Signature classification — see sig_mgr.h for the tier model. Every
    // driver in this tree is unsigned today and that is fine: this does
    // NOT gate on tier, only on TAMPERED — a .sig + co-located .pub that
    // fails to verify against this file's current content, meaning it
    // changed after someone signed it. That is refused unconditionally,
    // for a /flash/drivers entry as much as an /sdcard/drivers one — a
    // tampered driver blob is worth refusing to init() regardless of
    // where it came from. An unsigned driver keeps loading exactly as it
    // always has; this only adds a hard stop for the one signal that
    // means something is actively wrong, plus makes the trust tier
    // visible via drv_entry_t.sig_tier for whatever reads it later.
    sig_tier_t sig = sig_mgr_classify(path);
    if (sig == SIG_TIER_TAMPERED) {
        ESP_LOGE(TAG, "REFUSING driver at %s: signature present but INVALID (tampered)", path);
        return;
    }

    int existing = find_driver_slot_by_name(hdr.name);
    if (existing >= 0 && version_cmp(hdr.version, s_drivers[existing].version) <= 0) {
        ESP_LOGI(TAG, "driver '%s' v%s already loaded (have v%s) — skipping %s",
                 hdr.name, hdr.version, s_drivers[existing].version, path);
        return;
    }

    drv_entry_t *ent;
    if (existing >= 0) {
        // Catcall registration is last-registered-wins at the kernel level
        // (see purr_kernel.c), so calling this driver's init() below will
        // correctly take over the catcall slot. Its drv_entry_t status
        // entry is reused in place rather than appended as a duplicate.
        ESP_LOGI(TAG, "driver '%s': v%s supersedes loaded v%s from %s",
                 hdr.name, hdr.version, s_drivers[existing].version, path);
        ent = &s_drivers[existing];
    } else {
        if (s_driver_count >= MAX_DRIVERS) return;
        ent = &s_drivers[s_driver_count++];
    }

    strncpy(ent->name, hdr.name, sizeof(ent->name) - 1);
    strncpy(ent->version, hdr.version, sizeof(ent->version) - 1);
    ent->status = DRV_STATUS_OK;
    ent->fail_reason[0] = '\0';
    ent->sig_tier = sig;

    // kernel_min check
    if (hdr.kernel_min[0] && version_cmp(KITT_VERSION, hdr.kernel_min) < 0) {
        ent->status = DRV_STATUS_SKIP;
        snprintf(ent->fail_reason, sizeof(ent->fail_reason),
                 "needs kernel >= %s", hdr.kernel_min);
        ESP_LOGW(TAG, "skip %s: %s", hdr.name, ent->fail_reason);
        return;
    }

    // kernel_max check
    bool beyond_max = hdr.kernel_max[0] && version_cmp(KITT_VERSION, hdr.kernel_max) > 0;
    if (beyond_max) {
        // Compat check — all required catcalls must be present
        if (!compat_check(hdr.required_catcalls, ent->fail_reason, sizeof(ent->fail_reason))) {
            ent->status = DRV_STATUS_FAIL;
            ESP_LOGE(TAG, "FAIL %s: %s", hdr.name, ent->fail_reason);
            return;
        }
        ent->status = DRV_STATUS_COMPAT;
    }

    // Call init
    if (hdr.init && hdr.init() != 0) {
        ent->status = DRV_STATUS_FAIL;
        snprintf(ent->fail_reason, sizeof(ent->fail_reason), "init() failed");
        ESP_LOGE(TAG, "FAIL %s: init returned error", hdr.name);
        return;
    }

    ESP_LOGI(TAG, "%s %s v%s",
             drv_status_badge(ent->status), hdr.name, hdr.version);
}

// ── Scan a directory for driver .purr files ───────────────────────────────────

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcmp(ext, ".purr") != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        load_driver(path);
    }
    closedir(d);
}

// ── Public API ────────────────────────────────────────────────────────────────

int driver_manager_init(void) {
    s_driver_count = 0;
    for (int i = 0; s_scan_paths[i]; i++) {
        scan_dir(s_scan_paths[i]);
    }
    ESP_LOGI(TAG, "init complete: %d drivers registered", s_driver_count);
    return 0;
}

void driver_manager_deinit(void) {
    s_driver_count = 0;
}

int driver_manager_get_count(void) {
    return s_driver_count;
}

const drv_entry_t *driver_manager_get_entry(int idx) {
    if (idx < 0 || idx >= s_driver_count) return NULL;
    return &s_drivers[idx];
}

const char *drv_status_badge(drv_status_t s) {
    switch (s) {
    case DRV_STATUS_OK:     return "[OK]";
    case DRV_STATUS_COMPAT: return "[COMPAT]";
    case DRV_STATUS_FAIL:   return "[FAIL]";
    case DRV_STATUS_SKIP:   return "[SKIP]";
    default:                return "[?]";
    }
}

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(driver_manager) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    // Explicit — unset (0) sorted this ahead of P1 drivers (see miniwin's
    // matching comment for the failure that exposed it).
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "driver_manager",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = driver_manager_init,
    .deinit            = driver_manager_deinit,
};
