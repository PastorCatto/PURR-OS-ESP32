/*
 * purr_abi.h - structures and constants shared by the bootloader, the boot
 * package, CoreOS and the tools.
 *
 * Plain C, standard integer types only, no ESP-IDF includes. Little-endian,
 * packed, fixed size. Every struct has a version and a size assertion.
 *
 * Behaviour is specified in bootloader/SPEC.md (container, purrcfg, handoff)
 * and PurrOS/components/coreos/SPEC.md. This file is the authority for layout.
 */
#ifndef PURR_ABI_H
#define PURR_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_PACKED __attribute__((packed))
#define PURR_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* ------------------------------------------------------------------------- */
/* Image container (bootloader/SPEC.md section 3)                            */
/* ------------------------------------------------------------------------- */

#define PURR_IMAGE_MAGIC           0x50555252u  /* 'PURR' */
#define PURR_IMAGE_HEADER_VERSION  1

#define PURR_CHIP_ESP32            0     /* esp_chip_id_t values */
#define PURR_CHIP_ESP32S3          9
#define PURR_CHIP_ANY              0xFFFF /* apps carry their own payload table */

/* image_type */
#define PURR_IMG_OS                1     /* PURR OS */
#define PURR_IMG_RECOVERY          2     /* KittenOS */
#define PURR_IMG_MODULE            3     /* system module (.kitt), see subtype */
#define PURR_IMG_APP               4     /* app image (.cat) */

/* Low 8 bits of the header flags hold the subtype of a system module. */
#define PURR_FLAGS_SUBTYPE(flags)  ((flags) & 0xFFu)

#define PURR_MOD_KERNEL            1
#define PURR_MOD_COREOS            2
#define PURR_MOD_APPMANAGER        3
#define PURR_MOD_RUNTIME           4
#define PURR_MOD_DRIVER            5
#define PURR_MOD_BOOTPKG           6
#define PURR_MOD_DEVBUNDLE         7
#define PURR_MOD_LOADER            8     /* recovery loader */

/* Key roles (Keys/SPEC.md section 2) */
#define PURR_ROLE_NONE             0
#define PURR_ROLE_BOOT             1
#define PURR_ROLE_SYSTEM           2
#define PURR_ROLE_OWNER            3
#define PURR_ROLE_DEVELOPER        4
#define PURR_ROLE_VENDOR           5

#define PURR_NAME_LEN              32
#define PURR_VERSION_LEN           12
#define PURR_SHA256_LEN            32
#define PURR_PUBKEY_LEN            64    /* raw P-256 X || Y */
#define PURR_SIG_LEN               64    /* raw P-256 r || s */

typedef struct PURR_PACKED {
    uint32_t magic;                           /* PURR_IMAGE_MAGIC */
    uint8_t  header_version;                  /* PURR_IMAGE_HEADER_VERSION */
    uint16_t header_size;                     /* bytes, lets the header grow */
    uint16_t chip_id;                         /* PURR_CHIP_* */
    uint8_t  image_type;                      /* PURR_IMG_* */
    uint8_t  key_id;                          /* which key in the key bag signed it */
    uint16_t flags;                           /* low byte: module subtype */
    char     name[PURR_NAME_LEN];
    char     version[PURR_VERSION_LEN];       /* "1.2.3" or "1.0.0-dp10" */
    char     min_boot_version[PURR_VERSION_LEN];
    uint32_t payload_offset;                  /* from the start of the image */
    uint32_t payload_size;
    uint8_t  payload_sha256[PURR_SHA256_LEN];
    uint8_t  signature[PURR_SIG_LEN];         /* ECDSA P-256 over the bytes before it */
} purr_image_header_t;

PURR_STATIC_ASSERT(sizeof(purr_image_header_t) == 173, "image header layout changed");

/* The signature covers every header byte before the signature field. */
#define PURR_IMAGE_SIGNED_LEN  offsetof(purr_image_header_t, signature)

/* ------------------------------------------------------------------------- */
/* purrcfg (bootloader/SPEC.md section 5)                                    */
/* ------------------------------------------------------------------------- */

#define PURR_CFG_MAGIC             0x47464350u  /* 'PCFG' */
#define PURR_CFG_VERSION           1
#define PURR_CFG_SECTOR_SIZE       4096
#define PURR_CFG_SECTORS           2            /* A/B copies */

#define PURR_SECURE_OFF            0
#define PURR_SECURE_WARN           1
#define PURR_SECURE_ENFORCE        2

/* flags: one-shot unless noted, cleared by the bootloader before handoff */
#define PURR_CFGF_IGNORE_ONCE      (1u << 0)
#define PURR_CFGF_UPDATE_KEY       (1u << 1)
#define PURR_CFGF_SECURE_OFF_ONCE  (1u << 2)
#define PURR_CFGF_FORCE_RECOVERY   (1u << 3)
#define PURR_CFGF_FORCE_LOADER     (1u << 4)
/* The opposite direction from the flags above: set by the bootloader itself (bootloader/SPEC.md
 * section 6's boot_fail_count ladder), not by a request the OS made, when it chose the loader
 * because the failure count crossed the ladder's second threshold rather than because of
 * FORCE_LOADER or a human picking it from the menu. Read and cleared by the recovery loader
 * (RecoveryLoader/SPEC.md section 2.1), never by the bootloader. */
#define PURR_CFGF_AUTO_REINSTALL   (1u << 5)
/* Set by the recovery loader (never the bootloader) right before it restarts into a
 * freshly-installed `kernel`/`kittenos`, alongside resetting `boot_fail_count` to 0 --
 * otherwise the ladder's own count, unaffected by a successful flash, immediately
 * re-escalates back to the loader on the very next boot before the fix ever gets a chance
 * to run (found for real, 2026-09-30: a genuine auto-recovery loop, each cycle a real
 * success, that could never reach a healthy boot). Read and cleared by KittenOS
 * (`PurrOS/main/main.c`, `CONFIG_PURR_PROFILE_RECOVERY`) on the boot right after: stage 2
 * of recovery, reusing the same saved network record to carry on installing `kernel` and
 * the module index (`Install/SPEC.md` step 3) automatically, not just sitting at a normal
 * login prompt as if nothing had just happened. */
#define PURR_CFGF_CONTINUE_INSTALL (1u << 6)

/* boot_target */
#define PURR_TARGET_NORMAL         0
#define PURR_TARGET_KITTENOS       1
#define PURR_TARGET_LOADER         2

/* update_state (PurrOS/SPEC.md section 6.1) */
#define PURR_UPD_NONE              0
#define PURR_UPD_STAGED            1
#define PURR_UPD_REQUESTED         2
#define PURR_UPD_MOVING            3
#define PURR_UPD_UNCONFIRMED       4
#define PURR_UPD_CONFIRMED         5
#define PURR_UPD_FAILED            6

/* Components that have a version floor. */
#define PURR_COMP_KITTENOS         1
#define PURR_COMP_KERNEL           2
#define PURR_COMP_COREOS           3
#define PURR_COMP_APPMANAGER       4
#define PURR_COMP_RUNTIME          5
#define PURR_COMP_DEVBUNDLE        6
#define PURR_COMP_BOOTPKG          7
#define PURR_COMP_LOADER           8
#define PURR_FLOOR_SLOTS           8

#define PURR_KEY_SLOTS             8            /* override slots in purrcfg */
#define PURR_KEY_MAX_ID            31           /* revocation is a 32-bit mask */

typedef struct PURR_PACKED {
    uint8_t component;                        /* PURR_COMP_*, 0 = unused slot */
    uint8_t reserved[3];
    uint32_t version;                         /* packed major<<24 | minor<<16 | patch */
} purr_floor_t;

typedef struct PURR_PACKED {
    uint8_t key_id;
    uint8_t role;                             /* PURR_ROLE_* */
    uint8_t valid;                            /* 1 = slot in use */
    uint8_t reserved;
    uint8_t pub[PURR_PUBKEY_LEN];
} purr_key_slot_t;

typedef struct PURR_PACKED {
    uint8_t  target;                          /* which file the update is for */
    uint8_t  state;                           /* PURR_UPD_* */
    uint8_t  attempts;
    uint8_t  reserved;
    uint32_t version;                         /* packed version being installed */
} purr_update_t;

typedef struct PURR_PACKED {
    uint32_t magic;                           /* PURR_CFG_MAGIC */
    uint16_t version;                         /* PURR_CFG_VERSION */
    uint16_t size;                            /* sizeof(purr_cfg_t) */
    uint32_t seq;                             /* increments on every write */
    uint8_t  secure_mode;                     /* PURR_SECURE_* */
    uint8_t  boot_target;                     /* PURR_TARGET_* */
    uint8_t  reserved0[2];
    uint32_t flags;                           /* PURR_CFGF_* */
    uint32_t revoked_keys;                    /* bit per key id */
    uint32_t boot_seq;                        /* written only by the bootloader */
    uint32_t boot_fail_count;
    uint32_t latest_time;                     /* unix seconds, only moves forward */
    purr_update_t update;
    purr_floor_t floors[PURR_FLOOR_SLOTS];
    purr_key_slot_t keys[PURR_KEY_SLOTS];     /* override slots for the key bag */
    uint8_t  efuse_arm[32];                   /* reserved for the eFuse gating flow */
    uint8_t  reserved1[64];
    uint32_t crc32;                           /* over everything above, must stay last */
} purr_cfg_t;

PURR_STATIC_ASSERT(sizeof(purr_cfg_t) <= PURR_CFG_SECTOR_SIZE, "purrcfg must fit a sector");

/* ------------------------------------------------------------------------- */
/* Bootloader handoff (bootloader/SPEC.md section 7)                         */
/* ------------------------------------------------------------------------- */

#define PURR_HANDOFF_MAGIC         0x4F464E48u  /* 'HNFO' */
#define PURR_HANDOFF_VERSION       1

#define PURR_BOOT_NONE             0     /* no handoff, e.g. a stock bootloader */
#define PURR_BOOT_VERIFIED         1
#define PURR_BOOT_FAILED_WARNED    2
#define PURR_BOOT_FORCED_RECOVERY  3
#define PURR_BOOT_CONFIG_DEFAULT   4

typedef struct PURR_PACKED {
    uint32_t magic;                           /* PURR_HANDOFF_MAGIC */
    uint8_t  version;                         /* PURR_HANDOFF_VERSION */
    uint8_t  boot_state;                      /* PURR_BOOT_* */
    uint8_t  key_id_used;                     /* 0xFF = none */
    uint8_t  flags_used;                      /* which one-shot flags were consumed */
    uint32_t bootloader_version;              /* packed version */
    uint32_t crc32;                           /* over everything above */
} purr_handoff_t;

PURR_STATIC_ASSERT(sizeof(purr_handoff_t) == 16, "handoff layout changed");

/* ------------------------------------------------------------------------- */
/* Boot package (bootloader/SPEC.md section 9)                               */
/* ------------------------------------------------------------------------- */

/*
 * The payload of a boot package image (module subtype PURR_MOD_BOOTPKG) is this
 * preamble, then `text_size` bytes of code, then `data_size` bytes of read-only and
 * initialised data. The bootloader copies them into its reserved RAM window, zeroes
 * `bss_size` bytes after the data, and calls `entry`.
 *
 * ESP32-S3 window: one block of internal SRAM that is seen twice, as data at
 * 0x3FC88000 and as code at 0x40378000. The first 32 KB is code, the next 32 KB is
 * data and bss. The package is linked for those addresses, so it needs no relocation.
 * The window is free before the system is loaded and is overwritten by it.
 */
typedef struct PURR_PACKED {
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t entry;                           /* run address of purr_pkg_entry */
} purr_pkg_preamble_t;

PURR_STATIC_ASSERT(sizeof(purr_pkg_preamble_t) == 16, "package preamble layout changed");

#define PURR_PKG_S3_TEXT_WRITE     0x3FC88000u   /* where the bootloader writes the code */
#define PURR_PKG_S3_TEXT_RUN       0x40378000u   /* where the code runs */
#define PURR_PKG_S3_DATA           0x3FC90000u
#define PURR_PKG_S3_TEXT_MAX       0x8000u
#define PURR_PKG_S3_DATA_MAX       0x8000u

#define PURR_PKG_MAX_PARTS         8
#define PURR_PKG_SERVICES_VERSION  1

/* A bootable-or-not app slot, as the bootloader found it in the partition table. */
typedef struct {
    char    name[16];
    uint8_t bootable;                         /* nonzero if it starts with a valid image */
    uint8_t pad[3];
} purr_boot_part_t;

#define PURR_GPIO_OUT              0         /* push-pull output, starts low */
#define PURR_GPIO_OPEN_DRAIN       1         /* output low or released; pulled up, readable */

/* What the bootloader offers the package. */
typedef struct {
    uint32_t version;                         /* PURR_PKG_SERVICES_VERSION */
    void (*log)(const char *line);
    void (*delay_us)(uint32_t us);
    /* Route a pin to the GPIO matrix in the given mode. Levels are then written by the
     * package itself, straight to the GPIO registers. */
    void (*gpio_setup)(int pin, int mode);
} purr_boot_services_t;

#define PURR_PKG_CHOICE_NORMAL             (-1)   /* leave the choice to the bootloader */
#define PURR_PKG_CHOICE_INTERNET_RECOVERY  (-2)   /* start the recovery loader (the "test" slot) */

/*
 * The package's entry. Returns the index in `parts` to boot, or one of the
 * PURR_PKG_CHOICE_* sentinels above.
 */
typedef int (*purr_pkg_entry_fn)(const purr_boot_services_t *svc,
                                 const purr_boot_part_t *parts, int nparts, int preferred);

#ifdef __cplusplus
}
#endif

#endif /* PURR_ABI_H */
