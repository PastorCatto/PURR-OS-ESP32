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
#include "purr_repo_trust.h"
#include "purr_move.h"
#include "purr_crypto_mbedtls.h"
#include "commands.h"

static const char *TAG = "recsrc";

static char s_base[PURR_NETREC_SRC_BASE_LEN], s_name[PURR_NETREC_SRC_NAME_LEN];
static int s_loaded, s_relay;
/* The official source a signed move file set (purr_move.h): empty while the compiled default is in use. */
static char s_off_rec[PURR_NETREC_OFF_URL_LEN], s_off_mod[PURR_NETREC_OFF_URL_LEN], s_off_msg[PURR_NETREC_OFF_MSG_LEN];
static uint32_t s_off_seq;

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
    char why[48];
    if (purr_netrec_load_official(s_off_rec, s_off_mod, s_off_msg, &s_off_seq) == 0 && s_off_rec[0] != '\0') {
        if (!purr_repo_url_allowed(PURR_REPO_STORE, s_off_rec, why, sizeof(why)) || !purr_repo_url_allowed(PURR_REPO_STORE, s_off_mod, why, sizeof(why))) {
            s_off_rec[0] = s_off_mod[0] = '\0';                   /* a damaged record: the compiled default */
        } else {
            ESP_LOGI(TAG, "official source moved (move %u): %s", (unsigned)s_off_seq, s_off_rec);
        }
    }
}

/* The official addresses: what a signed move set, else the compiled default. A dev server does not change these. */
const char *purr_recovery_official_manifest_url(void)
{
    load_once();
    return s_off_rec[0] ? s_off_rec : CONFIG_PURR_RECOVERY_MANIFEST_URL;
}

const char *purr_recovery_official_modules_url(void)
{
    load_once();
    return s_off_mod[0] ? s_off_mod : CONFIG_PURR_MODULE_INDEX_URL;
}

int purr_recovery_official_moved(void)
{
    load_once();
    return s_off_rec[0] != '\0';
}

uint32_t purr_recovery_move_seq(void)
{
    load_once();
    return s_off_seq;
}

void purr_recovery_move_clear(void)
{
    load_once();
    purr_netrec_save_official("", "", "", s_off_seq);             /* back to the compiled default; the seq stays, so an old move file cannot come back */
    s_off_rec[0] = s_off_mod[0] = s_off_msg[0] = '\0';
}

static void file_next_to(const char *manifest_url, const char *file, char *out, size_t cap)
{
    const char *slash = strrchr(manifest_url, '/');
    size_t dir = slash ? (size_t)(slash - manifest_url) + 1 : 0;
    if (dir + strlen(file) + 1 > cap) {
        dir = 0;
    }
    memcpy(out, manifest_url, dir);
    snprintf(out + dir, cap - dir, "%s", file);
}

int purr_recovery_check_moved(char *message, size_t cap)
{
    if (message != NULL && cap > 0) {
        message[0] = '\0';
    }
    load_once();
    if (s_base[0] != '\0') {
        return 0;                                                 /* a dev server is in use: nothing to move */
    }
    const char *asked = purr_recovery_official_manifest_url();
    char url[200], sig_hex[200];
    static uint8_t text[PURR_MOVE_MAX_TEXT + 1];
    size_t n = 0, sn = 0;
    file_next_to(asked, "moved.txt", url, sizeof(url));
    if (purr_fetch_into(url, text, sizeof(text), &n) != ESP_OK || n == 0 || n > PURR_MOVE_MAX_TEXT) {
        return 0;                                                 /* no such file: the normal case */
    }
    file_next_to(asked, "moved.txt.sig", url, sizeof(url));
    uint8_t sig[PURR_SIG_LEN];
    if (purr_fetch_into(url, (uint8_t *)sig_hex, sizeof(sig_hex) - 1, &sn) != ESP_OK || sn == 0 || purr_repo_sig_decode(sig_hex, sn, sig) != 0) {
        ESP_LOGW(TAG, "a move file is there but its signature is not");
        return 0;
    }
    purr_move_t m;
    purr_move_result_t r = purr_move_check(&purr_crypto_mbedtls, purr_os_keybag(), (const char *)text, n, sig, asked, s_off_seq, &m);
    if (r != PURR_MOVE_OK) {
        ESP_LOGW(TAG, "a move file was not followed: %s", purr_move_result_name(r));
        return 0;
    }
    if (purr_move_is_default(&m, CONFIG_PURR_RECOVERY_MANIFEST_URL, CONFIG_PURR_MODULE_INDEX_URL)) {
        purr_netrec_save_official("", "", m.message, m.seq);
        s_off_rec[0] = s_off_mod[0] = '\0';
    } else {
        purr_netrec_save_official(m.recovery, m.modules, m.message, m.seq);
        snprintf(s_off_rec, sizeof(s_off_rec), "%s", m.recovery);
        snprintf(s_off_mod, sizeof(s_off_mod), "%s", m.modules);
    }
    snprintf(s_off_msg, sizeof(s_off_msg), "%s", m.message);
    s_off_seq = m.seq;
    ESP_LOGI(TAG, "followed move %u: updates now come from %s", (unsigned)m.seq, s_off_rec[0] ? s_off_rec : "the default address");
    if (message != NULL && cap > 0) {
        snprintf(message, cap, "%s", m.message[0] ? m.message : "Your updates now come from a new address.");
    }
    return 1;
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
    return s_base[0] ? dev_url(url, sizeof(url), "recovery.manifest") : purr_recovery_official_manifest_url();
}

const char *purr_recovery_modules_url(void)
{
    static char url[128];
    load_once();
    return s_base[0] ? dev_url(url, sizeof(url), "modules.manifest") : purr_recovery_official_modules_url();
}

const char *purr_recovery_legal_url(void)
{
    static char url[256];
    load_once();
    if (s_base[0]) {
        return dev_url(url, sizeof(url), "legal.txt");
    }
    const char *m = purr_recovery_official_manifest_url();   /* the official server keeps legal.txt next to the manifest */
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
