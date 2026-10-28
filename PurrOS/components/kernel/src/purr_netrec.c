#include "purr_netrec.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

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

static int read_net(const esp_partition_t *p, record_t *rec)
{
    if (esp_partition_read(p, 0, rec, sizeof(*rec)) != ESP_OK) {
        return -1;
    }
    return rec->magic == MAGIC && rec->crc32 == purr_crc32(0, rec, offsetof(record_t, crc32)) ? 0 : -1;
}

static int read_src(const esp_partition_t *p, src_record_t *src)
{
    if (esp_partition_read(p, SRC_OFFSET, src, sizeof(*src)) != ESP_OK) {
        return -1;
    }
    return src->magic == SRC_MAGIC && src->crc32 == purr_crc32(0, src, offsetof(src_record_t, crc32)) ? 0 : -1;
}

/* have_net / have_src: write that record (the other is left out, so it reads as empty). */
static void write_both(const esp_partition_t *p, const record_t *net, const src_record_t *src)
{
    if (esp_partition_erase_range(p, 0, p->erase_size) != ESP_OK ||
        (net != NULL && esp_partition_write(p, 0, net, sizeof(*net)) != ESP_OK) ||
        (src != NULL && esp_partition_write(p, SRC_OFFSET, src, sizeof(*src)) != ESP_OK)) {
        ESP_LOGW(TAG, "could not save the recovery record");
    }
}

void purr_netrec_save(const char *ssid, const char *pass)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < SRC_OFFSET + sizeof(src_record_t)) {
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
    write_both(p, &rec, have_src ? &src : NULL);
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
    if (p == NULL || p->size < SRC_OFFSET + sizeof(src_record_t)) {
        return;
    }
    record_t net;
    int have_net = read_net(p, &net) == 0;
    if (base == NULL || base[0] == '\0') {                 /* clear the choice: back to the official server */
        write_both(p, have_net ? &net : NULL, NULL);
        return;
    }
    src_record_t src;
    memset(&src, 0, sizeof(src));
    src.magic = SRC_MAGIC;
    snprintf(src.base, sizeof(src.base), "%s", base);
    snprintf(src.name, sizeof(src.name), "%s", name ? name : "");
    snprintf(src.id, sizeof(src.id), "%s", id ? id : "");
    src.crc32 = purr_crc32(0, &src, offsetof(src_record_t, crc32));
    write_both(p, have_net ? &net : NULL, &src);
}

int purr_netrec_load_source(char base[PURR_NETREC_SRC_BASE_LEN], char name[PURR_NETREC_SRC_NAME_LEN],
                            char id[PURR_NETREC_SRC_ID_LEN])
{
    base[0] = name[0] = id[0] = '\0';
    const esp_partition_t *p = find();
    if (p == NULL || p->size < SRC_OFFSET + sizeof(src_record_t)) {
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
