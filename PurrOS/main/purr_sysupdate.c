/* purr_sysupdate.c - see purr_sysupdate.h. */
#include "purr_sysupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "sdkconfig.h"

#include "commands.h"
#include "purr_abi.h"
#include "purr_board.h"
#include "purr_cfgstore.h"
#include "purr_fetch.h"
#include "purr_kernel.h"
#include "purr_manifest.h"
#include "purr_netrec.h"
#include "purr_slot_trailer.h"
#include "purr_trust_os.h"
#include "purr_util.h"

#define MANIFEST_MAX (16 * 1024)

static void slot_version(const char *label, char out[16])
{
    snprintf(out, 16, "?");
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    purr_image_header_t h;
    if (p != NULL && purr_slot_trailer_header(p, &h) == 0) {
        snprintf(out, 16, "%.*s", (int)sizeof(h.version), h.version);
    }
}

void purr_sysupdate_versions(char kernel[16], char kitten[16])
{
    slot_version("kernel", kernel);
    slot_version("kittenos", kitten);
}

int purr_devsrv_get(purr_repo_server_t *out)
{
    return purr_security_dev_active() ? purr_repo_dev_last(out) : -1;
}

int purr_devsrv_set(const purr_repo_server_t *s)
{
    if (!purr_security_dev_active() || s == NULL) {
        return -1;
    }
    purr_repo_dev_remember(s);
    purr_netrec_save_source(s->base, s->name, s->id);        /* the loader reads this record (and honours it only in developer mode) */
    return 0;
}

int purr_devsrv_set_typed(const char *typed, char *err, size_t errcap)
{
    purr_repo_server_t s = {0};
    if (!purr_security_dev_active()) {
        snprintf(err, errcap, "Developer mode is off.");
        return -1;
    }
    if (purr_repo_dev_resolve(typed, s.base, sizeof(s.base), err, errcap) != 0) {
        return -1;
    }
    snprintf(s.name, sizeof(s.name), "dev server");
    snprintf(s.id, sizeof(s.id), "typed");
    return purr_devsrv_set(&s);
}

static int newer(const purr_manifest_t *m, const char *component, const char *have_text, char *what, size_t cap, int *any)
{
    const purr_manifest_entry_t *e = purr_manifest_find(m, component, CONFIG_IDF_TARGET, purr_board()->name);
    uint32_t want, have;
    if (e == NULL || purr_version_parse(e->version, strlen(e->version), &want) != 0) {
        return 0;
    }
    if (purr_version_parse(have_text, strlen(have_text), &have) != 0 || want > have) {
        size_t n = strlen(what);
        snprintf(what + n, cap - n, "%s%s %s", n ? ", " : "", component, e->version);
        *any = 1;
    }
    return 0;
}

static int s_boot_newer;

int purr_sysupdate_includes_bootloader(void)
{
    return s_boot_newer;
}

int purr_sysupdate_check(int use_dev, char *msg, size_t cap)
{
    s_boot_newer = 0;
    char url[160];
    if (use_dev) {
        purr_repo_server_t sv;
        if (purr_devsrv_get(&sv) != 0) {
            snprintf(msg, cap, "No developer server is set.");
            return -1;
        }
        snprintf(url, sizeof(url), "%s/recovery/recovery.manifest", sv.base);
    } else {
        snprintf(url, sizeof(url), "%s", CONFIG_PURR_RECOVERY_MANIFEST_URL);
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (purr_fetch_alloc(url, &buf, &len, MANIFEST_MAX) != 0) {
        snprintf(msg, cap, "Could not reach the %s.", use_dev ? "developer server" : "update server");
        return -1;
    }
    purr_manifest_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        free(buf);
        snprintf(msg, cap, "Out of memory.");
        return -1;
    }
    purr_manifest_parse(m, (const char *)buf, len);
    free(buf);
    char kv[16], ov[16], what[96] = "";
    purr_sysupdate_versions(kv, ov);
    int any = 0;
    newer(m, "kernel", kv, what, sizeof(what), &any);
    newer(m, "kittenos", ov, what, sizeof(what), &any);
    {
        const purr_manifest_entry_t *be = purr_manifest_find(m, "bootloader", CONFIG_IDF_TARGET, purr_board()->name);
        purr_handoff_t hh;
        uint32_t want = 0;
        if (be != NULL && purr_version_parse(be->version, strlen(be->version), &want) == 0 && purr_kernel_handoff(&hh) == 0 &&
            want > hh.bootloader_version) {
            size_t n = strlen(what);
            snprintf(what + n, sizeof(what) - n, "%sbootloader %s", n ? ", " : "", be->version);
            s_boot_newer = 1;
            any = 1;
        }
    }
    free(m);
    if (any) {
        snprintf(msg, cap, "Newer: %s. You have PURR OS %s, KittenOS %s.", what, kv, ov);
    } else {
        snprintf(msg, cap, "Up to date: PURR OS %s, KittenOS %s.", kv, ov);
    }
    return any;
}

int purr_sysupdate_request(int use_dev, int force)
{
    purr_repo_server_t sv;
    if (use_dev) {
        if (purr_devsrv_get(&sv) != 0) {
            return -1;
        }
        purr_netrec_save_source(sv.base, sv.name, sv.id);
    } else {
        purr_netrec_save_source("", "", "");                  /* the official server */
    }
    return purr_secure_request_reinstall(force);
}
