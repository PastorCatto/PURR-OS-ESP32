// Loads and runs the boot package (bootloader/SPEC.md section 9).
//
// The package is a PURR image in its own raw partition, named "bootpkg". This checks
// it (magic, chip, type, size, SHA-256 of the payload), copies it into the reserved
// RAM window and calls it. The signature is not checked yet: that needs the key bag
// and purrcfg, so for now the package is accepted unsigned, as the spec allows while
// secure mode is off.
#include <stdint.h>
#include <string.h>

#include "bootloader_flash_priv.h"
#include "bootloader_sha.h"
#include "bootloader_utility.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"

#include "purr_abi.h"
#include "purr_bootpkg.h"

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

static bool find_partition(uint32_t *offset, uint32_t *size)
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
            strncmp((const char *)table[i].label, PKG_PART_NAME, sizeof(table[i].label)) == 0) {
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

// Returns the entry address, or 0 with a reason logged.
static uint32_t load_package(void)
{
#if !CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGW(TAG, "bootpkg: no RAM window defined for this chip yet");
    return 0;
#else
    uint32_t off = 0, part_size = 0;
    if (!find_partition(&off, &part_size)) {
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

    uint32_t entry = 0;
    purr_image_header_t h;
    memcpy(&h, base, sizeof(h));
    const char *why = NULL;

    if (h.magic != PURR_IMAGE_MAGIC) {
        why = "no PURR image (partition empty?)";
    } else if (h.header_version != PURR_IMAGE_HEADER_VERSION || h.header_size < sizeof(h)) {
        why = "unknown header version";
    } else if (h.chip_id != THIS_CHIP) {
        why = "built for another chip";
    } else if (h.image_type != PURR_IMG_MODULE || PURR_FLAGS_SUBTYPE(h.flags) != PURR_MOD_BOOTPKG) {
        why = "not a boot package";
    } else if (h.payload_size < sizeof(purr_pkg_preamble_t) || (h.payload_size % 4) != 0 ||
               h.payload_offset > part_size || h.payload_size > part_size - h.payload_offset) {
        why = "payload size out of range or not a multiple of 4";
    } else {
        const uint8_t *payload = base + h.payload_offset;
        purr_pkg_preamble_t p;
        memcpy(&p, payload, sizeof(p));
        uint8_t digest[PURR_SHA256_LEN];
        bootloader_sha256_handle_t sha = bootloader_sha256_start();
        bootloader_sha256_data(sha, payload, h.payload_size);
        bootloader_sha256_finish(sha, digest);

        if (memcmp(digest, h.payload_sha256, sizeof(digest)) != 0) {
            why = "payload hash does not match";
        } else if (p.text_size > PURR_PKG_S3_TEXT_MAX ||
                   p.data_size > PURR_PKG_S3_DATA_MAX ||
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
            ESP_LOGI(TAG, "bootpkg %.12s loaded: code %u, data %u, bss %u bytes",
                     h.version, (unsigned)p.text_size, (unsigned)p.data_size, (unsigned)p.bss_size);
        }
    }
    bootloader_munmap(base);
    if (why) {
        ESP_LOGW(TAG, "bootpkg: %s", why);
    }
    return entry;
#endif
}

int purr_bootpkg_run(const bootloader_state_t *bs, int preferred)
{
    ESP_LOGI(TAG, "bootpkg: looking");
    flush_log();
    uint32_t entry = load_package();
    flush_log();
    if (entry == 0) {
        return -1;
    }

    purr_boot_part_t parts[PURR_PKG_MAX_PARTS];
    int n = bs->app_count < PURR_PKG_MAX_PARTS ? bs->app_count : PURR_PKG_MAX_PARTS;
    for (int i = 0; i < n; i++) {
        memset(&parts[i], 0, sizeof(parts[i]));
        strcpy(parts[i].name, "ota_");
        parts[i].name[4] = (char)('0' + (i % 10));
        uint32_t first = 0;   /* the flash reader takes whole words */
        parts[i].bootable = bootloader_flash_read(bs->ota[i].offset, &first, sizeof(first), false) == ESP_OK &&
                            (first & 0xFF) == 0xE9;   /* an ESP app image */
    }

    ESP_LOGI(TAG, "starting the boot menu");
    flush_log();
    int choice = ((purr_pkg_entry_fn)entry)(&s_services, parts, n, preferred);
    if (choice < 0 || choice >= n) {
        return -1;
    }
    return choice;
}
