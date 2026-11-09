// Loads and runs the boot package (bootloader/SPEC.md section 9).
//
// The package is a PURR image in its own raw partition, named "bootpkg". This verifies it
// (magic, chip, type, layout, payload hash, and now the ECDSA P-256 signature against the
// key bag) then copies it into the reserved RAM window and calls it. An unsigned or
// unverified package is only accepted while secure_mode is off, per bootloader/SPEC.md
// section 9.
#include <stdint.h>
#include <string.h>

#include "bootloader_flash_priv.h"
#include "bootloader_utility.h"
#include "esp_flash_partitions.h"
/* Our own lines stay at INFO even though the rest of the bootloader logs at WARN. */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "hal/wdt_hal.h"
#include "soc/gpio_sig_map.h"

#include "purr_abi.h"
#include "purr_bootcfg.h"
#include "purr_bootpkg.h"
#include "purr_crypto_uecc.h"
#include "purr_keybag.h"
#include "purr_verify.h"

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

static const char *TAG = "purr_boot";

/* The console is USB, whose FIFO only drains when the host polls. Wait a moment, so a
 * crash right after a line does not lose it. */
static void flush_log(void)
{
    esp_rom_delay_us(30000);
}

#define PKG_PART_NAME    "bootpkg"
#define PKG_PART_SUBTYPE 0x40          /* a custom raw data partition */

#if CONFIG_IDF_TARGET_ESP32S3
#define THIS_CHIP PURR_CHIP_ESP32S3
#else
#define THIS_CHIP PURR_CHIP_ESP32
#endif

/* ------------------------------------------------------------ services */

static void svc_log(const char *line)
{
    ESP_LOGI(TAG, "%s", line);
    flush_log();
}

/* The bootloader arms the RTC watchdog when it starts (CONFIG_BOOTLOADER_WDT_TIME_MS, 9 s) and nothing fed it, so the boot menu waiting for a key was reset
 * by the watchdog about 5 s after it appeared (the verification and the splash had already used the first few seconds): the device restarted instead of
 * falling through to its default after the menu's own 20 s idle time, and did that in a loop. The package waits through delay_us, so that is where the
 * watchdog is fed, every half second at most. It is fed only for a bounded total (90 s of waiting, far more than a menu and its 20 s fall-through need):
 * a package that hangs in its wait loop is still reset, as the watchdog is there to do. */
#define WAIT_CHUNK_US   500000u
#define WAIT_FEED_MAX_US (90ull * 1000000ull)
static uint64_t s_waited_us;

static void wdt_feed(void)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_feed(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);
}

static void svc_delay_us(uint32_t us)
{
    while (us > 0) {
        uint32_t chunk = us > WAIT_CHUNK_US ? WAIT_CHUNK_US : us;
        if (s_waited_us < WAIT_FEED_MAX_US) {
            wdt_feed();
        }
        esp_rom_delay_us(chunk);
        s_waited_us += chunk;
        us -= chunk;
    }
}

static void svc_gpio_setup(int pin, int mode)
{
    esp_rom_gpio_pad_select_gpio(pin);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_set_level(&GPIO, pin, 0);
    if (mode == PURR_GPIO_OUT) {
        gpio_ll_output_enable(&GPIO, pin);
    } else {
        gpio_ll_output_disable(&GPIO, pin);
        gpio_ll_input_enable(&GPIO, pin);
        gpio_ll_pullup_en(&GPIO, pin);
    }
}

static purr_boot_services_t s_services = {
    .version = PURR_PKG_SERVICES_VERSION,
    .log = svc_log,
    .delay_us = svc_delay_us,
    .gpio_setup = svc_gpio_setup,
};

/* ------------------------------------------------------------ finding it */

/* The partition table is read once into RAM with the plain flash reader, never through
 * bootloader_mmap(). The bootloader stage only ever allows one mmap at a time ("tried to
 * bootloader_mmap twice", found on real hardware 2026-09-29): this used to map the table on
 * every lookup, so any lookup made while load_package() held its own mapping of the boot
 * package would crash, and whether one did depended on call order. With no mapping here,
 * a lookup is safe from anywhere, in any order, any number of times. */
static uint32_t s_table[ESP_PARTITION_TABLE_MAX_LEN / sizeof(uint32_t)];   /* word aligned */
static bool s_table_loaded;

bool purr_find_partition(const char *name, uint32_t *offset, uint32_t *size)
{
    if (!s_table_loaded) {
        if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET, s_table, sizeof(s_table), false) != ESP_OK) {
            return false;
        }
        s_table_loaded = true;
    }
    const esp_partition_info_t *table = (const esp_partition_info_t *)s_table;
    for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {
        if (table[i].magic != ESP_PARTITION_MAGIC) {
            break;
        }
        if (table[i].type == PART_TYPE_DATA && table[i].subtype == PKG_PART_SUBTYPE &&
            strncmp((const char *)table[i].label, name, sizeof(table[i].label)) == 0) {
            *offset = table[i].pos.offset;
            *size = table[i].pos.size;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ loading it */

typedef struct {
    const uint8_t *base;
    uint32_t size;
} mmap_read_ctx_t;

static int mmap_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    mmap_read_ctx_t *m = ctx;
    if ((uint64_t)offset + len > m->size) {
        return -1;
    }
    memcpy(buf, m->base + offset, len);
    return 0;
}

/* Whether the loaded package understands progress calls (purr_abi.h): its version is 0.2 or later. A package
 * from before that would read a negative nparts as a count, so it is never sent one. */
static bool s_pkg_progress;
static bool s_pkg_v4;               /* 0.3 or later: it understands the splash, the key check and the menu reasons (services version 4) */

static bool version_at_least_0_2(const char *v)
{
    int major = 0, minor = 0, i = 0;
    while (v[i] >= '0' && v[i] <= '9') major = major * 10 + (v[i++] - '0');
    if (v[i] == '.') {
        i++;
        while (v[i] >= '0' && v[i] <= '9') minor = minor * 10 + (v[i++] - '0');
    }
    return major > 0 || minor >= 2;
}

static bool version_at_least(const char *v, int want_major, int want_minor)
{
    int major = 0, minor = 0, i = 0;
    while (v[i] >= '0' && v[i] <= '9') major = major * 10 + (v[i++] - '0');
    if (v[i] == '.') {
        i++;
        while (v[i] >= '0' && v[i] <= '9') minor = minor * 10 + (v[i++] - '0');
    }
    return major > want_major || (major == want_major && minor >= want_minor);
}

// Returns the entry address, or 0 with a reason logged.
static uint32_t load_package(void)
{
#if !CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGW(TAG, "bootpkg: no RAM window defined for this chip yet");
    return 0;
#else
    uint32_t off = 0, part_size = 0;
    if (!purr_find_partition(PKG_PART_NAME, &off, &part_size)) {
        ESP_LOGW(TAG, "bootpkg: no \"%s\" partition", PKG_PART_NAME);
        return 0;
    }
    if (part_size < sizeof(purr_image_header_t) + sizeof(purr_pkg_preamble_t)) {
        ESP_LOGW(TAG, "bootpkg: partition too small");
        return 0;
    }

    /* The effective state (trust block checked once, development mode applied): bootloader/
     * SPEC.md section 5. secure_mode here is the effective mode, the key bag only holds
     * override keys a valid signature vouched for. */
    const purr_cfg_t *cfg_view = purr_bootcfg_view();
    purr_cfg_t cfg = *cfg_view;

    /* The only mapping held anywhere in the bootloader: nothing below may map again. */
    const uint8_t *base = bootloader_mmap(off, part_size);
    if (base == NULL) {
        ESP_LOGW(TAG, "bootpkg: cannot read flash");
        return 0;
    }

    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);

    purr_verify_env_t env = {
        .chip_id = THIS_CHIP,
        .bag = &bag,
        .crypto = &purr_crypto_uecc,
        .bootloader_version = 0,  /* the bootloader has no version scheme yet */
        .version_floor = 0,
        .enforce_floor = 0,
    };
    mmap_read_ctx_t rdctx = {base, part_size};
    purr_image_header_t h;
    purr_verify_result_t vr = purr_image_verify(&env, mmap_read, &rdctx, part_size, &h);

    uint32_t entry = 0;
    const char *why = NULL;

    /* purr_image_verify checks the signature is valid for whatever subtype the header
     * claims; it does not know this partition is only ever supposed to hold a boot
     * package. That check is ours, and it applies whether or not the image verified. */
    if (h.magic != PURR_IMAGE_MAGIC) {
        why = "no PURR image (partition empty?)";
    } else if (h.image_type != PURR_IMG_MODULE || PURR_FLAGS_SUBTYPE(h.flags) != PURR_MOD_BOOTPKG) {
        why = "not a boot package";
    } else if (vr != PURR_V_OK && cfg.secure_mode != PURR_SECURE_OFF) {
        why = purr_verify_name(vr);
        ESP_LOGW(TAG, "bootpkg: rejected (%s), secure mode is %s", why,
                 cfg.secure_mode == PURR_SECURE_WARN ? "warn" : "enforce");
    } else {
        if (vr != PURR_V_OK) {
            ESP_LOGW(TAG, "bootpkg: unverified (%s), accepted because secure mode is off",
                     purr_verify_name(vr));
        }
        const uint8_t *payload = base + h.payload_offset;
        purr_pkg_preamble_t p;
        memcpy(&p, payload, sizeof(p));
        if (h.payload_size < sizeof(p)) {
            why = "payload smaller than its own preamble";
        } else if (p.text_size > PURR_PKG_S3_TEXT_MAX || p.data_size > PURR_PKG_S3_DATA_MAX ||
                   p.bss_size > PURR_PKG_S3_DATA_MAX - p.data_size ||
                   sizeof(p) + (uint64_t)p.text_size + p.data_size > h.payload_size) {
            why = "does not fit its RAM window";
        } else if (p.entry < PURR_PKG_S3_TEXT_RUN || p.entry >= PURR_PKG_S3_TEXT_RUN + p.text_size) {
            why = "entry outside the code";
        } else {
            memcpy((void *)PURR_PKG_S3_TEXT_WRITE, payload + sizeof(p), p.text_size);
            memcpy((void *)PURR_PKG_S3_DATA, payload + sizeof(p) + p.text_size, p.data_size);
            memset((void *)(PURR_PKG_S3_DATA + p.data_size), 0, p.bss_size);
            __asm__ __volatile__("memw; isync");
            entry = p.entry;
            s_pkg_progress = version_at_least_0_2(h.version);
            s_pkg_v4 = version_at_least(h.version, 0, 3);
            ESP_LOGI(TAG, "bootpkg %.12s loaded (key id %u, %s): code %u, data %u, bss %u bytes",
                     h.version, (unsigned)h.key_id, purr_verify_name(vr),
                     (unsigned)p.text_size, (unsigned)p.data_size, (unsigned)p.bss_size);
        }
    }
    bootloader_munmap(base);
    if (why) {
        ESP_LOGW(TAG, "bootpkg: %s", why);
    }
    return entry;
#endif
}

static void set_name(purr_boot_part_t *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    for (size_t i = 0; i < sizeof(p->name) - 1 && name[i]; i++) {
        p->name[i] = name[i];
    }
}

uint8_t purr_looks_bootable(const esp_partition_pos_t *pos)
{
    uint32_t first = 0;   /* the flash reader takes whole words */
    if (pos->size == 0) {
        return 0;
    }
    return bootloader_flash_read(pos->offset, &first, sizeof(first), false) == ESP_OK &&
           (first & 0xFF) == 0xE9;   /* an ESP app image */
}

/* Tells the package which slot is being verified and how far along (thousandths), so it can draw it. */
static void stage(uint32_t entry, const char *label, int done, int total)
{
    if (s_pkg_progress) {
        ((purr_pkg_entry_fn)entry)(&s_services, (const purr_boot_part_t *)label, -PURR_PKG_STAGE_VERIFY,
                                   total > 0 ? done * 1000 / total : 0);
    }
}

static int pkg_stage(uint32_t entry, int stage, const char *text, int arg)
{
    return ((purr_pkg_entry_fn)entry)(&s_services, (const purr_boot_part_t *)text, -stage, arg);
}

/* Lists the slots (verifying each), runs the package's menu, and maps its answer. Returns 1 with *boot_index set, 2 for "restart", 0 if it
 * could not be shown. */
static int run_menu(uint32_t entry, const bootloader_state_t *bs, int preferred, int *boot_index, bool (*usable)(int index),
                    uint32_t reason, const char *detail1, const char *detail2, bool show_progress)
{
    int total = bs->app_count + (bs->factory.size != 0 ? 1 : 0), done = 0;

    /* The menu's list: the kernel slot first (PurrOS/SPEC.md's fallback chain), then KittenOS
     * (the factory slot). Only ever one kernel-style slot now -- the whole point of
     * consolidating the old ota_0/ota_1 pair into a single named "kernel" partition -- but
     * the loop stays general in case that ever changes. */
    purr_boot_part_t parts[PURR_PKG_MAX_PARTS];
    int map[PURR_PKG_MAX_PARTS];          /* menu index -> the bootloader's slot index */
    int n = 0, menu_preferred = -1;
    for (int i = 0; i < bs->app_count && n < PURR_PKG_MAX_PARTS - 1; i++) {
        char name[16] = "Kernel";   /* no snprintf: it drags newlib's printf in */
        if (i > 0) {
            name[6] = ' ';
            name[7] = (char)('0' + (i % 10));
            name[8] = '\0';
        }
        set_name(&parts[n], name);
        if (show_progress) {
            stage(entry, name, done++, total);
        }
        parts[n].bootable = usable(i);
        map[n] = i;
        if (i == preferred) {
            menu_preferred = n;
        }
        n++;
    }
    if (bs->factory.size != 0 && n < PURR_PKG_MAX_PARTS) {
        set_name(&parts[n], "KittenOS");
        if (show_progress) {
            stage(entry, "KittenOS", done++, total);
        }
        parts[n].bootable = usable(FACTORY_INDEX);
        map[n] = FACTORY_INDEX;
        if (preferred == FACTORY_INDEX) {
            menu_preferred = n;
        }
        n++;
    }

    if (show_progress) {
        stage(entry, "slots", total, total);               /* the bar is full before the menu takes over */
    }
    s_services.menu_reason = reason;
    s_services.menu_idle_ms = 20000;
    s_services.detail1 = detail1;
    s_services.detail2 = detail2;
    ESP_LOGI(TAG, "starting the boot menu (reason %u)", (unsigned)reason);
    flush_log();
    int choice = ((purr_pkg_entry_fn)entry)(&s_services, parts, n, menu_preferred);
    if (choice == PURR_PKG_CHOICE_RESTART) {
        return 2;
    }
    if (choice == PURR_PKG_CHOICE_INTERNET_RECOVERY) {
        if (bs->test.size == 0) {
            ESP_LOGW(TAG, "internet recovery requested, but there is no recovery loader");
            return 0;
        }
        ESP_LOGI(TAG, "internet recovery: starting the recovery loader");
        *boot_index = TEST_APP_INDEX;
        return 1;
    }
    if (choice < 0 || choice >= n) {
        return 0;
    }
    *boot_index = map[choice];
    return 1;
}

static void set_secure_state(void)
{
    const purr_trust_t *t = purr_bootcfg_trust();          /* the package's status chips read this */
    s_services.secure_state = (t->mode & 3u) | (t->dev ? PURR_SECST_DEV : 0u) | (t->signature_ok ? PURR_SECST_SIGNED : 0u);
}

bool purr_bootpkg_run(const bootloader_state_t *bs, int preferred, int *boot_index,
                      bool (*usable)(int index))
{
    uint32_t entry = load_package();
    if (entry == 0) {
        return false;
    }
    set_secure_state();
    return run_menu(entry, bs, preferred, boot_index, usable, PURR_MENU_WHY_KEY, NULL, NULL, true) == 1;
}

int purr_bootpkg_start(const bootloader_state_t *bs, int preferred, bool fast, uint32_t why, int fail_slot,
                       int *boot_index, bool (*usable)(int index))
{
    uint32_t entry = 0;
    bool shown = false;
    if (!fast || why != 0) {
        entry = load_package();
        if (entry == 0) {
            return -1;                                      /* no usable package: boot what the ladder chose, no screen */
        }
        set_secure_state();
        if (!s_pkg_v4) {                                    /* an older package knows only the old menu: show it as before */
            int r = run_menu(entry, bs, preferred, boot_index, usable, PURR_MENU_WHY_KEY, NULL, NULL, true);
            return r == 1 ? 1 : (r == 2 ? 2 : 0);
        }
        pkg_stage(entry, PURR_PKG_STAGE_SPLASH, preferred == FACTORY_INDEX ? "Starting KittenOS" : "Starting PURR OS", 0);
        shown = true;
    }

    /* The check of the slot that would start. On a fast start this is the only thing that happens; the signature check is never skipped. */
    bool ok = usable(preferred);
    if (why == 0 && !ok) {
        why = PURR_MENU_WHY_NO_VERIFY;
    }
    if (why == 0 && shown) {
        if (pkg_stage(entry, PURR_PKG_STAGE_KEYCHECK, "", 300) == 1) {
            why = PURR_MENU_WHY_KEY;
        }
    }
    if (why == 0) {
        *boot_index = preferred;
        return 0;
    }

    if (entry == 0) {                                       /* a fast start that ran into trouble: the screens come back for this one */
        entry = load_package();
        if (entry == 0) {
            return -1;
        }
        set_secure_state();
        if (!s_pkg_v4) {
            int r = run_menu(entry, bs, preferred, boot_index, usable, PURR_MENU_WHY_KEY, NULL, NULL, true);
            return r == 1 ? 1 : (r == 2 ? 2 : 0);
        }
    }
    const char *d1 = NULL, *d2 = NULL;
    if (why == PURR_MENU_WHY_NO_VERIFY) {
        purr_slot_failure_text(fail_slot >= 0 ? fail_slot : preferred, &d1, &d2);
    }
    int r = run_menu(entry, bs, preferred, boot_index, usable, why, d1, d2, false);
    return r == 1 ? 1 : (r == 2 ? 2 : 0);
}
