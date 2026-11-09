/*
 * purr_manifest.h - the release manifest (OTA/SPEC.md section 5).
 *
 * A flat text format, read by things with very little to work with (the recovery
 * loader has no filesystem): stanzas of "key=value" lines separated by a blank line,
 * `#` starts a comment. One stanza per component. Unknown keys are ignored, so the
 * format can grow. An entry missing a required field, or with a malformed size or
 * hash, is dropped rather than trusted with a guessed value.
 *
 * Example:
 *
 *   release=1.2.0
 *   released=2026-09-27
 *
 *   component=coreos
 *   version=1.2.0
 *   chip=esp32s3
 *   board=tdeck_plus
 *   file=coreos-tdeck_plus-1.2.0.kitt
 *   size=245760
 *   sha256=3b1c2f...(64 hex characters)
 *   key=system
 *   min_bootloader=1.0.0
 *   min_coreos=1.0.0
 *   payload_size=233796
 *   payload_sha256=9f1a0c...(64 hex characters, F-14, optional)
 *
 * `chip` and `board` may be "any" to match every chip or board.
 *
 * This same format and parser also serve the module index (OTA/SPEC.md section 6.1) -- a
 * separate file, not a new format: one stanza per individual command module or kernelmod
 * (`about`, `fs`, ...), using `type=module` or `type=kernelmod` to say which folder it
 * belongs in (`/system` or `/kernelmods`). `component` doubles as the module's name. Example:
 *
 *   component=fs
 *   type=kernelmod
 *   version=0.1.0
 *   chip=esp32s3
 *   board=tdeck_plus
 *   file=fs-tdeck_plus-0.1.0.cat
 *   size=2737
 *   sha256=c2a9d0...(64 hex characters)
 *   key=developer
 *   min_coreos=1.2.0
 *
 * And a driver index/pack (Drivers/SPEC.md section 2/3), `type=driver`: one stanza per
 * individual driver file, destined for `/boot/drivers`. `component` doubles as the driver's
 * name ("st7789", "gt911", ...), same as every other type here. `compatible`/`buses`/`catcall`
 * are the driver's own header fields (section 3) -- a single value each for now (one driver,
 * one bus, one catcall), not a list, matching how small every other widget/entry shape in
 * this codebase starts; a driver that genuinely serves more than one `compatible` string or
 * needs more than one bus can get a real list later if one ever actually needs it. Optional,
 * like `key`/`min_coreos` on every other entry here -- an older reader just won't understand
 * what kind of driver it found. Per-board pin assignment (the "pack" half of Drivers/SPEC.md
 * section 2, not covered by this file-level entry) is separate, later work. Example:
 *
 *   component=st7789
 *   type=driver
 *   version=1
 *   chip=esp32s3
 *   board=tdeck_plus
 *   file=st7789-tdeck_plus-1.cat
 *   size=9120
 *   sha256=7e4b21...(64 hex characters)
 *   key=system
 *   compatible=st7789
 *   buses=spi
 *   catcall=display
 */
#ifndef PURR_MANIFEST_H
#define PURR_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MANIFEST_MAX_ENTRIES  16
#define PURR_MANIFEST_NAME_LEN     32
#define PURR_MANIFEST_VERSION_LEN  16
#define PURR_MANIFEST_CHIP_LEN     16
#define PURR_MANIFEST_BOARD_LEN    24
#define PURR_MANIFEST_FILE_LEN     64
#define PURR_MANIFEST_ROLE_LEN     16
#define PURR_MANIFEST_DATE_LEN     16
#define PURR_MANIFEST_TYPE_LEN     16
#define PURR_MANIFEST_COMPAT_LEN   32
#define PURR_MANIFEST_BUSES_LEN    16
#define PURR_MANIFEST_CATCALL_LEN  16

/* `type`, added for the module index (OTA/SPEC.md section 6.1 -- a second, separate file
 * using this same parser, not a new format): empty ("") for a whole-component entry (kernel,
 * coreos, kittenos, ...), "module" or "kernelmod" for one of the individual command
 * modules/kernelmods living in /system or /kernelmods. `component` doubles as that module's
 * name ("about", "fs", ...) either way -- one less field, since nothing needs both at once.
 * `min_coreos` on a module entry means what it already means on every other entry: the
 * minimum CoreOS version it needs, not a required exact match -- the real compatibility gate
 * is the ABI version baked into the .cat file's own header, already checked at load time. */
typedef struct {
    char component[PURR_MANIFEST_NAME_LEN];
    char type[PURR_MANIFEST_TYPE_LEN];
    char version[PURR_MANIFEST_VERSION_LEN];
    char chip[PURR_MANIFEST_CHIP_LEN];
    char board[PURR_MANIFEST_BOARD_LEN];
    char file[PURR_MANIFEST_FILE_LEN];
    uint32_t size;
    uint8_t sha256[32];
    char key_role[PURR_MANIFEST_ROLE_LEN];
    char min_bootloader[PURR_MANIFEST_VERSION_LEN];
    char min_coreos[PURR_MANIFEST_VERSION_LEN];
    /* F-14, optional (0/all-zero if absent -- older manifests, and bootpkg, which has no
     * payload split, don't carry these): size and hash of just the PAYLOAD inside `file`'s
     * container (purr_image_header_t's payload_offset/payload_size/payload_sha256), for a
     * component whose target partition holds only the stripped payload, never the whole
     * signed container (kernel, kittenos -- RecoveryLoader's own partition_matches_manifest()
     * needs this to check what's already on the device without downloading it again). */
    uint32_t payload_size;
    uint8_t payload_sha256[32];
    int have_payload_sha256;
    /* `type=driver` only (Drivers/SPEC.md section 3): empty ("") on every other entry type.
     * Single value each, not a list -- see the file header comment on why. */
    char compatible[PURR_MANIFEST_COMPAT_LEN];
    char buses[PURR_MANIFEST_BUSES_LEN];
    char catcall[PURR_MANIFEST_CATCALL_LEN];
    /* `type=driver`: the driver bus ABI range it works with (purr_driver_bus.h), 0 = not stated; `blocked=yes` marks a version that
     * broke on a newer OS: the update service never offers it (plan8.md "Driver compatibility"). */
    uint16_t abi_min, abi_max;
    uint8_t blocked;
    /* `withdrawn=yes`: the publisher pulled this version (it is bad). Every reader that picks an entry through purr_manifest_find() never sees it. */
    uint8_t withdrawn;
} purr_manifest_entry_t;

#define PURR_MANIFEST_WITHDRAWN_LEN 160
#define PURR_MANIFEST_REASON_LEN    64

typedef struct {
    char release[PURR_MANIFEST_VERSION_LEN];
    char released[PURR_MANIFEST_DATE_LEN];
    /* A release-wide list of withdrawn versions, for a device that is ALREADY on one and for the entry that has since been replaced:
     *   withdrawn_versions=kernel@1.0.20,kittenos@1.0.20,*@1.0.20     (`*` is every component)
     *   withdrawn_reason=the setup wizard showed two Wi-Fi pages        (shown to the person, one line)
     * See OTA/SPEC.md "Withdrawing a release". */
    char withdrawn_versions[PURR_MANIFEST_WITHDRAWN_LEN];
    char withdrawn_reason[PURR_MANIFEST_REASON_LEN];
    purr_manifest_entry_t entries[PURR_MANIFEST_MAX_ENTRIES];
    int count;
    int dropped;         /* entries seen but rejected: missing field, bad size or hash */
    int truncated;        /* nonzero if there were more than PURR_MANIFEST_MAX_ENTRIES */
} purr_manifest_t;

/* Always succeeds in the sense of leaving `m` in a valid, usable state: bad stanzas are
 * dropped (see m->dropped), not fatal. Returns the number of entries kept. */
int purr_manifest_parse(purr_manifest_t *m, const char *text, size_t len);

/* The entry for a component on a given chip and board, or NULL. "any" in the manifest
 * matches every chip or board; chip or board passed as NULL also matches anything. */
const purr_manifest_entry_t *purr_manifest_find(const purr_manifest_t *m, const char *component,
                                                const char *chip, const char *board);

/* The same, but withdrawn entries are returned too: for saying why nothing is offered, and for the tool that writes the file. */
const purr_manifest_entry_t *purr_manifest_find_any(const purr_manifest_t *m, const char *component,
                                                    const char *chip, const char *board);

/* 1 if `component` at `version` has been withdrawn: the manifest's own list names it (as "<component>@<version>" or "*@<version>"). The entry's own
 * `withdrawn=yes` is checked by purr_manifest_find(); this one answers for a version that is NOT in the manifest any more, such as the one a device is running.
 * `reason` (may be NULL) gets the release's note. */
int purr_manifest_is_withdrawn(const purr_manifest_t *m, const char *component, const char *version, char *reason, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MANIFEST_H */
