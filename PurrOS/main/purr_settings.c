/*
 * purr_settings.c - the settings registry, the device-wide store and the `settings` command
 * (purr_settings.h).
 */
#include "purr_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "commands.h"
#include "login.h"
#include "purr_fs.h"
#include "purr_trust_os.h"

static const char *TAG = "settings";

#define MAX_SETTINGS 32
#define CONF_PATH    "/etc/system.conf"
#define CONF_MAX     768

static const purr_setting_t *s_reg[MAX_SETTINGS];
static int s_nreg;

int purr_settings_register(const purr_setting_t *s)
{
    if (s == NULL || s->key == NULL || s->get == NULL) {
        return -1;
    }
    if (purr_settings_find(s->key) != NULL) {
        return 0;
    }
    if (s_nreg >= MAX_SETTINGS) {
        return -1;
    }
    s_reg[s_nreg++] = s;
    return 0;
}

int purr_settings_count(void) { return s_nreg; }
const purr_setting_t *purr_settings_at(int i) { return (i >= 0 && i < s_nreg) ? s_reg[i] : NULL; }

const purr_setting_t *purr_settings_find(const char *key)
{
    for (int i = 0; i < s_nreg; i++) {
        if (strcmp(s_reg[i]->key, key) == 0) {
            return s_reg[i];
        }
    }
    return NULL;
}

int purr_settings_parse_bool(const char *s, int *out)
{
    static const char *yes[] = {"on", "1", "true", "yes"};
    static const char *no[] = {"off", "0", "false", "no"};
    for (size_t i = 0; i < 4; i++) {
        if (strcasecmp(s, yes[i]) == 0) { *out = 1; return 0; }
        if (strcasecmp(s, no[i]) == 0) { *out = 0; return 0; }
    }
    return -1;
}

/* ---------------------------------------------------------------- the device-wide store */

typedef struct {
    char *buf;
    uint32_t cap, len;
} rd_t;

static int rd_cb(void *ctx, const void *data, uint32_t len)
{
    rd_t *r = ctx;
    if (r->len + len > r->cap) {
        len = r->cap - r->len;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return 0;
}

static char s_conf[CONF_MAX];           /* "key=value\n" lines */
static SemaphoreHandle_t s_lock;

static int s_conf_bad;                  /* the file exists but could not be read: nothing is saved over it */

static void conf_load(void)
{
    purr_fs_t *fs = purr_login_fs();
    s_conf[0] = '\0';
    s_conf_bad = 0;
    if (fs == NULL || !purr_fs_mounted(fs)) {
        return;
    }
    uint32_t n = 0;
    int r = purr_fs_slurp(fs, CONF_PATH, s_conf, sizeof(s_conf), &n);
    if (r == 1) {
        return;                          /* none yet: the defaults stand */
    }
    if (r < 0) {                         /* there but unreadable: the defaults stand for now, and a later change must not overwrite the real file */
        s_conf[0] = '\0';
        s_conf_bad = 1;
        ESP_LOGE(TAG, "%s could not be read: settings are not saved until it can be", CONF_PATH);
    }
}

/* Finds "key=" at the start of a line; returns the line start, or NULL. */
static char *conf_find(const char *key)
{
    size_t kl = strlen(key);
    for (char *p = s_conf; *p;) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            return p;
        }
        char *nl = strchr(p, '\n');
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return NULL;
}

int purr_settings_kv_get(const char *key, char *out, size_t cap)
{
    int rc = -1;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    char *p = conf_find(key);
    if (p != NULL) {
        p += strlen(key) + 1;
        size_t n = strcspn(p, "\n");
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, p, n);
        out[n] = '\0';
        rc = 0;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return rc;
}

int purr_settings_kv_int(const char *key, int fallback)
{
    char v[16];
    return purr_settings_kv_get(key, v, sizeof(v)) == 0 ? atoi(v) : fallback;
}

int purr_settings_kv_set(const char *key, const char *value)
{
    if (strchr(value, '\n') != NULL || strchr(key, '=') != NULL) {
        return -1;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_conf_bad) {
        conf_load();                    /* try again: a transient failure may have passed */
        if (s_conf_bad) {
            if (s_lock) xSemaphoreGive(s_lock);
            return -1;                  /* saving now would overwrite the file that could not be read */
        }
    }
    char *p = conf_find(key);
    if (p != NULL) {                    /* drop the old line */
        char *nl = strchr(p, '\n');
        memmove(p, nl ? nl + 1 : p + strlen(p), strlen(nl ? nl + 1 : "") + 1);
    }
    size_t used = strlen(s_conf);
    int rc = -1;
    int n = snprintf(s_conf + used, sizeof(s_conf) - used, "%s=%s\n", key, value);
    if (n > 0 && (size_t)n < sizeof(s_conf) - used) {
        rc = 0;
        purr_fs_t *fs = purr_login_fs();
        if (fs != NULL && purr_fs_mounted(fs)) {
            purr_fs_mkdir(fs, "/etc");
            if (purr_fs_write_kept(fs, CONF_PATH, s_conf, (uint32_t)strlen(s_conf)) < 0) {
                ESP_LOGW(TAG, "could not save %s", CONF_PATH);
                rc = -1;
            }
        }
    } else {
        s_conf[used] = '\0';
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return rc;
}

/* ---------------------------------------------------------------- time zone and clock format */

typedef struct {
    const char *name;
    const char *tz;                     /* POSIX TZ string */
} zone_t;

static const zone_t s_zones[] = {
    {"UTC",         "UTC0"},
    {"US-Eastern",  "EST5EDT,M3.2.0,M11.1.0"},
    {"US-Central",  "CST6CDT,M3.2.0,M11.1.0"},
    {"US-Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US-Arizona",  "MST7"},
    {"US-Pacific",  "PST8PDT,M3.2.0,M11.1.0"},
    {"BC-Pacific",  "PCT7"},            /* British Columbia since its move to year-round Pacific time: UTC-7 all year, no daylight saving (owner, 2026-11-05) */
    {"US-Alaska",   "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US-Hawaii",   "HST10"},
    {"UK",          "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central-EU",  "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern-EU",  "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"India",       "IST-5:30"},
    {"China",       "CST-8"},
    {"Japan",       "JST-9"},
    {"Sydney",      "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"NZ",          "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
#define N_ZONES ((int)(sizeof(s_zones) / sizeof(s_zones[0])))

int purr_settings_zone_count(void) { return N_ZONES; }
const char *purr_settings_zone_name(int i) { return (i >= 0 && i < N_ZONES) ? s_zones[i].name : NULL; }

static void apply_zone(const char *name_or_tz)
{
    const char *tz = name_or_tz;
    for (int i = 0; i < N_ZONES; i++) {
        if (strcasecmp(name_or_tz, s_zones[i].name) == 0) {
            tz = s_zones[i].tz;
        }
    }
    setenv("TZ", tz, 1);
    tzset();
}

static int zone_get(char *out, size_t cap)
{
    if (purr_settings_kv_get("time.zone", out, cap) != 0) {
        snprintf(out, cap, "UTC");
    }
    return 0;
}

static int zone_set(const char *v, char *err, size_t cap)
{
    int known = 0;
    for (int i = 0; i < N_ZONES; i++) {
        known |= strcasecmp(v, s_zones[i].name) == 0;
    }
    /* a raw POSIX string must at least name a zone and an offset: letters then a digit */
    if (!known && !(strlen(v) >= 4 && strlen(v) < 48 && strpbrk(v, "0123456789") != NULL &&
                    ((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z')))) {
        snprintf(err, cap, "unknown zone (try one of the names, or a POSIX TZ such as EST5EDT)");
        return -1;
    }
    if (purr_settings_kv_set("time.zone", v) != 0) {
        snprintf(err, cap, "could not save");
        return -1;
    }
    apply_zone(v);
    return 0;
}

static int clock24_get(char *out, size_t cap)
{
    snprintf(out, cap, "%s", purr_settings_kv_int("time.clock24", 1) ? "on" : "off");
    return 0;
}

static int clock24_set(const char *v, char *err, size_t cap)
{
    int b;
    if (purr_settings_parse_bool(v, &b) != 0) {
        snprintf(err, cap, "expected on or off");
        return -1;
    }
    return purr_settings_kv_set("time.clock24", b ? "1" : "0");
}

static const purr_setting_t s_zone_setting = {
    "time.zone", "time zone for the clock and dates", "a name from `settings` or a POSIX TZ string", 1,
    zone_get, zone_set};
static const purr_setting_t s_clock24_setting = {
    "time.clock24", "24-hour clock", "on|off", 0, clock24_get, clock24_set};

/* ---------------------------------------------------------------- security and developer options
 *
 * The work is in commands.c (the same code the `secure` command runs); these are the same switches by key. */

static int sec_enforce_get(char *out, size_t cap)
{
    purr_secure_status_t st;
    purr_secure_status(&st);
    snprintf(out, cap, "%s", st.mode_now == PURR_SECURE_ENFORCE ? "on" : "off");
    return 0;
}

static int sec_enforce_set(const char *v, char *err, size_t cap)
{
    int b;
    if (purr_settings_parse_bool(v, &b) != 0) {
        snprintf(err, cap, "expected on or off");
        return -1;
    }
    char msg[96];
    int rc = purr_security_set_enforce(b, msg, sizeof(msg));
    if (rc != 0) {
        snprintf(err, cap, "%s", msg);
    }
    return rc;
}

static int sec_dev_get(char *out, size_t cap)
{
    purr_secure_status_t st;
    purr_secure_status(&st);
    snprintf(out, cap, "%s", st.dev_now ? "on" : "off");
    return 0;
}

static int sec_dev_set(const char *v, char *err, size_t cap)
{
    int b;
    if (purr_settings_parse_bool(v, &b) != 0) {
        snprintf(err, cap, "expected on or off");
        return -1;
    }
    char msg[96];
    int rc = purr_security_set_dev(b, msg, sizeof(msg));
    if (rc != 0) {
        snprintf(err, cap, "%s", msg);
    }
    return rc;
}

static int unsigned_get(char *out, size_t cap)
{
    snprintf(out, cap, "%s", purr_settings_kv_int("dev.unsigned_apps", 0) ? "on" : "off");
    return 0;
}

static int unsigned_set(const char *v, char *err, size_t cap)
{
    int b;
    if (purr_settings_parse_bool(v, &b) != 0) {
        snprintf(err, cap, "expected on or off");
        return -1;
    }
    char msg[96];
    int rc = purr_security_set_unsigned_apps(b, msg, sizeof(msg));
    if (rc != 0) {
        snprintf(err, cap, "%s", msg);
    }
    return rc;
}

static int autologin_get(char *out, size_t cap)
{
    char who[32];
    purr_login_autologin_user(who, sizeof(who));
    snprintf(out, cap, "%s", who[0] ? who : "off");
    return 0;
}

static int autologin_set(const char *v, char *err, size_t cap)
{
    int off = strcasecmp(v, "off") == 0 || strcasecmp(v, "none") == 0;
    purr_acct_result_t r = purr_login_set_autologin(off ? NULL : v, NULL);   /* the shell session is already authenticated */
    if (r != PURR_ACCT_OK) {
        snprintf(err, cap, "%s", purr_login_account_error(r));
        return -1;
    }
    return 0;
}

static const purr_setting_t s_security_settings[] = {
    {"login.autologin", "account that signs in at boot with no password (a user name, or off)",
     "<user>|off", 1, autologin_get, autologin_set},
    {"security.enforce", "refuse unsigned or altered system images at boot (next boot)", "on|off", 1,
     sec_enforce_get, sec_enforce_set},
    {"security.devmode", "developer mode: boot with a warning instead of refusing (next boot)", "on|off", 1,
     sec_dev_get, sec_dev_set},
    {"dev.unsigned_apps", "run unsigned apps from your own folder; needs developer mode active this boot",
     "on|off", 1, unsigned_get, unsigned_set},
};

void purr_settings_setup(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    conf_load();
    char tz[48];
    if (purr_settings_kv_get("time.zone", tz, sizeof(tz)) == 0) {
        apply_zone(tz);
    }
    for (size_t i = 0; i < sizeof(s_security_settings) / sizeof(s_security_settings[0]); i++) {
        purr_settings_register(&s_security_settings[i]);
    }
    purr_settings_register(&s_zone_setting);
    purr_settings_register(&s_clock24_setting);
}

/* ---------------------------------------------------------------- the `settings` command */

static int may_change(const purr_setting_t *s)
{
    return !s->admin || purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN;
}

static void list_all(purr_cli_t *cli)
{
    for (int i = 0; i < s_nreg; i++) {
        char v[64];
        if (s_reg[i]->get(v, sizeof(v)) != 0) {
            snprintf(v, sizeof(v), "?");
        }
        purr_cli_printf(cli, "%-20s %-12s %s%s\n", s_reg[i]->key, v, s_reg[i]->help,
                        s_reg[i]->admin ? " (admin)" : "");
        purr_cli_printf(cli, "%-20s %-12s [%s]\n", "", "", s_reg[i]->values);
    }
}

int purr_settings_cmd(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        if (s_nreg == 0) {
            purr_cli_puts(cli, "no settings registered\n");
        } else {
            list_all(cli);
        }
        return 0;
    }
    if (strcmp(argv[1], "get") == 0 && argc == 3) {
        const purr_setting_t *s = purr_settings_find(argv[2]);
        char v[64];
        if (s == NULL || s->get(v, sizeof(v)) != 0) {
            purr_cli_printf(cli, "settings: no such setting: %s\n", argv[2]);
            return 1;
        }
        purr_cli_printf(cli, "%s\n", v);
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc >= 4) {
        const purr_setting_t *s = purr_settings_find(argv[2]);
        if (s == NULL || s->set == NULL) {
            purr_cli_printf(cli, "settings: %s: %s\n", argv[2], s ? "read-only" : "no such setting");
            return 1;
        }
        if (!may_change(s)) {
            purr_cli_printf(cli, "settings: %s needs an administrator\n", argv[2]);
            return 1;
        }
        char val[64] = "";
        for (int i = 3; i < argc; i++) {              /* the value may hold spaces */
            size_t used = strlen(val);
            snprintf(val + used, sizeof(val) - used, "%s%s", i > 3 ? " " : "", argv[i]);
        }
        char err[96] = "";
        if (s->set(val, err, sizeof(err)) != 0) {
            purr_cli_printf(cli, "settings: %s: %s\n", argv[2], err[0] ? err : "not accepted");
            return 1;
        }
        char now[64];
        s->get(now, sizeof(now));
        purr_cli_printf(cli, "%s = %s\n", argv[2], now);
        return 0;
    }
    purr_cli_puts(cli, "usage: settings [list] | settings get <key> | settings set <key> <value>\n");
    return 1;
}
