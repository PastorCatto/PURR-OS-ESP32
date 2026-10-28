/*
 * purr_slot_trailer.h - writing and checking the signature trailer of kernel, kittenos and
 * loader (bootloader/SPEC.md section 3.1). Shared by net_install (commands.c) and the recovery
 * loader (recovery_loader.c), the two things that write these slots.
 */
#ifndef PURR_SLOT_TRAILER_H
#define PURR_SLOT_TRAILER_H

#include <stdint.h>

#include "esp_partition.h"

#include "purr_abi.h"
#include "purr_trailer.h"
#include "purr_verify.h"

/* Whether an image of payload_size bytes fits the slot once the trailer sector is reserved. */
int purr_slot_trailer_fits(const esp_partition_t *slot, uint32_t payload_size);

/*
 * Write the signed container header as the slot's trailer and read it back. The caller has
 * already erased the slot and written the payload at offset 0. Returns 0, or -1 on a write or
 * read-back failure.
 */
int purr_slot_trailer_write(const esp_partition_t *slot, const purr_image_header_t *hdr);

/* The signed container header stored in the slot's trailer, unverified. 0 if one was read. */
int purr_slot_trailer_header(const esp_partition_t *slot, purr_image_header_t *out);

/*
 * Verify what is in the slot right now, exactly as the bootloader will (the same shared
 * purr_trailer_verify()). want_type/want_subtype say what the slot may hold. Returns
 * PURR_V_OK, or the first failure; PURR_V_BAD_MAGIC means there is no trailer.
 */
purr_verify_result_t purr_slot_trailer_check(const purr_verify_env_t *env,
                                             const esp_partition_t *slot,
                                             uint8_t want_type, uint8_t want_subtype);

/* Whether a reset marked this slot (the "kernel" or "kittenos" partition) BAD (PURR_CFGF_BAD_*): a writer must then reinstall it even if
 * what is there still verifies. purr_slot_trailer_write() clears the mark once the new image is in. */
int purr_slot_marked_bad(const esp_partition_t *slot);

#endif
