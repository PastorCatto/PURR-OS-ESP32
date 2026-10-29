/* purr_recovery_src.c - see purr_recovery_src.h. */
#include "purr_brand.h"
#include "purr_recovery_src.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "purr_abi.h"
#include "purr_fetch.h"
#include "purr_kernel.h"
#include "purr_manifest.h"
#include "purr_netrec.h"
#include "purr_repo.h"

static const char *TAG = "recsrc";

static char s_base[PURR_NETREC_SRC_BASE_LEN], s_name[PURR_NETREC_SRC_NAME_LEN];
static int s_loaded, s_relay;

int purr_recovery_dev_mode(void)
{
    purr_handoff_t h;
    return purr_kernel_handoff(&h) == 0 && (h.secure_state & PURR_SECST_DEV) != 0;
}

int purr_recovery_source_is_relay(void) { return s_relay && s_base[0] != '\0'; }

int purr_recovery_server_allowed(const char *id)
{
    return (id != NULL && strncmp(id, "relay-", 6) == 0) || purr_recovery_dev_allowed();
}

int purr_recovery_dev_allowed(void)
{
    purr_handoff_t h;
    return purr_kernel_handoff(&h) == 0 && (h.secure_state & PURR_SECST_DEV) != 0 && (h.secure_state & PURR_SECST_DEVREC) != 0;
}

static void load_once(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = 1;
    s_base[0] = s_name[0] = '\0';
    if (purr_recovery_dev_allowed()) {
        char id[PURR_NETREC_SRC_ID_LEN];
        if (purr_netrec_load_source(s_base, s_name, id) == 0) {
            ESP_LOGI(TAG, "recovery source: dev server \"%s\" at %s", s_name, s_base);
        }
    }
}

void purr_recovery_use(const purr_lan_server_t *server)
{
    s_loaded = 1;
    s_relay = 0;
    if (server == NULL || !purr_recovery_server_allowed(server->id)) {
        s_base[0] = s_name[0] = '\0';
        if (purr_recovery_dev_allowed()) {
            purr_netrec_save_source("", "", "");
        }
        return;
    }
    snprintf(s_base, sizeof(s_base), "%s", server->base);
    snprintf(s_name, sizeof(s_name), "%s", server->name);
    if (strncmp(server->id, "relay-", 6) == 0) {
        s_relay = 1;                                    /* not remembered: the next recovery asks the network again */
        return;
    }
    purr_netrec_save_source(server->base, server->name, server->id);
}

static const char *dev_url(char *buf, size_t cap, const char *file)
{
    snprintf(buf, cap, "%s/recovery/%s", s_base, file);
    return buf;
}

const char *purr_recovery_manifest_url(void)
{
    static char url[128];
    load_once();
    return s_base[0] ? dev_url(url, sizeof(url), "recovery.manifest") : CONFIG_PURR_RECOVERY_MANIFEST_URL;
}

const char *purr_recovery_modules_url(void)
{
    static char url[128];
    load_once();
    return s_base[0] ? dev_url(url, sizeof(url), "modules.manifest") : CONFIG_PURR_MODULE_INDEX_URL;
}

const char *purr_recovery_legal_url(void)
{
    static char url[256];
    load_once();
    if (s_base[0]) {
        return dev_url(url, sizeof(url), "legal.txt");
    }
    const char *m = CONFIG_PURR_RECOVERY_MANIFEST_URL;       /* the official server keeps legal.txt next to the manifest */
    const char *slash = strrchr(m, '/');
    size_t dir = slash ? (size_t)(slash - m) + 1 : 0;
    if (dir + sizeof("legal.txt") > sizeof(url)) {
        dir = 0;
    }
    memcpy(url, m, dir);
    snprintf(url + dir, sizeof(url) - dir, "legal.txt");
    return url;
}

const char *purr_recovery_source_name(void)
{
    load_once();
    return s_base[0] ? (s_name[0] ? s_name : s_base) : NULL;
}

int purr_recovery_verify(const char *base, char *msg, size_t cap)
{
    char why[96], url[160];
    if (!purr_repo_url_allowed(PURR_REPO_SDK, base, why, sizeof(why))) {
        snprintf(msg, cap, "That address isn't allowed: %s", why);
        return -1;
    }
    snprintf(url, sizeof(url), "%s/recovery/recovery.manifest", base);
    static uint8_t buf[16 * 1024];
    size_t len = 0;
    if (purr_fetch_local_into(url, buf, sizeof(buf), &len) != ESP_OK || len == 0) {
        snprintf(msg, cap, "Couldn't reach it, or it has no recovery files yet.");
        return -1;
    }
    purr_manifest_t man;
    purr_manifest_parse(&man, (const char *)buf, len);
    const purr_manifest_entry_t *e = purr_manifest_find(&man, "kittenos", CONFIG_IDF_TARGET, purr_board()->name);
    if (e == NULL) {
        snprintf(msg, cap, "It's up, but it has nothing for this device yet.");
        return -1;
    }
    snprintf(msg, cap, "Verified. It has " PURR_BRAND_PRODUCT " %s for this device.", e->version);
    return 0;
}

int purr_recovery_emergency(void)
{
    load_once();
    if (s_base[0] != '\0') {
        return -1;                                      /* a server is already the source and it was the one that failed */
    }
    static purr_lan_server_t found[4];
    int n = purr_lan_discover(found, 4, 4000);
    for (int i = 0; i < n; i++) {
        char msg[96];
        if (!purr_recovery_server_allowed(found[i].id)) {
            ESP_LOGW(TAG, "emergency mode: ignoring %s (a developer server, and developer recovery is off)", found[i].base);
            continue;
        }
        if (purr_recovery_verify(found[i].base, msg, sizeof(msg)) == 0) {
            ESP_LOGW(TAG, "emergency mode: using \"%s\" at %s", found[i].name, found[i].base);
            snprintf(s_base, sizeof(s_base), "%s", found[i].base);          /* for this recovery only: never remembered */
            snprintf(s_name, sizeof(s_name), "%s", found[i].name);
            s_relay = strncmp(found[i].id, "relay-", 6) == 0;
            return 0;
        }
        ESP_LOGW(TAG, "emergency mode: %s is no use: %s", found[i].base, msg);
    }
    return -1;
}
