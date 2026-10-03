// PURR OS second-stage bootloader.
//
// The ROM bootloader loads this from flash. It does the bare minimum:
// initialise hardware, read the partition table, pick an app slot
// (otadata-aware) and jump to it. Nothing else lives here.
#include <stdbool.h>
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "purr_bootcfg.h"
#include "purr_bootpkg.h"

static const char *TAG = "purr_boot";

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    ESP_LOGI(TAG, "PURR OS bootloader");

    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "failed to load partition table");
        bootloader_reset();
    }

    /* boot_fail_count ladder (bootloader/SPEC.md section 6, PurrOS/components/coreos/SPEC.md
     * section 7): incremented here, on every single boot, before anything else looks at it --
     * not inside the boot package as the spec originally said, because the boot package never
     * runs at all on a FORCE_RECOVERY/FORCE_LOADER boot, and the count needs to track every
     * attempt uniformly for the ladder below to mean anything. CoreOS resets it to 0 once a
     * boot reaches healthy (coreos/SPEC.md section 4 step 8); a boot that never gets that far
     * -- crashed, hung, reset -- leaves it exactly where this raised it, so it climbs across
     * repeated silent failures even when nothing ever made a clean decision about why. */
    purr_cfg_t cfg;
    bool have_cfg = purr_bootcfg_load(&cfg);
    cfg.boot_fail_count++;
    uint32_t fail_count = cfg.boot_fail_count;
    if (!purr_bootcfg_store(&cfg)) {
        ESP_LOGW(TAG, "purrcfg: could not record this boot attempt");
    }

    /* The default chain, PurrOS/SPEC.md's design: prefer the kernel slot (ota_0-subtype,
     * named "kernel" -- kept on that subtype so the stock ESP-IDF loader below still knows
     * how to jump to it; there is no A/B pair anymore, just this one slot); if it doesn't
     * even look like a real app image, fall back to KittenOS; if that's gone too, fall back
     * to the recovery loader (internet recovery) automatically, not just on request. This
     * replaces the old otadata-based bootloader_utility_get_selected_boot_partition() pick,
     * which had nothing left to select between once ota_1 was reclaimed and now ota_0 is
     * renamed -- it would have just always defaulted to factory. purr_looks_bootable() is the
     * same shallow "is this an ESP app image" check the boot menu already uses to gray out a
     * dead slot; a real PURR-signature check before boot here is designed (bootloader/SPEC.md
     * section 6) but not yet built.
     *
     * On top of that shallow check, the ladder above changes which slot is *preferred*, not
     * just which is used when an earlier one is missing: looking bootable only means the
     * first word is a valid magic byte, not that the thing actually ran successfully last
     * time. Three tries at each tier before moving the preferred slot further down: 1-3,
     * kernel; 4-6, kittenos; 7+, the recovery loader. Each tier still falls through the
     * *other* two slots in order if its preferred one isn't there or doesn't look bootable --
     * a missing loader at tier 3 falls back to kittenos next, not all the way back to the
     * kernel that's already proven unreliable. */
    typedef struct { int index; bool is_test; bool is_factory; } slot_t;
    static const slot_t k_tier0[3] = {
        {0, false, false}, {FACTORY_INDEX, false, true}, {TEST_APP_INDEX, true, false},
    };
    static const slot_t k_tier1[3] = {
        {FACTORY_INDEX, false, true}, {TEST_APP_INDEX, true, false}, {0, false, false},
    };
    static const slot_t k_tier2[3] = {
        {TEST_APP_INDEX, true, false}, {FACTORY_INDEX, false, true}, {0, false, false},
    };
    const slot_t *tier = k_tier0;
    if (fail_count > 6) {
        tier = k_tier2;
    } else if (fail_count > 3) {
        tier = k_tier1;
    }
    if (tier != k_tier0) {
        ESP_LOGW(TAG, "boot_fail_count %u: preferring %s", (unsigned)fail_count,
                 tier == k_tier2 ? "the recovery loader" : "KittenOS");
    }

    int boot_index = INVALID_INDEX;
    for (int i = 0; i < 3; i++) {
        const slot_t *s = &tier[i];
        const esp_partition_pos_t *pos = s->is_test ? &bs.test : s->is_factory ? &bs.factory : &bs.ota[0];
        bool present = s->is_test ? bs.test.size != 0 : s->is_factory ? bs.factory.size != 0 : bs.app_count > 0;
        if (present && purr_looks_bootable(pos)) {
            boot_index = s->index;
            break;
        }
    }
    if (boot_index == INVALID_INDEX) {
        ESP_LOGE(TAG, "no bootable app found");
        bootloader_reset();
    }

    (void)have_cfg;
    bool ladder_chose_loader = (boot_index == TEST_APP_INDEX && tier == k_tier2);

    /* A recovery request (the shell's "reboot recovery") skips the menu and starts KittenOS
     * once. "reboot loader" does the same for the recovery loader (the "test" slot,
     * RecoveryLoader/SPEC.md). If the target is not there, fall through to the menu as usual.
     * Either one is a deliberate, explicit request -- it overrides the ladder's pick outright,
     * and never counts as the silent auto path below, even if the ladder happened to agree. */
    if (purr_bootcfg_take_flag(PURR_CFGF_FORCE_RECOVERY) && bs.factory.size != 0) {
        ESP_LOGI(TAG, "recovery requested: starting KittenOS");
        boot_index = FACTORY_INDEX;
        ladder_chose_loader = false;
    } else if (purr_bootcfg_take_flag(PURR_CFGF_FORCE_LOADER) && bs.test.size != 0) {
        ESP_LOGI(TAG, "internet recovery requested: starting the recovery loader");
        boot_index = TEST_APP_INDEX;
        ladder_chose_loader = false;
    } else if (ladder_chose_loader) {
        /* Skip the menu entirely, the same as an explicit FORCE_LOADER request above: nobody
         * may be there to answer one, and purr_bootpkg_run()'s menu has no way to offer the
         * loader as a normal preselected choice anyway -- it only reaches the loader through
         * its own separate "internet recovery" menu action, never through `preferred`. boot_index
         * is already TEST_APP_INDEX from the ladder above. */
        ESP_LOGW(TAG, "repeated boot failure: escalating straight to the recovery loader");
    } else {
        /* The boot package shows the menu and may pick another slot. Without one, boot normally. */
        int chosen;
        if (purr_bootpkg_run(&bs, boot_index, &chosen)) {
            boot_index = chosen;
        }
    }

    /* Landing on the loader because the ladder itself preferred it, and nothing overrode that
     * with an explicit request, means nobody detected a specific reason -- just repeated
     * failure to ever reach healthy. Tell the recovery loader so it runs its silent auto mode
     * instead of a menu nobody may be there to answer (RecoveryLoader/SPEC.md section 2.1). A
     * human who explicitly picked "internet recovery" from the boot package's own menu above
     * still gets the ladder's verdict here if it happened to agree; that's a rare, harmless
     * coincidence, not a way to force auto mode from the menu. */
    if (boot_index == TEST_APP_INDEX && ladder_chose_loader) {
        cfg.flags |= PURR_CFGF_AUTO_REINSTALL;
        if (!purr_bootcfg_store(&cfg)) {
            ESP_LOGW(TAG, "purrcfg: could not record the auto-reinstall request");
        }
    }

    ESP_LOGI(TAG, "booting app slot %d", boot_index);
    bootloader_utility_load_boot_image(&bs, boot_index);
}
