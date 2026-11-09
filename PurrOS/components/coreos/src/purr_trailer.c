#include "purr_trailer.h"

#include <string.h>

/* The partition, seen as the container it was cut from: bytes below payload_offset come from
 * the trailer's header copy, bytes from payload_offset up come from the start of the partition. */
typedef struct {
    purr_read_fn          rd;
    void                 *ctx;
    const purr_image_header_t *hdr;
    uint32_t              payload_offset;
} virtual_t;

static int virtual_read(void *vctx, uint32_t offset, void *buf, uint32_t len)
{
    virtual_t *v = vctx;

    if ((uint64_t)offset + len <= sizeof(*v->hdr)) {
        memcpy(buf, (const uint8_t *)v->hdr + offset, len);
        return 0;
    }
    if (offset >= v->payload_offset) {
        return v->rd(v->ctx, offset - v->payload_offset, buf, len);
    }
    return -1;      /* the gap between the header and the payload is never read */
}

purr_verify_result_t purr_trailer_verify(const purr_verify_env_t *env, purr_read_fn rd, void *ctx,
                                         uint32_t part_size, uint8_t want_type, uint8_t want_subtype,
                                         purr_image_header_t *hdr_out)
{
    purr_image_header_t hdr;

    if (part_size <= PURR_TRAILER_SIZE) {
        return PURR_V_BAD_LAYOUT;
    }
    if (rd(ctx, PURR_TRAILER_OFFSET(part_size), &hdr, sizeof(hdr)) != 0) {
        return PURR_V_IO_ERROR;
    }
    if (hdr_out) {
        *hdr_out = hdr;
    }
    if (hdr.magic != PURR_IMAGE_MAGIC) {
        return PURR_V_BAD_MAGIC;
    }

    /* What the partition is allowed to hold is the partition's business, not the header's. */
    if (hdr.image_type != want_type ||
        (want_type == PURR_IMG_MODULE && PURR_FLAGS_SUBTYPE(hdr.flags) != want_subtype)) {
        return PURR_V_BAD_TYPE;
    }

    /* The image has to fit in front of the trailer. The container-relative layout checks
     * (offsets, header size, version) are purr_image_verify()'s, run below. */
    if (hdr.payload_size > PURR_TRAILER_MAX_IMAGE(part_size)) {
        return PURR_V_BAD_LAYOUT;
    }
    if (hdr.payload_offset < sizeof(hdr) || hdr.payload_offset > PURR_TRAILER_SIZE) {
        return PURR_V_BAD_LAYOUT;
    }

    virtual_t v = {rd, ctx, &hdr, hdr.payload_offset};
    return purr_image_verify(env, virtual_read, &v, hdr.payload_offset + hdr.payload_size, hdr_out);
}
