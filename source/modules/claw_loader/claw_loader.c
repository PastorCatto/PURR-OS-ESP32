// claw_loader.c — see claw_loader.h for the full picture.
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "purr_module.h"
#include "purr_kernel.h"
#include "claw_elf.h"
#include "claw_loader.h"
#include "app_manager.h"   // app_manager_mark_scan_dirty() — see personal_add/remove's own call sites

static const char *TAG = "claw_loader";

// The import table — see claw_elf.h's own header comment on
// CLAW_SEC_EXTERN/claw_import_t: this list IS the capability boundary for
// loaded code. A module can call a named host function only if it's listed
// here; anything else in the firmware is unreachable by name to it,
// deliberately.
//
// Generated (by purrstrap's _generate_claw_imports(), see purrstrap.py)
// from purr_kernel.h's own public function surface, not hand-maintained —
// used to be a single hand-written entry here, which was a generation gap
// against an ABI that was already fully designed, not a real ceiling on
// what a loaded module should be able to reach. Regenerated on every
// `purrstrap build`/`purrstrap generate`; do not hand-edit the generated
// file. s_imports[]/CLAW_IMPORT_COUNT come from it.
#include "claw_imports_generated.h"

// "claw_slot" — promoted from the R&D spike's "claw_poc" partition once
// three real-hardware rounds confirmed the approach (see
// partitions_16mb_ota.csv's own comment on this partition for the sizing
// story). Same name change reasoning as everywhere else in this codebase
// that graduates scratch work: the partition's JOB didn't change, only
// whether it's still "may be abandoned" scratch.
//
// "sys_claw" — the second, bigger pool added alongside it (see
// partitions_16mb_ota.csv's own comment on THIS partition): core/system
// packages need real room for a UI screen, not a 32KB personal-app slot.
#define CLAW_SLOT_PARTITION_NAME "claw_slot"
#define CLAW_SLOT_SUBTYPE        0x40
#define SYS_CLAW_PARTITION_NAME  "sys_claw"
#define SYS_CLAW_SUBTYPE         0x41

// ── Slot table ───────────────────────────────────────────────────────────
//
// Each pool's partition (see claw_pool_t's own comment in claw_loader.h for
// the two pools' sizes) is divided into CLAW_MAX_SLOTS equal, independently
// mmap'd/erased/written sub-regions, instead of one module owning the whole
// partition at offset 0 the way this file used to. CLAW_MAX_SLOTS is
// deliberately small — 2, not more — for two reasons: both partitions are
// already sized against real-hardware needs (claw_slot: nothing loaded
// there has ever needed more than a couple KB; sys_claw: loginUI's own
// size budget), and both divide evenly by the flash erase-sector size with
// no rounding, which a larger N either shrinks further into or, worse,
// stops dividing evenly, needing per-slot erase-length rounding logic to
// get right instead of falling out of the arithmetic for free. Growing
// this later means either accepting smaller slots or growing the relevant
// partition itself — a partitions_16mb_ota.csv change, deliberately NOT
// made in this pass beyond adding sys_claw itself.
//
// One s_slot_used[] array PER POOL — a system-package load must never
// contend with (or accidentally free) a personal app's slot, and vice
// versa, so these are kept completely independent rather than sharing one
// array indexed some other way.
static bool s_slot_used_dynamic[CLAW_MAX_SLOTS];
static bool s_slot_used_system[CLAW_MAX_SLOTS];

static bool *slot_table_for(claw_pool_t pool)
{
    return (pool == CLAW_POOL_SYSTEM) ? s_slot_used_system : s_slot_used_dynamic;
}

static const char *partition_name_for(claw_pool_t pool)
{
    return (pool == CLAW_POOL_SYSTEM) ? SYS_CLAW_PARTITION_NAME : CLAW_SLOT_PARTITION_NAME;
}

static uint8_t subtype_for(claw_pool_t pool)
{
    return (pool == CLAW_POOL_SYSTEM) ? SYS_CLAW_SUBTYPE : CLAW_SLOT_SUBTYPE;
}

static int find_free_slot(claw_pool_t pool)
{
    bool *used = slot_table_for(pool);
    for (int i = 0; i < CLAW_MAX_SLOTS; i++) {
        if (!used[i]) return i;
    }
    return -1;
}

int claw_loader_slots_free(claw_pool_t pool)
{
    bool *used = slot_table_for(pool);
    int n = 0;
    for (int i = 0; i < CLAW_MAX_SLOTS; i++) {
        if (!used[i]) n++;
    }
    return n;
}

bool claw_loader_load(const uint8_t *obj_bytes, size_t obj_len, claw_pool_t pool, claw_loaded_module_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pool = pool;
    out->slot = -1;

    claw_module_t m;
    if (!claw_elf_load(obj_bytes, obj_len, "claw_personal_init",
                        s_imports, CLAW_IMPORT_COUNT, &m)) {
        ESP_LOGE(TAG, "ELF parse failed (claw_personal_init not found or object malformed)");
        return false;
    }
    uint32_t deinit_off = 0;
    bool have_deinit = claw_elf_find_offset(obj_bytes, obj_len, "claw_personal_deinit", &deinit_off);
    if (!have_deinit) {
        ESP_LOGE(TAG, "claw_personal_deinit not found — every loaded module needs both entry points");
        claw_elf_free(&m);
        return false;
    }

    const char *part_name = partition_name_for(pool);
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype_for(pool), part_name);
    if (!part) {
        ESP_LOGE(TAG, "%s partition not found — check partitions_16mb_ota.csv", part_name);
        claw_elf_free(&m);
        return false;
    }

    int slot = find_free_slot(pool);
    if (slot < 0) {
        ESP_LOGE(TAG, "no free slot in %s (%d of %d already loaded)", part_name, CLAW_MAX_SLOTS, CLAW_MAX_SLOTS);
        claw_elf_free(&m);
        return false;
    }
    uint32_t slot_size   = (uint32_t)(part->size / CLAW_MAX_SLOTS);
    uint32_t slot_offset = (uint32_t)slot * slot_size;

    if (m.text_size > slot_size) {
        ESP_LOGE(TAG, "text (%u B) too big for one %s slot (%u B of %u total / %d slots)",
                 (unsigned)m.text_size, part_name,
                 (unsigned)slot_size, (unsigned)part->size, CLAW_MAX_SLOTS);
        claw_elf_free(&m);
        return false;
    }
    // slot_table_for(pool)[] is claimed here, before any further step that
    // can still fail below — every failure path from here on is the shared
    // `fail:` label, which releases it again. Claiming late (only on
    // success) would let two concurrent loads both pick the same "free"
    // slot in between.
    slot_table_for(pool)[slot] = true;

    if (m.rodata_size) {
        out->rodata_ram = heap_caps_malloc(m.rodata_size, MALLOC_CAP_8BIT);
        if (!out->rodata_ram) { ESP_LOGE(TAG, "rodata alloc failed"); goto fail; }
        memcpy(out->rodata_ram, m.rodata, m.rodata_size);
    }
    if (m.data_size) {
        out->data_ram = heap_caps_malloc(m.data_size, MALLOC_CAP_8BIT);
        if (!out->data_ram) { ESP_LOGE(TAG, "data alloc failed"); goto fail; }
        memcpy(out->data_ram, m.data, m.data_size);
    }
    if (m.bss_size) {
        out->bss_ram = heap_caps_malloc(m.bss_size, MALLOC_CAP_8BIT);
        if (!out->bss_ram) { ESP_LOGE(TAG, "bss alloc failed"); goto fail; }
        memset(out->bss_ram, 0, m.bss_size);
    }

    {
        // Probe-mmap just to learn the flash-mapped base address — content
        // doesn't matter yet. Flash bits can only go 1->0 without a fresh
        // erase, so patching in place after one write would silently do
        // nothing — this base is only used to COMPUTE the patched buffer
        // below, written for real afterward. (Confirmed live during the
        // R&D spike this was promoted from — see this module's git
        // history — that a second probe/final mmap pair for the same
        // range reliably returns the same address.)
        const void *probe_ptr = NULL;
        esp_partition_mmap_handle_t probe_handle = 0;
        esp_err_t err = esp_partition_mmap(part, slot_offset, m.text_size, ESP_PARTITION_MMAP_INST,
                                            &probe_ptr, &probe_handle);
        if (err != ESP_OK) { ESP_LOGE(TAG, "probe mmap failed: %s", esp_err_to_name(err)); goto fail; }
        uint32_t probe_base = (uint32_t)probe_ptr;
        esp_partition_munmap(probe_handle);

        uint8_t *buf = heap_caps_malloc(m.text_size, MALLOC_CAP_8BIT);
        if (!buf) { ESP_LOGE(TAG, "patch buffer alloc failed"); goto fail; }
        memcpy(buf, m.text, m.text_size);

        for (int i = 0; i < m.patch_count; i++) {
            const claw_patch_t *p = &m.patches[i];
            uint32_t base;
            switch (p->target_kind) {
                case CLAW_SEC_TEXT:   base = probe_base;                break;
                case CLAW_SEC_RODATA: base = (uint32_t)out->rodata_ram; break;
                case CLAW_SEC_DATA:   base = (uint32_t)out->data_ram;   break;
                case CLAW_SEC_BSS:    base = (uint32_t)out->bss_ram;    break;
                // target_off is already the fully-resolved absolute address
                // (import's addr + relocation addend) — see claw_elf.h's
                // claw_patch_t comment. No section base to add.
                case CLAW_SEC_EXTERN: base = 0;                         break;
                default: ESP_LOGE(TAG, "unknown patch target_kind %d", (int)p->target_kind); heap_caps_free(buf); goto fail;
            }
            uint32_t target_addr = base + p->target_off;
            memcpy(buf + p->text_off, &target_addr, sizeof(target_addr));
        }

        // Erase exactly enough sectors to cover m.text_size, rounded up to
        // the flash's own erase granularity and bounded to this slot's own
        // region — NOT part->erase_size unconditionally the way this used
        // to read. That was harmless for the ~1.6KB test payloads this was
        // ever exercised with (well under one erase sector), but wrong in
        // general: a text segment bigger than one erase_size unit would
        // get only its first sector erased, and writing over the
        // un-erased remainder can only clear bits, silently corrupting
        // the tail of the write. In the multi-slot world getting this
        // bound wrong is worse than silent corruption of your own module —
        // erase_len is clamped to slot_size so a miscalculation here can
        // never reach into a NEIGHBORING slot's still-loaded code.
        uint32_t erase_len = ((m.text_size + part->erase_size - 1) / part->erase_size) * part->erase_size;
        if (erase_len > slot_size) erase_len = slot_size;
        esp_err_t erase_err = esp_partition_erase_range(part, slot_offset, erase_len);
        if (erase_err != ESP_OK) { ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(erase_err)); heap_caps_free(buf); goto fail; }
        esp_err_t write_err = esp_partition_write(part, slot_offset, buf, m.text_size);
        heap_caps_free(buf);
        if (write_err != ESP_OK) { ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(write_err)); goto fail; }

        const void *exec_ptr = NULL;
        esp_partition_mmap_handle_t exec_handle = 0;
        err = esp_partition_mmap(part, slot_offset, m.text_size, ESP_PARTITION_MMAP_INST,
                                  &exec_ptr, &exec_handle);
        if (err != ESP_OK) { ESP_LOGE(TAG, "exec mmap failed: %s", esp_err_to_name(err)); goto fail; }
        if (probe_base != (uint32_t)exec_ptr) {
            // Confirmed-live assumption not holding — fail loudly rather
            // than run code that was patched against the wrong address.
            ESP_LOGE(TAG, "mmap base changed between probe (0x%08x) and real map (0x%08x) — "
                          "aborting rather than running mis-patched code", (unsigned)probe_base, (unsigned)(uint32_t)exec_ptr);
            esp_partition_munmap(exec_handle);
            goto fail;
        }

        out->mmap_handle = (uint32_t)exec_handle;
        out->slot   = slot;
        out->init   = (claw_init_fn)((uint32_t)exec_ptr + m.entry_off);
        out->deinit = (claw_deinit_fn)((uint32_t)exec_ptr + deinit_off);
    }

    ESP_LOGI(TAG, "loaded into slot %d/%d: init=%p deinit=%p",
             slot, CLAW_MAX_SLOTS, (void *)out->init, (void *)out->deinit);
    claw_elf_free(&m);
    return true;

fail:
    claw_elf_free(&m);
    if (slot >= 0) slot_table_for(pool)[slot] = false;
    if (out->rodata_ram) heap_caps_free(out->rodata_ram);
    if (out->data_ram)   heap_caps_free(out->data_ram);
    if (out->bss_ram)    heap_caps_free(out->bss_ram);
    memset(out, 0, sizeof(*out));
    return false;
}

void claw_loader_unload(claw_loaded_module_t *m)
{
    // Same "was this ever really loaded" guard claw_loader_load()'s own
    // fail path uses (mmap_handle is 0 for a zeroed/never-loaded struct) —
    // gates releasing the slot too, so calling this on a struct that was
    // never successfully loaded can't accidentally free a slot some OTHER
    // module is actually using (a zeroed struct's `slot` field is 0, not a
    // sentinel, so this check matters, not just the mmap_handle one).
    // m->pool is whichever pool claw_loader_load() recorded it under —
    // releases the SAME pool's slot table it was claimed from.
    if (m->mmap_handle) {
        esp_partition_munmap((esp_partition_mmap_handle_t)m->mmap_handle);
        if (m->slot >= 0 && m->slot < CLAW_MAX_SLOTS) slot_table_for(m->pool)[m->slot] = false;
    }
    if (m->rodata_ram) heap_caps_free(m->rodata_ram);
    if (m->data_ram)   heap_caps_free(m->data_ram);
    if (m->bss_ram)    heap_caps_free(m->bss_ram);
    memset(m, 0, sizeof(*m));
}

// ── Personal-space storage ──────────────────────────────────────────────────
// See claw_loader.h's own header comment on this section for the design.

// SD preferred (more capacity, the original/established location) —
// /flash/personal as a fallback for a device with no SD card at all (e.g.
// Heltec V3, see purr_kernel_flash_available()'s own doc comment). Every
// function below already treated "no SD" as "no personal space at all"
// (false/0/NULL); this just widens what counts as having one, and every
// caller's own contract is unchanged. On a flash-only device this is
// genuinely small — Heltec's own SPIFFS partition is 1MB total, shared
// with everything else that lands on /flash — fine for small pushed apps,
// not a general-purpose store; that constraint is real, not hidden.
static const char *personal_root(void)
{
    if (purr_kernel_sd_available())    return "/sdcard/personal";
    if (purr_kernel_flash_available()) return "/flash/personal";
    return NULL;
}

const char *claw_loader_personal_root(void) { return personal_root(); }

// Builds <root>/<username> into `out` (out_sz-bounded). Shared by every
// function below so the path format lives in exactly one place.
static void personal_dir_path(const char *root, const char *username, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/%s", root, username);
}

static void personal_file_path(const char *root, const char *username, const char *appname, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/%s/%s.claw", root, username, appname);
}

bool claw_loader_personal_add(const char *username, const char *appname,
                               const uint8_t *obj_bytes, size_t obj_len)
{
    const char *root = personal_root();
    if (!root) return false;

    // <root> itself is NOT in kernel_tdp_boot.c's ensure_sd_dirs() static
    // list (usernames aren't known at boot, and this path can now be on
    // /flash instead of /sdcard anyway) — ensure both levels here instead,
    // same stat()-then-mkdir() idiom that list already uses.
    struct stat st;
    if (stat(root, &st) != 0) {
        if (mkdir(root, 0755) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", root);
            return false;
        }
    }

    char dir_path[300];
    personal_dir_path(root, username, dir_path, sizeof(dir_path));
    if (stat(dir_path, &st) != 0) {
        if (mkdir(dir_path, 0755) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", dir_path);
            return false;
        }
    }

    char file_path[300];
    personal_file_path(root, username, appname, file_path, sizeof(file_path));
    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed", file_path);
        return false;
    }
    size_t written = fwrite(obj_bytes, 1, obj_len, f);
    fclose(f);
    if (written != obj_len) {
        ESP_LOGE(TAG, "short write to %s (%u of %u bytes) — removing partial file",
                 file_path, (unsigned)written, (unsigned)obj_len);
        remove(file_path);
        return false;
    }

    ESP_LOGI(TAG, "personal: added %s/%s.claw (%u B)", username, appname, (unsigned)obj_len);
    // A newly-added personal app changes what app_manager_scan_cached()'s
    // next call would find — see that function's own doc comment (app_
    // manager.h) on why this is the one place that needs to say so
    // explicitly, for every caller of THIS function (server_mgr's app
    // push, app_manager_remote's download) for free.
    app_manager_mark_scan_dirty();
    return true;
}

int claw_loader_personal_count(const char *username)
{
    const char *root = personal_root();
    if (!root) return 0;

    char dir_path[300];
    personal_dir_path(root, username, dir_path, sizeof(dir_path));
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *ext = strrchr(ent->d_name, '.');
        if (ext && strcmp(ext, ".claw") == 0) count++;
    }
    closedir(d);
    return count;
}

bool claw_loader_personal_at(const char *username, int idx, char *name_out, size_t name_out_sz)
{
    const char *root = personal_root();
    if (!root || idx < 0) return false;

    char dir_path[300];
    personal_dir_path(root, username, dir_path, sizeof(dir_path));
    DIR *d = opendir(dir_path);
    if (!d) return false;

    // Same linear readdir() walk app_manager.c's scan_dir() uses — this
    // directory is expected to hold at most a handful of entries (one
    // user's own personal apps), so there's no need for anything fancier.
    int seen = 0;
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcmp(ext, ".claw") != 0) continue;
        if (seen == idx) {
            size_t base_len = (size_t)(ext - ent->d_name);
            if (base_len >= name_out_sz) base_len = name_out_sz - 1;
            memcpy(name_out, ent->d_name, base_len);
            name_out[base_len] = '\0';
            found = true;
            break;
        }
        seen++;
    }
    closedir(d);
    return found;
}

bool claw_loader_personal_remove(const char *username, const char *appname)
{
    const char *root = personal_root();
    if (!root) return false;

    char file_path[300];
    personal_file_path(root, username, appname, file_path, sizeof(file_path));
    if (remove(file_path) != 0) {
        ESP_LOGW(TAG, "personal: remove %s failed (not found?)", file_path);
        return false;
    }
    ESP_LOGI(TAG, "personal: removed %s/%s.claw", username, appname);
    app_manager_mark_scan_dirty();   // see claw_loader_personal_add()'s own call site
    return true;
}

bool claw_loader_personal_load(const char *username, const char *appname, claw_loaded_module_t *out)
{
    const char *root = personal_root();
    if (!root) return false;

    char file_path[300];
    personal_file_path(root, username, appname, file_path, sizeof(file_path));
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "personal: fopen %s failed", file_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        ESP_LOGE(TAG, "personal: %s is empty or ftell failed", file_path);
        fclose(f);
        return false;
    }

    // Read into a plain heap buffer, not PSRAM-capped — this is a short-
    // lived staging buffer (freed below, before this function returns),
    // not something claw_loader_load() keeps a reference into past its own
    // return (see claw_elf.h's zero-copy note: text/rodata/data point INTO
    // it only for the duration of that one call).
    uint8_t *buf = heap_caps_malloc((size_t)fsize, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "personal: alloc failed (%ld B) for %s", fsize, file_path);
        fclose(f);
        return false;
    }
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (read_n != (size_t)fsize) {
        ESP_LOGE(TAG, "personal: short read of %s (%u of %ld bytes)", file_path, (unsigned)read_n, fsize);
        heap_caps_free(buf);
        return false;
    }

    bool ok = claw_loader_load(buf, (size_t)fsize, CLAW_POOL_DYNAMIC, out);
    heap_caps_free(buf);
    if (!ok) {
        ESP_LOGE(TAG, "personal: claw_loader_load failed for %s/%s", username, appname);
    }
    return ok;
}

// ── System-space storage ─────────────────────────────────────────────────
// See claw_loader.h's own header comment on this section for the design.
//
// NOT personal_root()'s SD-preferred/flash-fallback logic, deliberately —
// that ordering exists purely for STORAGE CAPACITY (SD has more room than
// a device's own SPIFFS partition), which doesn't apply here: a system
// package ships baked into /flash's SPIFFS image at build time
// (purrstrap.py's _stage_sysclaw_packages()), guaranteed present the
// moment the device is flashed. /sdcard/system is instead an OPTIONAL
// OVERRIDE location — same "/sdcard copy overrides the shipped default"
// precedent purr_quirk_load() already established for device.purr — so a
// later manual/pushed update (over the existing app transfer system, see
// this header's own top comment) never has to touch the read-mostly
// SPIFFS image at all. claw_loader_system_load() checks the override
// first, the shipped default second; claw_loader_system_install() only
// ever writes the override location, never /flash.
//
// Confirmed live the ordering matters, not just in theory: the very first
// hardware test of this (T-Deck Plus, real SD card mounted) failed with
// "fopen /sdcard/system/login_ui.claw failed" using personal_root()'s
// SD-preferred logic, because purrstrap only ever staged the package into
// /flash — there was never going to be anything at the SD path at all on
// a fresh device.

static const char *system_override_root(void)
{
    return purr_kernel_sd_available() ? "/sdcard/system" : NULL;
}

static const char *system_default_root(void)
{
    return purr_kernel_flash_available() ? "/flash/system" : NULL;
}

// Public accessor documents the override location specifically — the one
// a caller might need to stage an update INTO (claw_loader_personal_add()'s
// own claw_loader_personal_root() plays the equivalent role for personal
// apps); the shipped-default location is purrstrap's own concern, not
// runtime code's.
const char *claw_loader_system_root(void) { return system_override_root(); }

static void system_file_path(const char *root, const char *name, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/%s.claw", root, name);
}

bool claw_loader_system_install(const char *name, const uint8_t *obj_bytes, size_t obj_len)
{
    const char *root = system_override_root();
    if (!root) return false;

    struct stat st;
    if (stat(root, &st) != 0) {
        if (mkdir(root, 0755) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", root);
            return false;
        }
    }

    char file_path[300];
    system_file_path(root, name, file_path, sizeof(file_path));
    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed", file_path);
        return false;
    }
    size_t written = fwrite(obj_bytes, 1, obj_len, f);
    fclose(f);
    if (written != obj_len) {
        ESP_LOGE(TAG, "short write to %s (%u of %u bytes) — removing partial file",
                 file_path, (unsigned)written, (unsigned)obj_len);
        remove(file_path);
        return false;
    }

    ESP_LOGI(TAG, "system: installed override %s.claw (%u B)", name, (unsigned)obj_len);
    return true;
}

static bool try_load_system_file(const char *file_path, claw_loaded_module_t *out)
{
    FILE *f = fopen(file_path, "rb");
    if (!f) return false;   // not present here — caller tries the next candidate, not an error

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        ESP_LOGE(TAG, "system: %s is empty or ftell failed", file_path);
        fclose(f);
        return false;
    }

    // Same "short-lived staging buffer, not PSRAM-capped" reasoning as
    // claw_loader_personal_load()'s own read buffer.
    uint8_t *buf = heap_caps_malloc((size_t)fsize, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "system: alloc failed (%ld B) for %s", fsize, file_path);
        fclose(f);
        return false;
    }
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (read_n != (size_t)fsize) {
        ESP_LOGE(TAG, "system: short read of %s (%u of %ld bytes)", file_path, (unsigned)read_n, fsize);
        heap_caps_free(buf);
        return false;
    }

    bool ok = claw_loader_load(buf, (size_t)fsize, CLAW_POOL_SYSTEM, out);
    heap_caps_free(buf);
    if (!ok) {
        ESP_LOGE(TAG, "system: claw_loader_load failed for %s", file_path);
    }
    return ok;
}

bool claw_loader_system_load(const char *name, claw_loaded_module_t *out)
{
    char file_path[300];

    const char *override_root = system_override_root();
    if (override_root) {
        system_file_path(override_root, name, file_path, sizeof(file_path));
        if (try_load_system_file(file_path, out)) {
            ESP_LOGI(TAG, "system: loaded %s.claw from override (%s)", name, override_root);
            return true;
        }
    }

    const char *default_root = system_default_root();
    if (default_root) {
        system_file_path(default_root, name, file_path, sizeof(file_path));
        if (try_load_system_file(file_path, out)) return true;
    }

    ESP_LOGE(TAG, "system: %s.claw not found in override or shipped-default location", name);
    return false;
}

// ── Module lifecycle ────────────────────────────────────────────────────────
// No real setup needed — the flash partition is looked up fresh on every
// claw_loader_load() call (esp_partition_find_first() is cheap, and the
// slot could in principle live behind a partition that isn't present until
// later, e.g. an SD-backed one some future device uses instead), and there
// is no other state to initialize. Registered anyway, matching every other
// system module's shape (sig_mgr.c is the same story: some modules genuinely
// have nothing to do at init and register purely so app_manager/other
// modules can discover this one exists via purr_kernel_get_module()).

static int claw_loader_init(void) {
    ESP_LOGI(TAG, "ready");
    return 0;
}

static void claw_loader_deinit(void) {
}

PURR_MODULE_REGISTER(claw_loader) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "claw_loader",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = claw_loader_init,
    .deinit            = claw_loader_deinit,
};
