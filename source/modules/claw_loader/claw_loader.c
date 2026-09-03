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

static const char *TAG = "claw_loader";

// The import table — see claw_elf.h's own header comment on
// CLAW_SEC_EXTERN/claw_import_t: this list IS the capability boundary for
// loaded code. A module can call a named host function only if it's listed
// here; anything else in the firmware is unreachable by name to it,
// deliberately. Starts with exactly one entry — enough to prove the
// mechanism on real hardware — grown as real personal-space modules need
// more.
static const claw_import_t s_imports[] = {
    { "purr_kernel_uptime_ms", (uint32_t)&purr_kernel_uptime_ms },
};
#define CLAW_IMPORT_COUNT (sizeof(s_imports) / sizeof(s_imports[0]))

// "claw_slot" — promoted from the R&D spike's "claw_poc" partition once
// three real-hardware rounds confirmed the approach (see
// partitions_16mb_ota.csv's own comment on this partition for the sizing
// story). Same name change reasoning as everywhere else in this codebase
// that graduates scratch work: the partition's JOB didn't change, only
// whether it's still "may be abandoned" scratch.
#define CLAW_SLOT_PARTITION_NAME "claw_slot"
#define CLAW_SLOT_SUBTYPE        0x40

bool claw_loader_load(const uint8_t *obj_bytes, size_t obj_len, claw_loaded_module_t *out)
{
    memset(out, 0, sizeof(*out));

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

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, CLAW_SLOT_SUBTYPE, CLAW_SLOT_PARTITION_NAME);
    if (!part) {
        ESP_LOGE(TAG, "%s partition not found — check partitions_16mb_ota.csv", CLAW_SLOT_PARTITION_NAME);
        claw_elf_free(&m);
        return false;
    }
    if (m.text_size > part->size) {
        ESP_LOGE(TAG, "text (%u B) too big for %s partition (%u B)",
                 (unsigned)m.text_size, CLAW_SLOT_PARTITION_NAME, (unsigned)part->size);
        claw_elf_free(&m);
        return false;
    }

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
        esp_err_t err = esp_partition_mmap(part, 0, m.text_size, ESP_PARTITION_MMAP_INST,
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

        esp_err_t erase_err = esp_partition_erase_range(part, 0, part->erase_size);
        if (erase_err != ESP_OK) { ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(erase_err)); heap_caps_free(buf); goto fail; }
        esp_err_t write_err = esp_partition_write(part, 0, buf, m.text_size);
        heap_caps_free(buf);
        if (write_err != ESP_OK) { ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(write_err)); goto fail; }

        const void *exec_ptr = NULL;
        esp_partition_mmap_handle_t exec_handle = 0;
        err = esp_partition_mmap(part, 0, m.text_size, ESP_PARTITION_MMAP_INST,
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
        out->init   = (claw_init_fn)((uint32_t)exec_ptr + m.entry_off);
        out->deinit = (claw_deinit_fn)((uint32_t)exec_ptr + deinit_off);
    }

    ESP_LOGI(TAG, "loaded: init=%p deinit=%p", (void *)out->init, (void *)out->deinit);
    claw_elf_free(&m);
    return true;

fail:
    claw_elf_free(&m);
    if (out->rodata_ram) heap_caps_free(out->rodata_ram);
    if (out->data_ram)   heap_caps_free(out->data_ram);
    if (out->bss_ram)    heap_caps_free(out->bss_ram);
    memset(out, 0, sizeof(*out));
    return false;
}

void claw_loader_unload(claw_loaded_module_t *m)
{
    if (m->mmap_handle) esp_partition_munmap((esp_partition_mmap_handle_t)m->mmap_handle);
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

    bool ok = claw_loader_load(buf, (size_t)fsize, out);
    heap_caps_free(buf);
    if (!ok) {
        ESP_LOGE(TAG, "personal: claw_loader_load failed for %s/%s", username, appname);
    }
    return ok;
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
