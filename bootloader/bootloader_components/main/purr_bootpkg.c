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
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
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

static void svc_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
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

static const purr_boot_services_t s_services = {
    .version = PURR_PKG_SERVICES_VERSION,
    .log = svc_log,
    .delay_us = svc_delay_us,
    .gpio_setup = svc_gpio_setup,
};

/* ------------------------------------------------------------ finding it */

bool purr_find_partition(const char *name, uint32_t *offset, uint32_t *size)
{
    const esp_partition_info_t *table = bootloader_mmap(ESP_PARTITION_TABLE_OFFSET,
                                                        ESP_PARTITION_TABLE_MAX_LEN);
    if (table == NULL) {
        return false;
    }
    bool found = false;
    for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {
        if (table[i].magic != ESP_PARTITION_MAGIC) {
            break;
        }
        if (table[i].type == PART_TYPE_DATA && table[i].subtype == PKG_PART_SUBTYPE &&
            strncmp((const char *)table[i].label, name, sizeof(table[i].label)) == 0) {
            *offset = table[i].pos.offset;
            *size = table[i].pos.size;
            found = true;
            break;
        }
    }
    bootloader_munmap(table);
    return found;
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

    const uint8_t *base = bootloader_mmap(off, part_size);
    if (base == NULL) {
        ESP_LOGW(TAG, "bootpkg: cannot read flash");
        return 0;
    }

    purr_cfg_t cfg;
    purr_bootcfg_load(&cfg);      /* defaults (secure_mode = warn) if unreadable */

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

static uint8_t looks_bootable(const esp_partition_pos_t *pos)
{
    uint32_t first = 0;   /* the flash reader takes whole words */
    if (pos->size == 0) {
        return 0;
    }
    return bootloader_flash_read(pos->offset, &first, sizeof(first), false) == ESP_OK &&
           (first & 0xFF) == 0xE9;   /* an ESP app image */
}

bool purr_bootpkg_run(const bootloader_state_t *bs, int preferred, int *boot_index)
{
    uint32_t entry = load_package();
    if (entry == 0) {
        return false;
    }

    /* The menu's list: the PURR OS slots first, then KittenOS (the factory slot). */
    purr_boot_part_t parts[PURR_PKG_MAX_PARTS];
    int map[PURR_PKG_MAX_PARTS];          /* menu index -> the bootloader's slot index */
    int n = 0, menu_preferred = -1;
    for (int i = 0; i < bs->app_count && n < PURR_PKG_MAX_PARTS - 1; i++) {
        char name[16] = "PURR OS ota_0";   /* no snprintf: it drags newlib's printf in */
        name[12] = (char)('0' + (i % 10));
        set_name(&parts[n], name);
        parts[n].bootable = looks_bootable(&bs->ota[i]);
        map[n] = i;
        if (i == preferred) {
            menu_preferred = n;
        }
        n++;
    }
    if (bs->factory.size != 0 && n < PURR_PKG_MAX_PARTS) {
        set_name(&parts[n], "KittenOS");
        parts[n].bootable = looks_bootable(&bs->factory);
        map[n] = FACTORY_INDEX;
        if (preferred == FACTORY_INDEX) {
            menu_preferred = n;
        }
        n++;
    }

    ESP_LOGI(TAG, "starting the boot menu");
    flush_log();
    int choice = ((purr_pkg_entry_fn)entry)(&s_services, parts, n, menu_preferred);
    if (choice < 0 || choice >= n) {
        return false;
    }
    *boot_index = map[choice];
    return true;
}
