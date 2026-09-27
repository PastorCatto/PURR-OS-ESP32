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

#ifdef __cplusplus
}
#endif

#endif /* PURR_ABI_H */
