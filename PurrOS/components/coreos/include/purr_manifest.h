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
 *
 * `chip` and `board` may be "any" to match every chip or board.
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

typedef struct {
    char component[PURR_MANIFEST_NAME_LEN];
    char version[PURR_MANIFEST_VERSION_LEN];
    char chip[PURR_MANIFEST_CHIP_LEN];
    char board[PURR_MANIFEST_BOARD_LEN];
    char file[PURR_MANIFEST_FILE_LEN];
    uint32_t size;
    uint8_t sha256[32];
    char key_role[PURR_MANIFEST_ROLE_LEN];
    char min_bootloader[PURR_MANIFEST_VERSION_LEN];
    char min_coreos[PURR_MANIFEST_VERSION_LEN];
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
