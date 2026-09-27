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

void purr_netrec_save(const char *ssid, const char *pass)
{
    const esp_partition_t *p = find();
    if (p == NULL || p->size < sizeof(record_t)) {
        return;
    }
    record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = MAGIC;
    snprintf(rec.ssid, sizeof(rec.ssid), "%s", ssid);
    snprintf(rec.pass, sizeof(rec.pass), "%s", pass ? pass : "");
    rec.crc32 = purr_crc32(0, &rec, offsetof(record_t, crc32));

    if (esp_partition_erase_range(p, 0, p->erase_size) != ESP_OK ||
        esp_partition_write(p, 0, &rec, sizeof(rec)) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the recovery network record");
    }
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
    if (esp_partition_read(p, 0, &rec, sizeof(rec)) != ESP_OK) {
        return -1;
    }
    if (rec.magic != MAGIC || rec.crc32 != purr_crc32(0, &rec, offsetof(record_t, crc32))) {
        return -1;
    }
    rec.ssid[sizeof(rec.ssid) - 1] = '\0';
    rec.pass[sizeof(rec.pass) - 1] = '\0';
    memcpy(ssid, rec.ssid, sizeof(rec.ssid));
    memcpy(pass, rec.pass, sizeof(rec.pass));
    return 0;
}
