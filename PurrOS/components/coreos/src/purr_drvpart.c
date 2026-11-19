#include "purr_drvpart.h"

#include <string.h>

#include "purr_util.h"

static int name_ok(const char *file)
{
    size_t n = strnlen(file, PURR_DRVPART_NAME_LEN);
    if (n == 0 || n >= PURR_DRVPART_NAME_LEN) {
        return 0;                                   /* empty, or no terminator */
    }
    for (size_t i = 0; i < n; i++) {
        if (file[i] == '/' || file[i] == '\\' || (unsigned char)file[i] < 0x20 || (unsigned char)file[i] > 0x7E) {
            return 0;                               /* a file name, never a path */
        }
    }
    return 1;
}

int purr_drvpart_slot_info(purr_drvpart_read_fn rd, void *ctx, int slot, purr_drvslot_info_t *info)
{
    if (slot < 0 || slot >= PURR_DRVPART_SLOTS) {
        return PURR_DRVPART_DAMAGED;
    }
    purr_drvslot_hdr_t h;
    int e = rd(ctx, (uint32_t)slot * PURR_DRVPART_SLOT_SIZE, &h, sizeof(h));
    if (e != 0) {
        return e < 0 ? e : -e;
    }
    const uint8_t *raw = (const uint8_t *)&h;
    int blank = 1;
    for (size_t i = 0; i < sizeof(h); i++) {
        if (raw[i] != 0xFF) {
            blank = 0;
            break;
        }
    }
    if (blank) {
        return PURR_DRVPART_EMPTY;
    }
    if (h.magic != PURR_DRVPART_MAGIC || h.version != PURR_DRVPART_VERSION || !name_ok(h.file) || h.length == 0 || h.length > PURR_DRVPART_MAX_CAT) {
        return PURR_DRVPART_DAMAGED;
    }
    if (info != NULL) {
        info->slot = slot;
        memset(info->file, 0, sizeof(info->file));
        memcpy(info->file, h.file, strnlen(h.file, PURR_DRVPART_NAME_LEN - 1));
        info->length = h.length;
    }
    return 0;
}

int purr_drvpart_slot_read(purr_drvpart_read_fn rd, void *ctx, int slot, uint8_t *buf, uint32_t cap, uint32_t *len, purr_drvslot_info_t *info)
{
    purr_drvslot_info_t local;
    purr_drvslot_info_t *i = info != NULL ? info : &local;
    int e = purr_drvpart_slot_info(rd, ctx, slot, i);
    if (e != 0) {
        return e;
    }
    if (i->length > cap) {
        return PURR_DRVPART_DAMAGED;
    }
    purr_drvslot_hdr_t h;
    if (rd(ctx, (uint32_t)slot * PURR_DRVPART_SLOT_SIZE, &h, sizeof(h)) != 0 ||
        rd(ctx, (uint32_t)slot * PURR_DRVPART_SLOT_SIZE + PURR_DRVPART_HEADER, buf, i->length) != 0) {
        return -1;
    }
    if (purr_crc32(0, buf, i->length) != h.crc32) {
        return PURR_DRVPART_DAMAGED;
    }
    if (len != NULL) {
        *len = i->length;
    }
    return 0;
}

int purr_drvpart_build_slot(uint8_t *slot_out, const char *file, const uint8_t *cat, uint32_t len)
{
    if (file == NULL || cat == NULL || len == 0 || len > PURR_DRVPART_MAX_CAT || !name_ok(file)) {
        return -1;
    }
    memset(slot_out, 0xFF, PURR_DRVPART_SLOT_SIZE);
    purr_drvslot_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = PURR_DRVPART_MAGIC;
    h.version = PURR_DRVPART_VERSION;
    h.flags = 0;
    h.length = len;
    h.crc32 = purr_crc32(0, cat, len);
    memcpy(h.file, file, strlen(file));
    memset(h.reserved, 0xFF, sizeof(h.reserved));
    memcpy(slot_out, &h, sizeof(h));
    memcpy(slot_out + PURR_DRVPART_HEADER, cat, len);
    return 0;
}
