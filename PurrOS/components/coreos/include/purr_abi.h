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
#define PURR_MOD_UIASSETS          10    /* a UI icon/picture pack for the ui_assets partition (data, not code); written by stage 2 or `netinstall uiassets` */
#define PURR_MOD_BOOTLOADER        9     /* the second-stage bootloader itself (flash 0x0), updated only by KittenOS with a backup (bootloader/SPEC.md 9.2) */

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

/* Persistent (not one-shot) flag in purr_cfg_t.flags: development mode (plan3.md). Off by
 * default. While it is on, an effective mode of `enforce` is relaxed to `warn` -- a failed or
 * unsigned image boots with a warning -- unless the signed trust block says DEV_LOCKED. It never
 * lowers the mode below `warn`, and it is reported in the handoff and the secure status, so apps
 * can refuse to treat the device as trusted. Written by the OS (Settings, the `secure` command)
 * or baked into the initial purrcfg before flashing. */
#define PURR_CFGF_DEV_MODE         (1u << 8)

/* Persistent flag in purr_cfg_t.flags: FRESH. On by default in every new purrcfg (purr_cfg_defaults, and purrstrap's
 * initial purrcfg), meaning "this device has never reached a healthy boot of the full PURR OS". With it set and no
 * system installed the bootloader reports a first run (PURR_RR_BLANK) and the recovery loader opens with "Hi there,
 * first time?" instead of "something went wrong". Cleared by the full OS the first time it reaches a healthy boot
 * (purr_mark_boot_healthy); never by the bootloader, the loader or KittenOS, so it survives the whole install. */
#define PURR_CFGF_FRESH            (1u << 9)

/* Persistent flag in purr_cfg_t.flags: DEV_RECOVERY. Lets the recovery loader and KittenOS pull the recovery images from an SDK
 * server on the local network (the same server that hosts apps, `purrstrap dev serve`) instead of the official one, after the
 * owner picks and confirms the server on an extra setup page. Honoured by the bootloader ONLY while development mode is active
 * (like IGNORE_ONCE): it then reports PURR_SECST_DEVREC in the handoff. A device whose signed trust block locks development
 * mode out can never use it. The images themselves are still checked against the boot key before anything is written. */
#define PURR_CFGF_DEV_RECOVERY     (1u << 10)

/* purr_cfg_t.trust_flags (part of the signed trust block): */
#define PURR_TRUSTF_DEV_LOCKED     (1u << 0)    /* a production device: DEV_MODE is ignored */

/* A purr_cfg_t that purr_trust_view() filled in: the effective values, for decisions only. It
 * has this magic, so purr_cfg_store() refuses it -- storing a view would overwrite the signed
 * trust state with its own consequences. */
#define PURR_CFG_VIEW_MAGIC        0x57454956u  /* 'VIEW' */

/* Persistent flags in purr_cfg_t.flags: a reset (the recovery app's reinstall or erase) marks the kernel and KittenOS slots BAD. The
 * bootloader will not launch a slot marked bad (as long as the recovery loader is there to take over), so an interrupted reinstall can
 * never fall back to the old system. They are cleared per slot by whatever writes that slot's new image (purr_slot_trailer_write), and by
 * nothing else. */
#define PURR_CFGF_BAD_KERNEL       (1u << 11)
#define PURR_CFGF_BAD_KITTENOS     (1u << 12)

/* Persistent flag: FAST_START ("Fast startup" in Settings). On a healthy start the bootloader loads no boot package and draws nothing: it checks
 * the system, as always, and jumps. Any trouble (a failed check, a start that never reached healthy, a recovery request) brings the screens back
 * for that start. It removes what is shown, never a check, so every secure mode behaves exactly as before. */
#define PURR_CFGF_FAST_START       (1u << 13)
/* One-shot: SHOW_MENU ("Restart to the boot menu" in Settings). The next start shows the boot menu; the bootloader clears the flag when it reads it. */
#define PURR_CFGF_SHOW_MENU        (1u << 14)

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
/* Set by the recovery loader alongside PURR_CFGF_CONTINUE_INSTALL above, but only on the
 * unattended path (run_auto_mode(), PurrOS/main/recovery_loader.c) -- never on an attended
 * install, where a human already sat through offer_menu()/get_connected() and is still right
 * there for the login prompt that follows. Read and cleared by login.c's first_time_setup():
 * blocking forever on a typed username (read_line(), no timeout) is fine when someone is
 * known to be present, but on this path nothing summoned anyone to answer it -- same reasoning
 * run_auto_mode() itself already applies to skip offer_menu(). A separate flag from
 * CONTINUE_INSTALL because by the time login.c runs, main.c has already called
 * purr_continue_install(), which has already consumed CONTINUE_INSTALL -- this one needs to
 * survive one boot stage further. */
#define PURR_CFGF_UNATTENDED_SETUP (1u << 7)

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
#define PURR_COMP_BOOTLOADER       9     /* staged like the system files, but the swap writes flash 0x0 and keeps a backup */
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
    uint8_t  secure_mode;                     /* PURR_SECURE_*: the SIGNED trust mode (see trust_sig).
                                               * Only honoured when trust_sig verifies; otherwise
                                               * the device behaves as `warn`. */
    uint8_t  boot_target;                     /* PURR_TARGET_* */
    uint8_t  tighten;                         /* PURR_SECURE_*, unsigned: a floor on the mode. The OS
                                               * may raise it; it can never loosen anything. */
    uint8_t  trust_flags;                     /* PURR_TRUSTF_*, signed */
    uint32_t flags;                           /* PURR_CFGF_* */
    uint32_t revoked_keys;                    /* bit per key id */
    uint32_t boot_seq;                        /* written only by the bootloader */
    uint32_t boot_fail_count;
    uint32_t latest_time;                     /* unix seconds, only moves forward */
    purr_update_t update;
    purr_floor_t floors[PURR_FLOOR_SLOTS];
    purr_key_slot_t keys[PURR_KEY_SLOTS];     /* override slots for the key bag */
    uint8_t  efuse_arm[32];                   /* reserved for the eFuse gating flow */
    uint8_t  trust_sig[64];                   /* ECDSA P-256 by a boot-role key over the trust block:
                                               * secure_mode, trust_flags, revoked_keys, keys[]
                                               * (purr_trust.h). All 0x00 or all 0xFF = unsigned. */
    uint32_t crc32;                           /* over everything above, must stay last */
} purr_cfg_t;

PURR_STATIC_ASSERT(sizeof(purr_cfg_t) <= PURR_CFG_SECTOR_SIZE, "purrcfg must fit a sector");

/* ------------------------------------------------------------------------- */
/* Bootloader handoff (bootloader/SPEC.md section 7)                         */
/* ------------------------------------------------------------------------- */

#define PURR_HANDOFF_MAGIC         0x4F464E48u  /* 'HNFO' */
#define PURR_HANDOFF_VERSION       2

/* Why the bootloader ended up in (or near) recovery: a set of flags, so several can apply at once. The recovery
 * loader shows them on its first page (purr_reason.h has the wording). */
#define PURR_RR_UNSIGNED   (1u << 0)     /* a system image is not signed by a key this device trusts */
#define PURR_RR_ALTERED    (1u << 1)     /* an image no longer matches what was signed (tampering, or a bad write) */
#define PURR_RR_DAMAGED    (1u << 2)     /* an image is unreadable or not a valid image at all */
#define PURR_RR_ROLLBACK   (1u << 3)     /* an image is older than this device allows */
#define PURR_RR_REPEATED   (1u << 4)     /* the device failed to start several times in a row */
#define PURR_RR_NO_SYSTEM  (1u << 5)     /* nothing bootable was left */
#define PURR_RR_REQUESTED  (1u << 6)     /* somebody asked for recovery (the boot menu, or a command) */
#define PURR_RR_CONFIG     (1u << 7)     /* the boot settings could not be read */
#define PURR_RR_SYSTEM     (1u << 8)     /* the running system reported a problem and asked for a fresh install */
#define PURR_RR_BLANK      (1u << 9)     /* FRESH is set and no system is installed: a first run, not a failure */
#define PURR_RR_COUNT      10

#define PURR_BOOT_NONE             0     /* no handoff, e.g. a stock bootloader */
#define PURR_BOOT_VERIFIED         1
#define PURR_BOOT_FAILED_WARNED    2
#define PURR_BOOT_FORCED_RECOVERY  3
#define PURR_BOOT_CONFIG_DEFAULT   4     /* purrcfg unreadable, defaults used */
#define PURR_BOOT_AUTO_REINSTALL   5     /* the failure-count ladder sent us to the loader */
#define PURR_BOOT_UNCHECKED        6     /* normal boot; the slot passed only the shallow
                                          * app-image magic check, no PURR signature was
                                          * verified (per-image verification isn't built for
                                          * kernel/kittenos/loader yet, bootloader/SPEC.md
                                          * section 3). Becomes VERIFIED once it is. */

typedef struct PURR_PACKED {
    uint32_t magic;                           /* PURR_HANDOFF_MAGIC */
    uint8_t  version;                         /* PURR_HANDOFF_VERSION */
    uint8_t  boot_state;                      /* PURR_BOOT_* */
    uint8_t  key_id_used;                     /* 0xFF = none */
    uint8_t  flags_used;                      /* which one-shot flags were consumed */
    uint32_t bootloader_version;              /* packed version */
    uint8_t  secure_state;                    /* PURR_SECST_*: what the bootloader enforced */
    uint16_t recovery_reasons;                /* PURR_RR_*: why it is sending the device to recovery (0 = none) */
    uint8_t  reserved;
    uint32_t crc32;                           /* over everything above */
} purr_handoff_t;

PURR_STATIC_ASSERT(sizeof(purr_handoff_t) == 20, "handoff layout changed");

/* secure_state: the effective mode the bootloader used (bits 0-1, PURR_SECURE_*), whether
 * development mode was active (bit 2), and whether the trust block's signature verified
 * (bit 3). Apps read the combination through the secure status (plan3.md). */
#define PURR_SECST_MODE(s)         ((s) & 3u)
#define PURR_SECST_DEV             (1u << 2)
#define PURR_SECST_SIGNED          (1u << 3)
#define PURR_SECST_DEVREC          (1u << 4)     /* DEV_RECOVERY is on and development mode is active: a dev server may host recovery */

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
#define PURR_PKG_SERVICES_VERSION  4    /* 2: secure_state. 3: the package may be called to show progress. 4: splash, key check, menu reasons */

/* A progress call (services version 3; only made to a package whose own version is 0.2 or later). The same entry
 * point is called with nparts < 0: `parts` then points at a NUL-terminated label (the slot being verified, a plain
 * C string, not a purr_boot_part_t), nparts is -PURR_PKG_STAGE_*, and `preferred` is the progress in thousandths.
 * The package draws its progress screen and returns 0. The bootloader makes these calls while it verifies the
 * slots, before the menu entry call, so the screen is already up when the menu appears. */
#define PURR_PKG_STAGE_VERIFY      1
/* Services version 4 (only made to a package whose own version is 0.3 or later):
 *   SPLASH:   `parts` is the stage text ("Starting PURR OS"). Draws the start screen (logo, version, stage line, status chips). Returns 0.
 *   KEYCHECK: `preferred` is how many milliseconds to watch the keyboard. Returns 1 if the menu key (Space) was pressed in that time, else 0. */
#define PURR_PKG_STAGE_SPLASH      2
#define PURR_PKG_STAGE_KEYCHECK    3

/* Why the menu is up (purr_boot_services_t.menu_reason, services version 4). */
#define PURR_MENU_WHY_KEY          1    /* the menu key was held at power-on */
#define PURR_MENU_WHY_LAST_FAILED  2    /* the last start never reached a healthy boot */
#define PURR_MENU_WHY_NO_VERIFY    3    /* the system did not pass its check (detail1/detail2 say how) */
#define PURR_MENU_WHY_REQUESTED    4    /* Settings asked for the menu */

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
    /* version >= 2: the effective security state the bootloader is using, PURR_SECST_*
     * (mode, development mode, signed trust block), so the menu can show it. */
    uint32_t secure_state;
    /* version >= 4, for the menu call: why it is up (PURR_MENU_WHY_*), how long it may sit with nobody pressing a key before the default starts, and
     * for PURR_MENU_WHY_NO_VERIFY two short lines saying what failed (either may be NULL). */
    uint32_t menu_reason;
    uint32_t menu_idle_ms;
    const char *detail1;
    const char *detail2;
} purr_boot_services_t;

#define PURR_PKG_CHOICE_NORMAL             (-1)   /* leave the choice to the bootloader */
#define PURR_PKG_CHOICE_INTERNET_RECOVERY  (-2)   /* start the recovery loader (the "test" slot) */
#define PURR_PKG_CHOICE_RESTART            (-3)   /* restart the device (the problem screen's "Restart") */

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
