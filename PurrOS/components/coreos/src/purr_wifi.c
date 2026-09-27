#include "purr_wifi.h"

/* No libc string functions: this also has to build for the boot package later if a
 * provisioning menu ever needs it, so it stays as dependency-free as purr_menu. */

static size_t plen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int pcopy(char *dst, size_t dstsize, const char *src)
{
    size_t n = plen(src);
    if (n >= dstsize) {
        return 0;
    }
    for (size_t i = 0; i <= n; i++) {
        dst[i] = src[i];
    }
    return 1;
}

static int peq(const char *a, const char *b)
{
    size_t i = 0;
    for (; a[i] || b[i]; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int has_bad_char(const char *s)
{
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            return 1;
        }
    }
    return 0;
}

void purr_wifi_list_init(purr_wifi_list_t *l)
{
    l->count = 0;
}

int purr_wifi_list_add(purr_wifi_list_t *l, const char *ssid, const char *pass)
{
    if (has_bad_char(ssid) || has_bad_char(pass) || ssid[0] == '\0') {
        return PURR_WIFI_INVALID;
    }
    for (int i = 0; i < l->count; i++) {
        if (peq(l->nets[i].ssid, ssid)) {
            if (!pcopy(l->nets[i].pass, sizeof(l->nets[i].pass), pass)) {
                return PURR_WIFI_INVALID;
            }
            return PURR_WIFI_UPDATED;
        }
    }
    if (l->count >= PURR_WIFI_MAX_SAVED) {
        return PURR_WIFI_FULL;
    }
    purr_wifi_net_t *n = &l->nets[l->count];
    if (!pcopy(n->ssid, sizeof(n->ssid), ssid) || !pcopy(n->pass, sizeof(n->pass), pass)) {
        return PURR_WIFI_INVALID;
    }
    l->count++;
    return PURR_WIFI_ADDED;
}

int purr_wifi_list_remove(purr_wifi_list_t *l, const char *ssid)
{
    for (int i = 0; i < l->count; i++) {
        if (peq(l->nets[i].ssid, ssid)) {
            for (int j = i; j < l->count - 1; j++) {
                l->nets[j] = l->nets[j + 1];
            }
            l->count--;
            return 1;
        }
    }
    return 0;
}

const purr_wifi_net_t *purr_wifi_list_find(const purr_wifi_list_t *l, const char *ssid)
{
    for (int i = 0; i < l->count; i++) {
        if (peq(l->nets[i].ssid, ssid)) {
            return &l->nets[i];
        }
    }
    return NULL;
}

int purr_wifi_list_format(const purr_wifi_list_t *l, char *buf, size_t bufsize)
{
    size_t pos = 0;
    for (int i = 0; i < l->count; i++) {
        const purr_wifi_net_t *n = &l->nets[i];
        size_t need = plen(n->ssid) + 1 + plen(n->pass) + 1;   /* ssid, tab, pass, newline */
        if (pos + need > bufsize) {
            return -1;
        }
        for (size_t k = 0; n->ssid[k]; k++) {
            buf[pos++] = n->ssid[k];
        }
        buf[pos++] = '\t';
        for (size_t k = 0; n->pass[k]; k++) {
            buf[pos++] = n->pass[k];
        }
        buf[pos++] = '\n';
    }
    return (int)pos;
}

void purr_wifi_list_parse(purr_wifi_list_t *l, const char *buf, size_t len)
{
    purr_wifi_list_init(l);
    size_t i = 0;
    while (i < len && l->count < PURR_WIFI_MAX_SAVED) {
        size_t start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t line_len = i - start;
        if (i < len) {
            i++;                                   /* past the newline */
        }
        if (line_len == 0) {
            continue;
        }
        size_t tab = (size_t)-1;
        for (size_t k = 0; k < line_len; k++) {
            if (buf[start + k] == '\t') {
                tab = k;
                break;
            }
        }
        if (tab == (size_t)-1 || tab == 0 || tab >= sizeof(l->nets[0].ssid) ||
            line_len - tab - 1 >= sizeof(l->nets[0].pass)) {
            continue;                               /* malformed: skipped */
        }
        purr_wifi_net_t *n = &l->nets[l->count];
        for (size_t k = 0; k < tab; k++) {
            n->ssid[k] = buf[start + k];
        }
        n->ssid[tab] = '\0';
        size_t plenn = line_len - tab - 1;
        for (size_t k = 0; k < plenn; k++) {
            n->pass[k] = buf[start + tab + 1 + k];
        }
        n->pass[plenn] = '\0';
        l->count++;
    }
}

int purr_wifi_pick(const purr_wifi_list_t *saved, const purr_wifi_seen_t *seen, int nseen)
{
    int best = -1;
    for (int i = 0; i < nseen; i++) {
        if (purr_wifi_list_find(saved, seen[i].ssid) == NULL) {
            continue;
        }
        if (best < 0 || seen[i].rssi > seen[best].rssi) {
            best = i;
        }
    }
    return best;
}
