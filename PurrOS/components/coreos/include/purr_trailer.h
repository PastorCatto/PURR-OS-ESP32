/*
 * purr_trailer.h - signatures for raw app partitions (bootloader/SPEC.md section 3.1).
 *
 * kernel, kittenos and loader are real ESP-IDF app images: the stock loader insists on the ESP
 * magic byte at the very start of the partition, so a PURR header cannot go first. The
 * signature goes in the LAST sector of the partition instead -- a verbatim copy of the signed
 * container header that the release file already carries. The ESP image stays untouched at
 * offset 0, so the stock loader still loads it.
 *
 *   partition offset 0                          ESP app image (the container's payload)
 *   ...                                         0xFF
 *   partition end - PURR_TRAILER_SIZE           purr_image_header_t, then 0xFF
 *
 * Verification treats the partition as the container it was cut from: header bytes come from
 * the trailer, payload bytes from offset 0, and the ordinary purr_image_verify() does the rest
 * (chip, type and role, version floor, key, revocation, payload hash, signature). Plain C with
 * no ESP-IDF dependency, shared by the bootloader and CoreOS, tested on the PC.
 */
#ifndef PURR_TRAILER_H
#define PURR_TRAILER_H

#include <stdint.h>

#include "purr_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One flash sector, so writing it never disturbs the image. */
#define PURR_TRAILER_SIZE  4096u

/* Where the trailer lives in a partition of part_size bytes. */
#define PURR_TRAILER_OFFSET(part_size)  ((uint32_t)(part_size) - PURR_TRAILER_SIZE)

/* The largest image a partition of part_size bytes can hold once the trailer is reserved. */
#define PURR_TRAILER_MAX_IMAGE(part_size)  ((uint32_t)(part_size) - PURR_TRAILER_SIZE)

/*
 * Verify the app image in a partition of part_size bytes, read through rd. want_type and
 * want_subtype say what this partition may hold (for example PURR_IMG_MODULE and
 * PURR_MOD_KERNEL); a validly signed image of another kind is rejected, so a signed loader
 * cannot be passed off as a kernel. hdr_out, if not NULL, receives the trailer's header.
 *
 * Returns PURR_V_OK, or the first failure. PURR_V_BAD_MAGIC specifically means "no trailer
 * here" -- a slot flashed straight from a build, never signed -- as opposed to a trailer that
 * is present and wrong.
 */
purr_verify_result_t purr_trailer_verify(const purr_verify_env_t *env, purr_read_fn rd, void *ctx,
                                         uint32_t part_size, uint8_t want_type, uint8_t want_subtype,
                                         purr_image_header_t *hdr_out);

#ifdef __cplusplus
}
#endif

#endif /* PURR_TRAILER_H */
