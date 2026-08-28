// claw_poc_test.c — EXPERIMENTAL, cross-device .claw loading R&D spike.
//
// Rounds 1-2 (see git history on this file) proved the underlying
// mechanism using a hand-run extraction script (extract.py, scratch-only,
// never in this repo) that pre-parsed a compiled guest.o on the HOST and
// baked the result into a C header. This round replaces that with
// claw_elf.c — a real ELF32 parser that reads the section/symbol/
// relocation tables at RUNTIME, on-device — the actual capability needed
// before this can load anything it didn't already know about at build
// time. claw_poc_raw_blobs.h now holds the RAW compiled .o bytes, not a
// pre-digested patch list.
//
// Architecture this converged on (full story across all three rounds):
//   - Xtensa GCC's -fPIC/-fpie are dead ends on this toolchain: confirmed
//     empirically (objdump) that they emit byte-identical output to a
//     plain compile.
//   - call8/l32r-to-an-intra-section-target ARE already PC-relative — self-
//     relocating as long as a section loads as one contiguous blob. Only
//     cross-section absolute references (a literal-pool slot holding
//     &something in a DIFFERENT section) need patching — claw_elf.c's
//     R_XTENSA_32-only filter, generalized from the two hand-picked cases
//     rounds 1-2 used.
//   - PSRAM has no MALLOC_CAP_EXEC region on this chip; IRAM is small and
//     already under pressure. Code stays flash-XIP, mapped via
//     esp_partition_mmap(..., ESP_PARTITION_MMAP_INST, ...).
//   - .rodata/.data/.bss all become plain RAM copies at load time (round 1
//     originally tried flash-mapping .rodata too — two real crashes,
//     diagnosed not guessed, both explained in the ELF blob's own git
//     history) — only .text needs the flash-XIP treatment.
//   - Calling BACK into host firmware needs no patching at all — it's a
//     runtime function-pointer value (callx8 through a register), never a
//     compile-time symbol — round 2's whole point, and the reason the
//     existing purr_win.h/purr_kernel.h call shape should just work loaded.

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "claw_elf.h"
#include "claw_poc_raw_blobs.h"

static const char *POC_TAG = "claw_poc";

// Holds everything a loaded module needs kept alive for the duration of a
// call into it, and everything needed to tear it back down afterward.
typedef struct {
    void                       *exec_ptr;     // entry point, already offset by entry_off — cast and call
    void                       *rodata_ram, *data_ram, *bss_ram;
    esp_partition_mmap_handle_t mmap_handle;
} claw_loaded_t;

// Shared by both rounds below: parse -> alloc RAM copies for rodata/data/
// bss -> probe-mmap .text to learn its base -> build the fully patched
// .text buffer -> erase+write -> map again for real -> return the entry
// pointer. Every relocation.claw_elf_load() returned (not just the two
// hand-picked cases rounds 1-2 patched) gets applied generically here,
// keyed off target_kind.
static bool claw_load(const uint8_t *obj_bytes, size_t obj_len, const char *entry_symbol, claw_loaded_t *out)
{
    memset(out, 0, sizeof(*out));

    claw_module_t m;
    if (!claw_elf_load(obj_bytes, obj_len, entry_symbol, &m)) {
        ESP_LOGE(POC_TAG, "ELF parse failed for '%s'", entry_symbol);
        return false;
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "claw_poc");
    if (!part) {
        ESP_LOGE(POC_TAG, "claw_poc partition not found — check partitions_16mb_ota.csv");
        claw_elf_free(&m);
        return false;
    }
    if (m.text_size > part->size) {
        ESP_LOGE(POC_TAG, "text (%u B) too big for claw_poc partition (%u B)",
                 (unsigned)m.text_size, (unsigned)part->size);
        claw_elf_free(&m);
        return false;
    }

    if (m.rodata_size) {
        out->rodata_ram = heap_caps_malloc(m.rodata_size, MALLOC_CAP_8BIT);
        if (!out->rodata_ram) { ESP_LOGE(POC_TAG, "rodata alloc failed"); goto fail; }
        memcpy(out->rodata_ram, m.rodata, m.rodata_size);
    }
    if (m.data_size) {
        out->data_ram = heap_caps_malloc(m.data_size, MALLOC_CAP_8BIT);
        if (!out->data_ram) { ESP_LOGE(POC_TAG, "data alloc failed"); goto fail; }
        memcpy(out->data_ram, m.data, m.data_size);
    }
    if (m.bss_size) {
        out->bss_ram = heap_caps_malloc(m.bss_size, MALLOC_CAP_8BIT);
        if (!out->bss_ram) { ESP_LOGE(POC_TAG, "bss alloc failed"); goto fail; }
        memset(out->bss_ram, 0, m.bss_size);
    }

    // Probe-mmap just to learn the base address — content doesn't matter
    // yet. Flash bits can only go 1->0 without a fresh erase, so patching
    // in place after one write would silently do nothing (see round 1's
    // own history) — this base is only used to COMPUTE the patched buffer
    // below, written for real afterward.
    const void *probe_ptr = NULL;
    esp_partition_mmap_handle_t probe_handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, m.text_size, ESP_PARTITION_MMAP_INST,
                                        &probe_ptr, &probe_handle);
    if (err != ESP_OK) { ESP_LOGE(POC_TAG, "probe mmap failed: %s", esp_err_to_name(err)); goto fail; }
    uint32_t probe_base = (uint32_t)probe_ptr;
    esp_partition_munmap(probe_handle);

    uint8_t *buf = heap_caps_malloc(m.text_size, MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(POC_TAG, "patch buffer alloc failed"); goto fail; }
    memcpy(buf, m.text, m.text_size);

    for (int i = 0; i < m.patch_count; i++) {
        const claw_patch_t *p = &m.patches[i];
        uint32_t base;
        const char *kind_name;
        switch (p->target_kind) {
            case CLAW_SEC_TEXT:   base = probe_base;              kind_name = ".text (flash)"; break;
            case CLAW_SEC_RODATA: base = (uint32_t)out->rodata_ram; kind_name = ".rodata (RAM)"; break;
            case CLAW_SEC_DATA:   base = (uint32_t)out->data_ram;   kind_name = ".data (RAM)";   break;
            case CLAW_SEC_BSS:    base = (uint32_t)out->bss_ram;    kind_name = ".bss (RAM)";    break;
            default: ESP_LOGE(POC_TAG, "unknown patch target_kind %d", (int)p->target_kind); heap_caps_free(buf); goto fail;
        }
        uint32_t target_addr = base + p->target_off;
        memcpy(buf + p->text_off, &target_addr, sizeof(target_addr));
        ESP_LOGI(POC_TAG, "patch[%d]: .text+%u <- 0x%08x (%s)", i, (unsigned)p->text_off,
                 (unsigned)target_addr, kind_name);
    }

    err = esp_partition_erase_range(part, 0, part->erase_size);
    if (err != ESP_OK) { ESP_LOGE(POC_TAG, "erase failed: %s", esp_err_to_name(err)); heap_caps_free(buf); goto fail; }
    err = esp_partition_write(part, 0, buf, m.text_size);
    heap_caps_free(buf);
    if (err != ESP_OK) { ESP_LOGE(POC_TAG, "write failed: %s", esp_err_to_name(err)); goto fail; }

    const void *exec_ptr = NULL;
    err = esp_partition_mmap(part, 0, m.text_size, ESP_PARTITION_MMAP_INST,
                              &exec_ptr, &out->mmap_handle);
    if (err != ESP_OK) { ESP_LOGE(POC_TAG, "exec mmap failed: %s", esp_err_to_name(err)); goto fail; }
    ESP_LOGI(POC_TAG, "probe_base=0x%08x exec_base=0x%08x (%s)",
             (unsigned)probe_base, (unsigned)(uint32_t)exec_ptr,
             probe_base == (uint32_t)exec_ptr ? "MATCH" : "MISMATCH — patch values are wrong");

    out->exec_ptr = (void *)((uint32_t)exec_ptr + m.entry_off);
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

static void claw_unload(claw_loaded_t *cl)
{
    if (cl->mmap_handle) esp_partition_munmap(cl->mmap_handle);
    if (cl->rodata_ram) heap_caps_free(cl->rodata_ram);
    if (cl->data_ram)   heap_caps_free(cl->data_ram);
    if (cl->bss_ram)    heap_caps_free(cl->bss_ram);
    memset(cl, 0, sizeof(*cl));
}

// ── Round 1 (via the ELF parser now): pure self-contained relocation ───────

typedef int (*guest_entry_fn)(int);

void claw_poc_run(void)
{
    claw_loaded_t cl;
    if (!claw_load(guest_o_bytes, GUEST_O_BYTES_LEN, "entry", &cl)) return;

    guest_entry_fn entry_fn = (guest_entry_fn)cl.exec_ptr;
    ESP_LOGI(POC_TAG, "calling loaded entry() at %p ...", (void *)entry_fn);
    int result = entry_fn(10);
    ESP_LOGI(POC_TAG, "entry(10) = %d (expected 92)", result);
    ESP_LOGI(POC_TAG, "%s", result == 92 ? "POC PASS (via ELF parser)" : "POC FAIL");

    claw_unload(&cl);
}

// ── Round 2 (via the ELF parser now): calling back into host firmware ──────

typedef void (*host_log_fn)(const char *msg);
typedef int (*guest2_entry_fn)(int, host_log_fn);

static void poc_host_log(const char *msg)
{
    ESP_LOGI(POC_TAG, "[guest->host callback] %s", msg);
}

void claw_poc_run2(void)
{
    claw_loaded_t cl;
    if (!claw_load(guest2_o_bytes, GUEST2_O_BYTES_LEN, "entry2", &cl)) return;

    guest2_entry_fn entry2_fn = (guest2_entry_fn)cl.exec_ptr;
    ESP_LOGI(POC_TAG, "calling loaded entry2() at %p, passing host callback %p ...",
             (void *)entry2_fn, (void *)poc_host_log);
    int result = entry2_fn(21, poc_host_log);
    ESP_LOGI(POC_TAG, "entry2(21, poc_host_log) = %d (expected 42)", result);
    ESP_LOGI(POC_TAG, "%s", result == 42 ? "POC ROUND 2 PASS (via ELF parser)" : "POC ROUND 2 FAIL");

    claw_unload(&cl);
}
