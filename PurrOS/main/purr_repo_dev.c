/*
 * purr_repo_dev.c - the device's repository client (purr_repo_dev.h).
 */
#include "purr_repo_dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "commands.h"
#include "purr_fetch.h"
#include "purr_lan_discover.h"
#include "purr_settings.h"

static const char *TAG = "repo";

#define DISCOVERY_PORT 47800
#define DEFAULT_PORT   47801

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", msg);
    }
}

int purr_repo_dev_resolve(const char *typed, char *out, size_t cap, char *err, size_t errcap)
{
    if (!purr_security_dev_active()) {
        set_err(err, errcap, "developer mode is not active this boot");
        return -1;
    }
    char url[96];
    if (strncmp(typed, "http://", 7) == 0 || strncmp(typed, "https://", 8) == 0) {
        snprintf(url, sizeof(url), "%s", typed);
    } else {
        snprintf(url, sizeof(url), "http://%s", typed);
    }
    /* a bare host gets the default port */
    char scheme[8], host[96], path[96];
    uint16_t port;
    if (purr_repo_url_split(url, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)) != 0) {
        set_err(err, errcap, "that is not an address (try 10.0.2.2:47801)");
        return -1;
    }
    if (strchr(typed, ':') == NULL || (strncmp(typed, "http", 4) == 0 && strchr(typed + 8, ':') == NULL)) {
        port = DEFAULT_PORT;
    }
    char why[96];
    if (!purr_repo_url_allowed(PURR_REPO_SDK, url, why, sizeof(why))) {
        set_err(err, errcap, why);
        return -1;
    }
    /* Resolve the name once, check the ADDRESS, and use the address from here on, so the address that
     * passed the check is the one that gets connected to. */
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        set_err(err, errcap, "could not find that computer on the network");
        return -1;
    }
    uint32_t ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    if (!purr_repo_ip_is_local(ip)) {
        set_err(err, errcap, "that name points outside the local network; refused");
        return -1;
    }
    int n = snprintf(out, cap, "%s://%u.%u.%u.%u:%u", scheme, (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
                     (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), (unsigned)port);
    if (n <= 0 || (size_t)n >= cap) {
        set_err(err, errcap, "address too long");
        return -1;
    }
    return 0;
}

int purr_repo_dev_discover(purr_repo_server_t *out, int max, int wait_ms)
{
    if (!purr_security_dev_active() || max <= 0) {
        return 0;
    }
    purr_lan_server_t found[4];
    int n = purr_lan_discover(found, max < 4 ? max : 4, wait_ms);     /* probes, and listens for announcements */
    for (int i = 0; i < n; i++) {
        snprintf(out[i].base, sizeof(out[i].base), "%s", found[i].base);
        snprintf(out[i].name, sizeof(out[i].name), "%s", found[i].name);
        snprintf(out[i].id, sizeof(out[i].id), "%s", found[i].id);
    }
    return n;
}

int purr_repo_dev_index(const char *base, purr_repo_index_t *idx, char *err, size_t errcap)
{
    if (!purr_security_dev_active()) {
        set_err(err, errcap, "developer mode is not active this boot");
        return -1;
    }
    char url[160], why[96];
    if (!purr_repo_url_allowed(PURR_REPO_SDK, base, why, sizeof(why))) {
        set_err(err, errcap, why);
        return -1;
    }
    if (purr_repo_url_join(base, "index.json", url, sizeof(url)) != 0) {
        set_err(err, errcap, "bad server address");
        return -1;
    }
    uint8_t *buf = heap_caps_malloc(PURR_REPO_MAX_INDEX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(PURR_REPO_MAX_INDEX + 1);
    }
    if (buf == NULL) {
        set_err(err, errcap, "out of memory");
        return -1;
    }
    size_t got = 0;
    esp_err_t e = purr_fetch_local_into(url, buf, PURR_REPO_MAX_INDEX, &got);
    int rc = -1;
    if (e != ESP_OK) {
        snprintf(err, errcap, "could not reach the server (%s)", esp_err_to_name(e));
    } else {
        purr_repo_kind_t kind = PURR_REPO_SDK;
        rc = purr_repo_index_parse((const char *)buf, got, idx, &kind, err, errcap);
    }
    free(buf);
    return rc;
}

int purr_repo_dev_install(const char *base, const purr_repo_app_t *app, char *msg, size_t cap)
{
    if (!purr_security_dev_active()) {
        snprintf(msg, cap, "developer mode is not active this boot");
        return -1;
    }
    char url[200], why[96];
    if (!purr_repo_url_allowed(PURR_REPO_SDK, base, why, sizeof(why))) {
        snprintf(msg, cap, "%s", why);
        return -1;
    }
    if (purr_repo_url_join(base, app->url, url, sizeof(url)) != 0) {
        snprintf(msg, cap, "bad package address");
        return -1;
    }
    ESP_LOGI(TAG, "installing %s %s from %s", app->name, app->version, base);
    return purr_devapps_install_url(url, app, msg, cap);
}

/* ---------------------------------------------------------------- the server the owner chose */

int purr_repo_dev_last(purr_repo_server_t *out)
{
    char v[160];
    if (purr_settings_kv_get("dev.repo", v, sizeof(v)) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char *bar = strchr(v, '|');
    if (bar == NULL) {
        return -1;
    }
    *bar++ = '\0';
    snprintf(out->base, sizeof(out->base), "%.79s", v);
    char *bar2 = strchr(bar, '|');
    if (bar2 != NULL) {
        *bar2++ = '\0';
        snprintf(out->name, sizeof(out->name), "%.31s", bar2);
    }
    snprintf(out->id, sizeof(out->id), "%.23s", bar);
    return 0;
}

void purr_repo_dev_remember(const purr_repo_server_t *s)
{
    char v[160];
    snprintf(v, sizeof(v), "%s|%s|%s", s->base, s->id, s->name);
    purr_settings_kv_set("dev.repo", v);
    if (s->id[0] != '\0') {
        char key[40];
        snprintf(key, sizeof(key), "dev.trust.%s", s->id);
        purr_settings_kv_set(key, "1");
    }
}

int purr_repo_dev_trusted(const char *id)
{
    char key[40];
    snprintf(key, sizeof(key), "dev.trust.%s", id);
    return id[0] != '\0' && purr_settings_kv_int(key, 0) == 1;
}
