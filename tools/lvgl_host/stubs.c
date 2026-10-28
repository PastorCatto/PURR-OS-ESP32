/* Just enough of the platform for the theme, the widgets and the OOBE pages to run on the host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "commands.h"
#include "purr_settings.h"
#include "purr_ui_service.h"

purr_fs_t *purr_login_fs(void) { return NULL; }

esp_err_t purr_ui_service_run(void *(*fn)(void *), void *arg, void **result)
{
    void *r = fn(arg);
    if (result) *result = r;
    return 0;
}

static char s_kv[16][2][64];
static int s_nkv;
int purr_settings_kv_get(const char *key, char *out, size_t cap)
{
    for (int i = 0; i < s_nkv; i++)
        if (!strcmp(s_kv[i][0], key)) { snprintf(out, cap, "%s", s_kv[i][1]); return 0; }
    return -1;
}
int purr_settings_kv_set(const char *key, const char *value)
{
    for (int i = 0; i < s_nkv; i++)
        if (!strcmp(s_kv[i][0], key)) { snprintf(s_kv[i][1], 64, "%s", value); return 0; }
    if (s_nkv < 16) { snprintf(s_kv[s_nkv][0], 64, "%s", key); snprintf(s_kv[s_nkv][1], 64, "%s", value); s_nkv++; return 0; }
    return -1;
}
int purr_settings_kv_int(const char *key, int fallback)
{
    char v[64];
    return purr_settings_kv_get(key, v, sizeof(v)) == 0 ? atoi(v) : fallback;
}
int purr_settings_parse_bool(const char *s, int *out)
{
    if (!strcasecmp(s, "1") || !strcasecmp(s, "on") || !strcasecmp(s, "true")) { *out = 1; return 0; }
    if (!strcasecmp(s, "0") || !strcasecmp(s, "off") || !strcasecmp(s, "false")) { *out = 0; return 0; }
    return -1;
}

static const purr_setting_t *s_reg[16];
static int s_nreg;
int purr_settings_register(const purr_setting_t *s) { if (s_nreg < 16) s_reg[s_nreg++] = s; return 0; }
int purr_settings_count(void) { return s_nreg; }
const purr_setting_t *purr_settings_at(int i) { return i >= 0 && i < s_nreg ? s_reg[i] : NULL; }
const purr_setting_t *purr_settings_find(const char *key)
{
    for (int i = 0; i < s_nreg; i++) if (!strcmp(s_reg[i]->key, key)) return s_reg[i];
    return NULL;
}

static const char *const k_zones[] = {"UTC", "Eastern Time (US)", "Central Time (US)", "Mountain Time (US)",
                                      "Pacific Time (US)", "London", "Berlin", "Tokyo"};
int purr_settings_zone_count(void) { return (int)(sizeof(k_zones) / sizeof(k_zones[0])); }
const char *purr_settings_zone_name(int i) { return i >= 0 && i < purr_settings_zone_count() ? k_zones[i] : NULL; }

static int zone_get(char *o, size_t c) { return purr_settings_kv_get("time.zone", o, c); }
static int zone_set(const char *v, char *e, size_t c) { (void)e; (void)c; return purr_settings_kv_set("time.zone", v); }
static const purr_setting_t k_zone = {"time.zone", "time zone", "", 0, zone_get, zone_set};
void host_register_settings(void) { purr_settings_register(&k_zone); }
