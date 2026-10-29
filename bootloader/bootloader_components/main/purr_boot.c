// PURR OS second-stage bootloader.
//
// The ROM bootloader loads this from flash. It does the bare minimum:
// initialise hardware, read the partition table, pick an app slot
// (kernel, KittenOS or the recovery loader; there is no otadata) and jump to it.
#include <stdbool.h>
/* Our own lines stay at INFO even though the rest of the bootloader logs at WARN. */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "sdkconfig.h"
#include "bootloader_init.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/soc.h"
#include "soc/system_reg.h"
#endif
#include "bootloader_utility.h"
#include "purr_bootcfg.h"
#include "purr_bootpkg.h"
#include "purr_handoff.h"
#include "purr_reason.h"
#include "purr_slotverify.h"

static const char *TAG = "purr_boot";

/* ------------------------------------------------------------ slot policy */

/* Whether a slot may be launched, and what its signature trailer says (bootloader/SPEC.md
 * sections 3.1 and 6). Three app slots in all: the kernel (index 0, the only ota slot),
 * KittenOS (FACTORY_INDEX) and the recovery loader (TEST_APP_INDEX).
 *
 * Verification costs a hash over the whole image, so it only runs when it can change the
 * answer: always for the slot actually being launched (the handoff reports it), and for every
 * slot examined along the way only in enforce mode, where a failed check makes a slot
 * unusable. In off mode nothing is verified at all. */
#define NSLOTS 3

static const bootloader_state_t *s_bs;
static const purr_cfg_t *s_cfg;
static uint8_t s_mode;            /* the secure_mode in force for this boot */
static bool s_ignore_once;        /* IGNORE_ONCE: let a failed slot boot even in enforce */
static struct { bool done; purr_slot_verdict_t v; } s_cache[NSLOTS];
static bool marked_bad(int index);

static int cache_ix(int index)
{
    return index == FACTORY_INDEX ? 1 : index == TEST_APP_INDEX ? 2 : 0;
}

static const esp_partition_pos_t *slot_pos(int index)
{
    if (index == FACTORY_INDEX) {
        return s_bs->factory.size != 0 ? &s_bs->factory : NULL;
    }
    if (index == TEST_APP_INDEX) {
        return s_bs->test.size != 0 ? &s_bs->test : NULL;
    }
    return (index == 0 && s_bs->app_count > 0) ? &s_bs->ota[0] : NULL;
}

static purr_slot_kind_t slot_kind(int index)
{
    return index == FACTORY_INDEX ? PURR_SLOT_KITTENOS :
           index == TEST_APP_INDEX ? PURR_SLOT_LOADER : PURR_SLOT_KERNEL;
}

static const purr_slot_verdict_t *slot_verdict(int index)
{
    int c = cache_ix(index);
    if (!s_cache[c].done) {
        purr_slot_verify(slot_pos(index), slot_kind(index), s_cfg, &s_cache[c].v);
        s_cache[c].done = true;
    }
    return &s_cache[c].v;
}

/* Why the device is being sent to the recovery loader, as PURR_RR_* flags (purr_abi.h), for the loader's first page. Looks at
 * the two system slots (the kernel and KittenOS): damaged, unsigned or altered images, nothing usable at all, and whether this
 * is a first run (the FRESH flag, with nothing installed). Verifies a slot that was not verified yet, so it is only called
 * when the loader is the slot being launched. */
static uint16_t recovery_reasons(const purr_cfg_t *cfg, bool have_cfg, bool ladder, bool requested)
{
    uint16_t r = 0;
    bool fresh = (cfg->flags & PURR_CFGF_FRESH) != 0;
    bool any_usable = false;
    bool reset_pending = false;
    for (int i = 0; i < 2; i++) {
        int idx = i == 0 ? 0 : FACTORY_INDEX;
        const esp_partition_pos_t *pos = slot_pos(idx);
        if (pos == NULL) {
            continue;
        }
        if (marked_bad(idx)) {                  /* a reset is waiting for its reinstall: that is a request, not damage */
            reset_pending = true;
            continue;
        }
        if (!purr_looks_bootable(pos)) {
            if (!fresh) {
                r |= PURR_RR_DAMAGED;           /* a slot that was once installed and no longer looks like an image */
            }
            continue;
        }
        const purr_slot_verdict_t *v = slot_verdict(idx);
        if (s_mode != PURR_SECURE_OFF && !v->verified) {
            r |= purr_reason_from_verify(v->result);
        } else {
            any_usable = true;
        }
    }
    if (reset_pending) {
        r |= PURR_RR_REQUESTED;
    }
    if (!any_usable && !reset_pending) {
        r |= PURR_RR_NO_SYSTEM;
        if (fresh) {
            r |= PURR_RR_BLANK;
        }
    }
    if (ladder) {
        r |= PURR_RR_REPEATED;
    }
    if (requested) {
        r |= PURR_RR_REQUESTED;
    }
    if (!have_cfg) {
        r |= PURR_RR_CONFIG;
    }
    return r;
}

/* A slot a reset marked BAD (PURR_CFGF_BAD_*). Only honoured while the recovery loader is there to take over, so a missing loader can
 * never leave the device with nothing to start. */
static bool marked_bad(int index)
{
    if (s_cfg == NULL || index == TEST_APP_INDEX) {
        return false;
    }
    uint32_t bit = index == FACTORY_INDEX ? PURR_CFGF_BAD_KITTENOS : PURR_CFGF_BAD_KERNEL;
    if ((s_cfg->flags & bit) == 0) {
        return false;
    }
    const esp_partition_pos_t *loader = slot_pos(TEST_APP_INDEX);
    return loader != NULL && purr_looks_bootable(loader);
}

/* Also the boot menu's predicate (purr_bootpkg_run), so it never offers what this refuses. */
bool purr_slot_usable(int index)
{
    const esp_partition_pos_t *pos = slot_pos(index);
    if (pos == NULL || !purr_looks_bootable(pos)) {
        return false;
    }
    if (marked_bad(index)) {
        return false;
    }
    if (s_mode != PURR_SECURE_ENFORCE) {
        return true;
    }
    if (slot_verdict(index)->verified || s_ignore_once) {
        return true;
    }
    /* enforce: a slot that does not verify is not launched. Say so, once per slot, so the log
     * shows a refusal and not just a failed check followed by a different slot booting. */
    static bool said[NSLOTS];
    if (!said[cache_ix(index)]) {
        said[cache_ix(index)] = true;
        ESP_LOGW(TAG, "slot %d REFUSED (%s): enforce does not launch an unverified slot", index,
                 purr_verify_name(slot_verdict(index)->result));
    }
    return false;
}

#if CONFIG_IDF_TARGET_ESP32S3
/* Real-hardware crash loop (plan1.md): after a restart called from a task on core 1, IDF's esp_restart_noos can leave core 1 spinning
 * in its last loop while this bootloader runs. The next app's start-up sees the core's clock already on and so does not reset it
 * (cpu_start.c "OpenOCD may have already enabled clock"), and once the new image overwrites the code the core was spinning in, it
 * dies with IllegalInstruction right after "cpu_start: Multicore app". So: hold the app CPU in reset with its clock off. The app then
 * starts it the normal way. */
/* Two short lines for the problem screen: what was wrong with the slot, in words. */
void purr_slot_failure_text(int index, const char **line1, const char **line2)
{
    const char *fix = "Start recovery to put it back.";
    switch (slot_verdict(index)->result) {
    case PURR_V_BAD_SIGNATURE:
        *line1 = "Signature does not match.";
        *line2 = "It was not signed by a trusted key.";
        return;
    case PURR_V_NO_KEY:
    case PURR_V_ROLE_MISMATCH:
    case PURR_V_REVOKED:
        *line1 = "Not signed by a trusted key.";
        break;
    case PURR_V_BAD_HASH:
        *line1 = "The system files are damaged.";
        break;
    case PURR_V_BELOW_FLOOR:
    case PURR_V_TOO_OLD_BOOTLOADER:
        *line1 = "This version is older than allowed.";
        fix = "Start recovery to update it.";
        break;
    case PURR_V_BAD_MAGIC:
        *line1 = "No signature was found.";
        break;
    default:
        *line1 = "It could not be checked.";
        break;
    }
    *line2 = fix;
}

static void park_app_cpu(void)
{
    SET_PERI_REG_MASK(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    SET_PERI_REG_MASK(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);
    CLEAR_PERI_REG_MASK(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
    WRITE_PERI_REG(SYSTEM_CORE_1_CONTROL_1_REG, 0);
}
#endif

void __attribute__((noreturn)) call_start_cpu0(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    park_app_cpu();
#endif
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

    /* The effective security state (purr_trust.h, plan3.md): the trust block's signature is
     * checked once here against the compiled-in keys, development mode is applied, and from here
     * on every decision reads the VIEW (s_cfg), never the raw purrcfg. The raw cfg is still what
     * gets stored back below. */
    purr_bootcfg_publish(&cfg);
    const purr_trust_t *trust = purr_bootcfg_trust();

    /* The two one-shot loosening flags are consumed on this boot, before anything acts on them
     * (bootloader/SPEC.md section 5), so a crash cannot leave them set for the next boot. They
     * are honoured only in development mode; anywhere else they are cleared and ignored. */
    uint8_t flags_used = 0;     /* one-shot flags consumed this boot, for the handoff */
    bool ignore_flag = (cfg.flags & PURR_CFGF_IGNORE_ONCE) != 0;
    bool off_flag = (cfg.flags & PURR_CFGF_SECURE_OFF_ONCE) != 0;
    if (ignore_flag) {
        flags_used |= PURR_CFGF_IGNORE_ONCE;
    }
    if (off_flag) {
        flags_used |= PURR_CFGF_SECURE_OFF_ONCE;
    }
    cfg.flags &= ~(PURR_CFGF_IGNORE_ONCE | PURR_CFGF_SECURE_OFF_ONCE);
    s_ignore_once = ignore_flag && trust->dev;
    bool secure_off_once = off_flag && trust->dev;
    s_mode = secure_off_once ? PURR_SECURE_OFF : trust->mode;
    s_bs = &bs;
    s_cfg = purr_bootcfg_view();
    {
        static const char *const names[] = {"off", "warn", "enforce"};
        ESP_LOGI(TAG, "secure_mode: %s%s%s%s", s_mode < 3 ? names[s_mode] : "?",
                 trust->dev ? " (DEVELOPMENT MODE)" : "",
                 trust->signature_ok ? " (signed trust block)" : "",
                 secure_off_once ? " (SECURE_OFF_ONCE)" : "");
    }

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
        if (purr_slot_usable(s->index)) {
            boot_index = s->index;
            break;
        }
    }
    if (boot_index == INVALID_INDEX) {
        ESP_LOGE(TAG, "no bootable app found");
        bootloader_reset();
    }

    bool ladder_chose_loader = (boot_index == TEST_APP_INDEX && tier == k_tier2);
    bool menu_chose = false;               /* the boot menu picked the slot */

    /* A recovery request (the shell's "reboot recovery") skips the menu and starts KittenOS
     * once. "reboot loader" does the same for the recovery loader (the "test" slot,
     * RecoveryLoader/SPEC.md). If the target is not there, fall through to the menu as usual.
     * Either one is a deliberate, explicit request -- it overrides the ladder's pick outright,
     * and never counts as the silent auto path below, even if the ladder happened to agree. */
    if (purr_bootcfg_take_flag(PURR_CFGF_FORCE_RECOVERY) && bs.factory.size != 0) {
        ESP_LOGI(TAG, "recovery requested: starting KittenOS");
        boot_index = FACTORY_INDEX;
        ladder_chose_loader = false;
        flags_used |= PURR_CFGF_FORCE_RECOVERY;
    } else if (purr_bootcfg_take_flag(PURR_CFGF_FORCE_LOADER) && bs.test.size != 0) {
        ESP_LOGI(TAG, "internet recovery requested: starting the recovery loader");
        boot_index = TEST_APP_INDEX;
        ladder_chose_loader = false;
        flags_used |= PURR_CFGF_FORCE_LOADER;
    } else if ((cfg.flags & PURR_CFGF_CONTINUE_INSTALL) != 0 && bs.factory.size != 0 &&
               purr_slot_usable(FACTORY_INDEX)) {
        /* Peeked, not taken: commands.c's purr_continue_install() is the one that reads and
         * clears this flag, once KittenOS actually gets to run (purr_abi.h's own comment on
         * it). Without this override, the tier0 ladder above still prefers the kernel slot
         * whenever boot_fail_count is low -- exactly the state mark_install_success() just
         * put it in -- and restore_components()/run_auto_mode() installs kernel.cat *before*
         * kittenos.kitt in that same run, so kernel now looks bootable too. Found for real,
         * 2026-10: the device landed on the kernel slot's full-profile build instead of
         * KittenOS right after a fresh recovery install, which never auto-formats a blank
         * filesystem the way the recovery profile does -- half-installed, no accounts, no way
         * to log in. */
        ESP_LOGI(TAG, "continue-install pending: starting KittenOS to finish the install");
        boot_index = FACTORY_INDEX;
        ladder_chose_loader = false;
    } else if (ladder_chose_loader) {
        /* Skip the menu entirely, the same as an explicit FORCE_LOADER request above: nobody
         * may be there to answer one, and purr_bootpkg_run()'s menu has no way to offer the
         * loader as a normal preselected choice anyway -- it only reaches the loader through
         * its own separate "internet recovery" menu action, never through `preferred`. boot_index
         * is already TEST_APP_INDEX from the ladder above. */
        ESP_LOGW(TAG, "repeated boot failure: escalating straight to the recovery loader");
    } else {
        /* The boot package shows a screen only when one is needed (bootloader/SPEC.md section 9): a normal start is a splash and a jump, a Fast startup
         * start is not even that. The menu comes up when Space is held, when the last start never reached a healthy boot, when the slot that should
         * start does not pass its check, or when Settings asked for it. Verification is never skipped. */
        uint32_t why = 0;
        int fail_slot = -1;
        if (purr_bootcfg_take_flag(PURR_CFGF_SHOW_MENU)) {
            why = PURR_MENU_WHY_REQUESTED;
        } else if (fail_count > 1) {
            why = PURR_MENU_WHY_LAST_FAILED;
        } else if (tier == k_tier0 && purr_looks_bootable(slot_pos(tier[0].index)) && !purr_slot_usable(tier[0].index)) {
            why = PURR_MENU_WHY_NO_VERIFY;                 /* the kernel is there but fails its check: the ladder quietly moved on, the person should know */
            fail_slot = tier[0].index;
        }
        bool fast = (cfg.flags & PURR_CFGF_FAST_START) != 0;
        int chosen = boot_index;
        int r = purr_bootpkg_start(&bs, boot_index, fast, why, fail_slot, &chosen, purr_slot_usable);
        if (r == 2) {
            ESP_LOGI(TAG, "restart chosen in the boot menu");
            bootloader_reset();
        } else if (r == 1) {
            boot_index = chosen;
            menu_chose = true;
        }
    }

    /* Final gate on the slot actually being launched, whichever path chose it: the ladder, a
     * one-shot request, the continue-install override or the boot menu. An explicit request
     * must not be a way around enforce mode. If the choice is not usable, take the first
     * usable slot in the ladder's order; if there is none, there is nothing safe to run. */
    if (!purr_slot_usable(boot_index)) {
        ESP_LOGE(TAG, "slot %d may not be launched in this secure mode", boot_index);
        boot_index = INVALID_INDEX;
        ladder_chose_loader = false;
        for (int i = 0; i < 3; i++) {
            if (purr_slot_usable(tier[i].index)) {
                boot_index = tier[i].index;
                break;
            }
        }
        if (boot_index == INVALID_INDEX) {
            /* bootloader/SPEC.md section 6 step 7: whether this waits at a serial prompt, halts
             * or resets is an open decision. Today it resets, which keeps climbing
             * boot_fail_count and so ends up at the recovery loader's tier. */
            ESP_LOGE(TAG, "no slot passes verification; resetting");
            bootloader_reset();
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

    /* The handoff (bootloader/SPEC.md section 7): what CoreOS reads first to learn why it is
     * running. The chosen slot's signature trailer is checked here for the handoff even when
     * the policy would have let it boot regardless. In off mode nothing is verified, so the
     * state stays UNCHECKED. */
    uint8_t state = PURR_BOOT_UNCHECKED;
    uint8_t key_id = 0xFF;
    bool verified = false, failed = false;
    if (s_mode != PURR_SECURE_OFF) {
        const purr_slot_verdict_t *v = slot_verdict(boot_index);
        verified = v->verified;
        failed = !verified;
        if (verified) {
            key_id = v->key_id;
        } else {
            ESP_LOGW(TAG, "booting slot %d although it did not verify (%s), secure_mode %s",
                     boot_index, purr_verify_name(v->result),
                     s_mode == PURR_SECURE_ENFORCE ? "enforce + IGNORE_ONCE" : "warn");
        }
    }
    if (failed) {
        state = PURR_BOOT_FAILED_WARNED;
    } else if (boot_index == TEST_APP_INDEX && ladder_chose_loader) {
        state = PURR_BOOT_AUTO_REINSTALL;
    } else if (flags_used & (PURR_CFGF_FORCE_RECOVERY | PURR_CFGF_FORCE_LOADER)) {
        state = PURR_BOOT_FORCED_RECOVERY;
    } else if (verified) {
        state = PURR_BOOT_VERIFIED;
    } else if (!have_cfg) {
        state = PURR_BOOT_CONFIG_DEFAULT;
    }
    bool devrec = trust->dev && (cfg.flags & PURR_CFGF_DEV_RECOVERY) != 0;   /* only ever in development mode */
    uint8_t secure_state = (uint8_t)((s_mode & 3u) | (trust->dev ? PURR_SECST_DEV : 0u) |
                                     (devrec ? PURR_SECST_DEVREC : 0u) |
                                     (trust->signature_ok ? PURR_SECST_SIGNED : 0u));
    uint16_t reasons = 0;
    if (boot_index == TEST_APP_INDEX) {      /* the loader is starting: tell it why (its first page lists them) */
        bool requested = (flags_used & PURR_CFGF_FORCE_LOADER) != 0 || menu_chose;
        reasons = recovery_reasons(&cfg, have_cfg, ladder_chose_loader, requested);
        ESP_LOGI(TAG, "recovery reasons: 0x%x", (unsigned)reasons);
    }
    purr_handoff_write(state, key_id, flags_used, secure_state, reasons);

    ESP_LOGI(TAG, "booting app slot %d", boot_index);
    bootloader_utility_load_boot_image(&bs, boot_index);
}
