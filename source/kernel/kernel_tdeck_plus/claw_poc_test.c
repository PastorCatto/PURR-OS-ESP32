// claw_poc_test.c — EXPERIMENTAL, cross-device .claw loading R&D spike.
//
// Proves (or disproves, on real hardware, not by argument) the architecture
// this session's investigation converged on:
//
//   - Xtensa GCC's -fPIC/-fpie are dead ends here: confirmed empirically
//     (objdump on real compiles) that they emit byte-identical output to a
//     plain compile — no GOT, no PC-relative data access, nothing.
//   - But call8/l32r ARE already PC-relative — a compiled .o's intra-section
//     references (a call to a sibling static function, a literal-pool load
//     of another literal in the same section) are self-relocating as long
//     as the section loads as one contiguous blob. Only cross-section
//     absolute references (a literal-pool slot holding &some_global, which
//     lives in a different section entirely) need patching — the same
//     technique Linux kernel modules (.ko) use.
//   - PSRAM has no MALLOC_CAP_EXEC region on this chip (confirmed against
//     ESP-IDF's own esp32s3 heap_caps memory-layout table) and IRAM is
//     small/already under pressure — so loaded code doesn't live in RAM at
//     all. It lives in the claw_poc flash partition, mapped executable via
//     esp_partition_mmap(..., ESP_PARTITION_MMAP_INST, ...) — real, existing
//     ESP-IDF API for exactly this.
//
// guest_text/guest_rodata/guest_patches (claw_poc_blob.h) come from a tiny
// standalone-compiled guest.c that was NEVER linked into this firmware —
// see that header for the source and the expected result.
//
// RUN #1 CRASHED (Guru Meditation, LoadStoreError) calling into the loaded
// entry(): ESP32-S3's split instruction/data bus means flash mapped via
// ESP_PARTITION_MMAP_INST is only reachable via instruction fetch and l32r
// (a special Xtensa instruction that reads literals over the instruction
// bus even though it's technically a load) — entry()'s ORDINARY data load
// against the .rodata string (t[0], an l8ui) faulted against that
// instruction-mapped address.
//
// RUN #2 tried mapping .rodata a second time via ESP_PARTITION_MMAP_DATA —
// same crash, same location. Root cause, found in ESP-IDF's own
// flash_mmap.c, not guessed: when the same physical flash range is already
// mapped, a second esp_partition_mmap() call for it (ESP_MMU_MMAP_FLAG_
// PADDR_SHARED) returns the EXISTING mapping's capabilities rather than
// creating a second, differently-capable one — confirmed live, both calls
// returned the identical base address. .text and .rodata sat in the same
// MMU page (mmap() rounds down to CONFIG_MMU_PAGE_SIZE), so there was only
// ever one mapping regardless of which mode was requested second.
//
// FIX (this run): stop trying to hold two simultaneous views of the same
// physical flash. .rodata is now handled exactly like .bss — copied into a
// plain heap_caps_malloc'd RAM buffer at load time (pre-initialized instead
// of zeroed) — not flash-mapped at all. Only .text stays flash-XIP-executed,
// which is the one thing that actually has to be (nothing here can hold
// arbitrary-sized code in RAM — see the PSRAM/IRAM finding above). Any
// intra-.text literal-pool reference to .text itself (the call8 target, the
// two l32r-to-self-section slots) is still left untouched, per the very
// first finding: those are already correctly PC-relative as compiled.

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "claw_poc_blob.h"
#include "claw_poc_blob2.h"

static const char *POC_TAG = "claw_poc";

typedef int (*guest_entry_fn)(int);

void claw_poc_run(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "claw_poc");
    if (!part) {
        ESP_LOGE(POC_TAG, "claw_poc partition not found — check partitions_16mb_ota.csv");
        return;
    }

    size_t text_size = sizeof(guest_text);
    if (text_size > part->size) {
        ESP_LOGE(POC_TAG, "blob (%u B) too big for claw_poc partition (%u B)",
                 (unsigned)text_size, (unsigned)part->size);
        return;
    }

    // .bss and .rodata are both plain RAM now — .bss zeroed, .rodata
    // pre-filled with its compiled content. Neither is flash-mapped at
    // all, so there's no instruction/data-bus question for either one.
    void *bss = heap_caps_malloc(GUEST_BSS_SIZE, MALLOC_CAP_8BIT);
    if (!bss) { ESP_LOGE(POC_TAG, "bss alloc failed"); return; }
    memset(bss, 0, GUEST_BSS_SIZE);

    void *rodata_ram = heap_caps_malloc(sizeof(guest_rodata), MALLOC_CAP_8BIT);
    if (!rodata_ram) {
        ESP_LOGE(POC_TAG, "rodata alloc failed");
        heap_caps_free(bss);
        return;
    }
    memcpy(rodata_ram, guest_rodata, sizeof(guest_rodata));

    // Step 1: probe-mmap .text alone to learn the base address it will map
    // to — still needed, since spi_flash_mmap's virtual base isn't a
    // build-time constant, and flash bits can only go 1->0 without a fresh
    // erase (the unpatched blob's relocation slots are already zeroed, see
    // claw_poc_blob.h, so an in-place patch after one write would silently
    // do nothing).
    const void *probe_ptr = NULL;
    esp_partition_mmap_handle_t probe_handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, text_size, ESP_PARTITION_MMAP_INST,
                                        &probe_ptr, &probe_handle);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "probe mmap failed: %s", esp_err_to_name(err));
        heap_caps_free(rodata_ram); heap_caps_free(bss);
        return;
    }
    uint32_t probe_base = (uint32_t)probe_ptr;
    esp_partition_munmap(probe_handle);

    // Step 2: build the fully-patched .text buffer in RAM (this is the
    // staging buffer written to flash below — not where .text executes
    // from). Both patches now point at plain RAM addresses.
    uint8_t *buf = heap_caps_malloc(text_size, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(POC_TAG, "patch buffer alloc failed");
        heap_caps_free(rodata_ram); heap_caps_free(bss);
        return;
    }
    memcpy(buf, guest_text, text_size);

    for (int i = 0; i < GUEST_PATCH_COUNT; i++) {
        const guest_patch_t *p = &guest_patches[i];
        uint32_t target_addr = p->target_section == 0
            ? (uint32_t)rodata_ram + (uint32_t)p->addend
            : (uint32_t)bss        + (uint32_t)p->addend;
        memcpy(buf + p->text_off, &target_addr, sizeof(target_addr));
        ESP_LOGI(POC_TAG, "patch[%d]: .text+%u <- 0x%08x (%s, RAM)", i, (unsigned)p->text_off,
                 (unsigned)target_addr, p->target_section == 0 ? ".rodata" : ".bss");
    }

    // Step 3: erase + write the patched .text buffer for real.
    err = esp_partition_erase_range(part, 0, part->erase_size);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "erase failed: %s", esp_err_to_name(err));
        heap_caps_free(buf); heap_caps_free(rodata_ram); heap_caps_free(bss);
        return;
    }
    err = esp_partition_write(part, 0, buf, text_size);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "write failed: %s", esp_err_to_name(err));
        heap_caps_free(rodata_ram); heap_caps_free(bss);
        return;
    }

    // Step 4: map again, for real this time, and confirm the base-address
    // assumption the patches above were computed against actually held.
    const void *exec_ptr = NULL;
    esp_partition_mmap_handle_t exec_handle = 0;
    err = esp_partition_mmap(part, 0, text_size, ESP_PARTITION_MMAP_INST,
                              &exec_ptr, &exec_handle);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "exec mmap failed: %s", esp_err_to_name(err));
        heap_caps_free(rodata_ram); heap_caps_free(bss);
        return;
    }
    ESP_LOGI(POC_TAG, "probe_base=0x%08x exec_base=0x%08x (%s)",
             (unsigned)probe_base, (unsigned)(uint32_t)exec_ptr,
             probe_base == (uint32_t)exec_ptr ? "MATCH" : "MISMATCH — patch values are wrong");

    guest_entry_fn entry_fn = (guest_entry_fn)((uint32_t)exec_ptr + GUEST_ENTRY_OFF);
    ESP_LOGI(POC_TAG, "calling loaded entry() at %p ...", (void *)entry_fn);
    int result = entry_fn(10);
    ESP_LOGI(POC_TAG, "entry(10) = %d (expected 92)", result);
    ESP_LOGI(POC_TAG, "%s", result == 92 ? "POC PASS" : "POC FAIL");

    esp_partition_munmap(exec_handle);
    heap_caps_free(rodata_ram);
    heap_caps_free(bss);
}

// ── Round 2: can loaded code call BACK into the host firmware? ─────────────
//
// The question round 1 didn't touch at all. catcall_ui_t/purr_win.h already
// dispatch every app->kernel call through a runtime function-pointer struct
// (_UI_CALL/_UI_VOID -> _ui->win_create(...) etc.), never a direct symbol
// reference to a kernel-resident function — confirmed via objdump before
// this was written: the guest's call through a host-supplied function
// pointer compiles to `callx8 a8` with NO relocation entry at all. It's a
// pure runtime value, not a compile-time symbol. If this round passes, app
// code written against the existing purr_win.h/purr_kernel.h headers,
// completely unmodified, should work loaded this way exactly as well as it
// works pre-linked — that's the whole point of testing it.

typedef void (*host_log_fn)(const char *msg);
typedef int (*guest2_entry_fn)(int, host_log_fn);

static void poc_host_log(const char *msg)
{
    ESP_LOGI(POC_TAG, "[guest->host callback] %s", msg);
}

void claw_poc_run2(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "claw_poc");
    if (!part) {
        ESP_LOGE(POC_TAG, "claw_poc partition not found — check partitions_16mb_ota.csv");
        return;
    }

    size_t text_size = sizeof(guest2_text);
    if (text_size > part->size) {
        ESP_LOGE(POC_TAG, "blob (%u B) too big for claw_poc partition (%u B)",
                 (unsigned)text_size, (unsigned)part->size);
        return;
    }

    void *rodata_ram = heap_caps_malloc(sizeof(guest2_rodata), MALLOC_CAP_8BIT);
    if (!rodata_ram) { ESP_LOGE(POC_TAG, "rodata alloc failed"); return; }
    memcpy(rodata_ram, guest2_rodata, sizeof(guest2_rodata));

    const void *probe_ptr = NULL;
    esp_partition_mmap_handle_t probe_handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, text_size, ESP_PARTITION_MMAP_INST,
                                        &probe_ptr, &probe_handle);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "probe mmap failed: %s", esp_err_to_name(err));
        heap_caps_free(rodata_ram);
        return;
    }
    uint32_t probe_base = (uint32_t)probe_ptr;
    esp_partition_munmap(probe_handle);

    uint8_t *buf = heap_caps_malloc(text_size, MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(POC_TAG, "patch buffer alloc failed"); heap_caps_free(rodata_ram); return; }
    memcpy(buf, guest2_text, text_size);

    for (int i = 0; i < GUEST2_PATCH_COUNT; i++) {
        const guest_patch_t *p = &guest2_patches[i];
        uint32_t target_addr = (uint32_t)rodata_ram + (uint32_t)p->addend;   // only .rodata this round
        memcpy(buf + p->text_off, &target_addr, sizeof(target_addr));
        ESP_LOGI(POC_TAG, "patch[%d]: .text+%u <- 0x%08x (.rodata, RAM)", i,
                 (unsigned)p->text_off, (unsigned)target_addr);
    }

    err = esp_partition_erase_range(part, 0, part->erase_size);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "erase failed: %s", esp_err_to_name(err));
        heap_caps_free(buf); heap_caps_free(rodata_ram);
        return;
    }
    err = esp_partition_write(part, 0, buf, text_size);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "write failed: %s", esp_err_to_name(err));
        heap_caps_free(rodata_ram);
        return;
    }

    const void *exec_ptr = NULL;
    esp_partition_mmap_handle_t exec_handle = 0;
    err = esp_partition_mmap(part, 0, text_size, ESP_PARTITION_MMAP_INST,
                              &exec_ptr, &exec_handle);
    if (err != ESP_OK) {
        ESP_LOGE(POC_TAG, "exec mmap failed: %s", esp_err_to_name(err));
        heap_caps_free(rodata_ram);
        return;
    }
    ESP_LOGI(POC_TAG, "probe_base=0x%08x exec_base=0x%08x (%s)",
             (unsigned)probe_base, (unsigned)(uint32_t)exec_ptr,
             probe_base == (uint32_t)exec_ptr ? "MATCH" : "MISMATCH — patch values are wrong");

    guest2_entry_fn entry2_fn = (guest2_entry_fn)((uint32_t)exec_ptr + GUEST2_ENTRY_OFF);
    ESP_LOGI(POC_TAG, "calling loaded entry2() at %p, passing host callback %p ...",
             (void *)entry2_fn, (void *)poc_host_log);
    int result = entry2_fn(21, poc_host_log);
    ESP_LOGI(POC_TAG, "entry2(21, poc_host_log) = %d (expected 42)", result);
    ESP_LOGI(POC_TAG, "%s", result == 42 ? "POC ROUND 2 PASS" : "POC ROUND 2 FAIL");

    esp_partition_munmap(exec_handle);
    heap_caps_free(rodata_ram);
}
