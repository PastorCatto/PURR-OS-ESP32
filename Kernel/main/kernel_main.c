/*
 * kernel_main.c - the standalone kernel binary (PurrOS/components/kernel/SPEC.md,
 * PurrOS/SPEC.md section 6).
 *
 * Everything CoreOS needs below it, and nothing above it: hardware bring-up (display,
 * peripheral power, the shared bus) and the root filesystem, built as its own flashable
 * image for the `kernel` partition -- not the monolith that has played both roles until
 * now. This step proves the real separation and the real size reduction; actually loading
 * CoreOS as a relocatable file from here is the next step, not this one, so there is
 * nothing to hand off to yet and app_main() just idles once hardware and the filesystem
 * are confirmed up.
 */
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_console.h"
#include "purr_fs.h"
#include "purr_kernel.h"
#include "purr_kernel_table.h"

#include "kernel_table.h"
#include "loader.h"

static const char *TAG = "kernel";

/* TEMPORARY, 2026-09-30 (PurrOS/SPEC.md section 6): the first real CoreOS entry point this
 * kernel can load, proving mount -> write -> load -> relocate -> call with the real kernel
 * table works end to end, not just the mechanism in isolation (CoreOSSpike) or that real
 * coreos/src compiles this way (the 16-file purrstrap proof). Embedded the same way
 * commands.c's plant_temp_modules() embeds its modules -- temporary scaffolding until a real
 * distribution path exists for this kernel too, same discipline. Delete this array and the
 * plant/load call in app_main() once a real one replaces it. */
static const uint8_t s_coreos_min[] = {
#include "coreos_min_bytes.inc"
};

typedef const purr_kernel_module_table_t *(*coreos_entry_fn)(const purr_kernel_table_t *kernel);

static void load_coreos(purr_fs_t *fs)
{
    const char *path = "/coreos.cat";
    if (purr_fs_write(fs, path, s_coreos_min, sizeof(s_coreos_min)) != 0) {
        ESP_LOGW(TAG, "could not stage %s", path);
        return;
    }

    uint8_t *databuf;
    void *exec_ptr;
    uint32_t entry_offset;
    kernel_load_result_t r = kernel_load_relocatable_file(fs, path, 128 * 1024, 1,
                                                           &databuf, &exec_ptr, &entry_offset);
    if (r != KERNEL_LOAD_OK) {
        ESP_LOGW(TAG, "coreos load failed (%d)", (int)r);
        return;
    }

    coreos_entry_fn entry = (coreos_entry_fn)(uintptr_t)((uint8_t *)exec_ptr + entry_offset);
    const purr_kernel_module_table_t *table = entry(purr_kernel_table());
    if (table == NULL || table->abi_version != PURR_KERNEL_TABLE_ABI_VERSION) {
        ESP_LOGW(TAG, "coreos entry returned a bad table (abi mismatch or NULL)");
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return;
    }
    ESP_LOGI(TAG, "coreos loaded: abi %u, %u command(s)", (unsigned)table->abi_version,
            (unsigned)table->cmd_count);
}

static void kprint(const char *s)
{
    while (*s) {
        purr_console_put(NULL, *s++);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PURR kernel starting");

    if (purr_kernel_init() != ESP_OK || purr_kernel_display() == NULL) {
        /* No display: there is nothing to draw on, so say so on the serial console, the
         * same fallback main.c's own app_main() uses. */
        for (;;) {
            ESP_LOGE(TAG, "display did not come up");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    const purr_display_v2_t *d = purr_kernel_display();
    if (d->set_brightness) {
        d->set_brightness(255);
    }
    purr_console_init(d);
    kprint("PURR kernel\nhardware up\n");
    purr_console_flush();
    ESP_LOGI(TAG, "hardware up");

    static purr_fs_t fs;
    purr_bd_t bd;
    int mounted = (purr_fs_flash_bd("root", &bd) == 0) && (purr_fs_mount(&fs, &bd) == 0);
    kprint(mounted ? "root fs mounted\n" : "root fs NOT mounted\n");
    purr_console_flush();
    ESP_LOGI(TAG, "root filesystem: %s", mounted ? "mounted" : "not mounted");

    if (mounted) {
        kernel_table_set_fs(&fs);
        load_coreos(&fs);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
