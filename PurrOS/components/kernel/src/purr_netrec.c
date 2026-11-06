#include "purr_netrec.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "purr_io.h"

#include "purr_util.h"

static const char *TAG = "netrec";

#define MAGIC 0x43455250u   /* 'PREC' little-endian */

typedef struct {
    uint32_t magic;
    char ssid[PURR_NETREC_SSID_LEN];
    char pass[PURR_NETREC_PASS_LEN];
    uint8_t pad[3];         /* keep crc32 on a 4-byte boundary */
    uint32_t crc32;
} record_t;

static const esp_partition_t *find(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "netrec");
}

/* The recovery source record sits in the same sector, further in: the dev server the owner picked on the recovery setup page, so
 * KittenOS's stage 2 pulls from the same place. Both records are rewritten together, because a flash sector is erased whole. */
#define SRC_MAGIC 0x43525350u   /* 'PSRC' little-endian */
#define SRC_OFFSET 0x100

typedef struct {
    uint32_t magic;
    char base[PURR_NETREC_SRC_BASE_LEN];
    char name[PURR_NETREC_SRC_NAME_LEN];
    char id[PURR_NETREC_SRC_ID_LEN];
    uint8_t pad[3];
    uint32_t crc32;
} src_record_t;

/* The official source record (a signed move, purr_move.h), further in again. */
#define OFF_MAGIC 0x53464F50u   /* 'POFS' little-endian */
#define OFF_OFFSET 0x200

typedef struct {
    uint32_t magic;
    uint32_t seq;
    char recovery[PURR_NETREC_OFF_URL_LEN];
    char modules[PURR_NETREC_OFF_URL_LEN];
    char message[PURR_NETREC_OFF_MSG_LEN];
    uint32_t crc32;
} off_record_t;

static int read_net(const esp_partition_t *p, record_t *rec)
{
    if (purr_io_partition_read(p, 0, rec, sizeof(*rec)) != ESP_OK) {
        return -1;
    }
    return rec->magic == MAGIC && rec->crc32 == purr_crc32(0, rec, offsetof(record_t, crc32)) ? 0 : -1;
}

static int read_src(const esp_partition_t *p, src_record_t *src)
{
    if (purr_io_partition_read(p, SRC_OFFSET, src, sizeof(*src)) != ESP_OK) {
        return -1;
    }
    return src->magic == SRC_MAGIC && src->crc32 == purr_crc32(0, src, offsetof(src_record_t, crc32)) ? 0 : -1;
}

static int read_off(const esp_partition_t *p, off_record_t *off)
{
    if (purr_io_partition_read(p, OFF_OFFSET, off, sizeof(*off)) != ESP_OK) {
        return -1;
    }
    return off->magic == OFF_MAGIC && off->crc32 == purr_crc32(0, off, offsetof(off_record_t, crc32)) ? 0 : -1;
}

/* Writes the records that are given (one that is left out reads as empty). The whole sector is erased first, so a caller that changes one record passes the others
 * it read before. */
static void write_all(const esp_partition_t *p, const record_t *net, const src_record_t *src, const off_record_t *off)
{
    if (purr_io_partition_erase_range(p, 0, p->erase_size) != ESP_OK ||
        (net != NULL && purr_io_partition_write(p, 0, net, sizeof(*net)) != ESP_OK) ||
        (src != NULL && purr_io_partition_write(p, SRC_OFFSET, src, sizeof(*src)) != ESP_OK) ||
        (off != NULL && purr_io_partition_write(p, OFF_OFFSET, off, sizeof(*off)) != ESP_OK)) {
        ESP_LOGW(TAG, "could not save the recovery record");
    }
}

void purr_netrec_save(const char *ssid, const char *pass)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < OFF_OFFSET + sizeof(off_record_t)) {
        return;
    }
    record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = MAGIC;
    snprintf(rec.ssid, sizeof(rec.ssid), "%s", ssid);
    snprintf(rec.pass, sizeof(rec.pass), "%s", pass ? pass : "");
    rec.crc32 = purr_crc32(0, &rec, offsetof(record_t, crc32));
    src_record_t src;
    int have_src = read_src(p, &src) == 0;                 /* keep the chosen server across a network change */
    off_record_t off;
    int have_off = read_off(p, &off) == 0;
    write_all(p, &rec, have_src ? &src : NULL, have_off ? &off : NULL);
}

int purr_netrec_load(char ssid[PURR_NETREC_SSID_LEN], char pass[PURR_NETREC_PASS_LEN])
{
    ssid[0] = '\0';
    pass[0] = '\0';
    const esp_partition_t *p = find();
    if (p == NULL || p->size < sizeof(record_t)) {
        return -1;
    }
    record_t rec;
    if (read_net(p, &rec) != 0) {
        return -1;
    }
    rec.ssid[sizeof(rec.ssid) - 1] = '\0';
    rec.pass[sizeof(rec.pass) - 1] = '\0';
    memcpy(ssid, rec.ssid, sizeof(rec.ssid));
    memcpy(pass, rec.pass, sizeof(rec.pass));
    return 0;
}

void purr_netrec_save_source(const char *base, const char *name, const char *id)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < OFF_OFFSET + sizeof(off_record_t)) {
        return;
    }
    record_t net;
    int have_net = read_net(p, &net) == 0;
    off_record_t off;
    int have_off = read_off(p, &off) == 0;
    if (base == NULL || base[0] == '\0') {                 /* clear the choice: back to the official server */
        write_all(p, have_net ? &net : NULL, NULL, have_off ? &off : NULL);
        return;
    }
    src_record_t src;
    memset(&src, 0, sizeof(src));
    src.magic = SRC_MAGIC;
    snprintf(src.base, sizeof(src.base), "%s", base);
    snprintf(src.name, sizeof(src.name), "%s", name ? name : "");
    snprintf(src.id, sizeof(src.id), "%s", id ? id : "");
    src.crc32 = purr_crc32(0, &src, offsetof(src_record_t, crc32));
    write_all(p, have_net ? &net : NULL, &src, have_off ? &off : NULL);
}

int purr_netrec_load_source(char base[PURR_NETREC_SRC_BASE_LEN], char name[PURR_NETREC_SRC_NAME_LEN],
                            char id[PURR_NETREC_SRC_ID_LEN])
{
    base[0] = name[0] = id[0] = '\0';
    const esp_partition_t *p = find();
    if (p == NULL || p->size < OFF_OFFSET + sizeof(off_record_t)) {
        return -1;
    }
    src_record_t src;
    if (read_src(p, &src) != 0) {
        return -1;
    }
    src.base[sizeof(src.base) - 1] = src.name[sizeof(src.name) - 1] = src.id[sizeof(src.id) - 1] = '\0';
    memcpy(base, src.base, sizeof(src.base));
    memcpy(name, src.name, sizeof(src.name));
    memcpy(id, src.id, sizeof(src.id));
    return 0;
}

void purr_netrec_save_official(const char *recovery, const char *modules, const char *message, uint32_t seq)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < OFF_OFFSET + sizeof(off_record_t)) {
        return;
    }
    record_t net;
    int have_net = read_net(p, &net) == 0;
    src_record_t src;
    int have_src = read_src(p, &src) == 0;
    off_record_t off;
    memset(&off, 0, sizeof(off));
    off.magic = OFF_MAGIC;
    off.seq = seq;
    snprintf(off.recovery, sizeof(off.recovery), "%s", recovery ? recovery : "");
    snprintf(off.modules, sizeof(off.modules), "%s", modules ? modules : "");
    snprintf(off.message, sizeof(off.message), "%s", message ? message : "");
    off.crc32 = purr_crc32(0, &off, offsetof(off_record_t, crc32));
    write_all(p, have_net ? &net : NULL, have_src ? &src : NULL, &off);
}

int purr_netrec_load_official(char recovery[PURR_NETREC_OFF_URL_LEN], char modules[PURR_NETREC_OFF_URL_LEN], char message[PURR_NETREC_OFF_MSG_LEN], uint32_t *seq)
{
    recovery[0] = modules[0] = message[0] = '\0';
    *seq = 0;
    const esp_partition_t *p = find();
    if (p == NULL || p->size < OFF_OFFSET + sizeof(off_record_t)) {
        return -1;
    }
    off_record_t off;
    if (read_off(p, &off) != 0) {
        return -1;
    }
    off.recovery[sizeof(off.recovery) - 1] = off.modules[sizeof(off.modules) - 1] = off.message[sizeof(off.message) - 1] = '\0';
    memcpy(recovery, off.recovery, sizeof(off.recovery));
    memcpy(modules, off.modules, sizeof(off.modules));
    memcpy(message, off.message, sizeof(off.message));
    *seq = off.seq;
    return 0;
}
