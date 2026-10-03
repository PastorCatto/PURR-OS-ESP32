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
} purr_manifest_entry_t;

typedef struct {
    char release[PURR_MANIFEST_VERSION_LEN];
    char released[PURR_MANIFEST_DATE_LEN];
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

#ifdef __cplusplus
}
#endif

#endif /* PURR_MANIFEST_H */
