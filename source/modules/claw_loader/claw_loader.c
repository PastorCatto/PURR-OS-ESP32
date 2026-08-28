// claw_loader.c — see claw_loader.h for the full picture.
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "purr_module.h"
#include "claw_elf.h"
#include "claw_loader.h"

static const char *TAG = "claw_loader";

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
    if (!claw_elf_load(obj_bytes, obj_len, "claw_personal_init", &m)) {
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
