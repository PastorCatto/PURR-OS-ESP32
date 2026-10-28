/*
 * purr_settings.h - one place that knows what can be set (plan7.md, "Theme and Settings").
 *
 * A setting has a dotted key ("display.brightness", "theme.accent", "time.zone"), a way to read it
 * and a way to change it. The pieces of the system that own a setting register it; the shell's
 * `settings` command lists, reads and changes them by key, so everything the Settings app can change
 * can also be changed from the shell, over serial, or from a script, the same way.
 *
 * Settings that belong to the whole device are kept in /etc/system.conf (key=value lines). The theme
 * keeps its own per-user file (purr_w10m_theme.c) and just registers its keys here.
 */
#ifndef PURR_SETTINGS_H
#define PURR_SETTINGS_H

#include <stddef.h>

#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;                    /* "area.name" */
    const char *help;                   /* one line */
    const char *values;                 /* what is accepted, for people: "1-255", "dark|light" */
    int admin;                          /* 1 = changing it needs an administrator (it affects everyone) */
    int (*get)(char *out, size_t cap);                        /* 0 on success */
    int (*set)(const char *value, char *err, size_t cap);     /* 0 on success, else err says why */
} purr_setting_t;

/* The table entry must outlive the call (use a static). Registering a key twice keeps the first. */
int purr_settings_register(const purr_setting_t *s);
int purr_settings_count(void);
const purr_setting_t *purr_settings_at(int index);
const purr_setting_t *purr_settings_find(const char *key);

/* Loads /etc/system.conf, applies the time zone, and registers the settings that belong here
 * (time.zone, time.clock24). Call once the filesystem is mounted, before the shell starts. */
void purr_settings_setup(void);

/* The device-wide store behind the keys that live in /etc/system.conf. */
int purr_settings_kv_get(const char *key, char *out, size_t cap);   /* 0 if the key is set */
int purr_settings_kv_set(const char *key, const char *value);       /* saves at once; 0 on success */
int purr_settings_kv_int(const char *key, int fallback);

/* "on", "off", "1", "0", "true", "false", "yes", "no". Returns 0 and sets *out, or -1. */
int purr_settings_parse_bool(const char *s, int *out);

/* Time zones the Settings app offers, by name; the setting also accepts any POSIX TZ string. */
int purr_settings_zone_count(void);
const char *purr_settings_zone_name(int index);

/* The `settings` shell command: settings [list] | get <key> | set <key> <value>. */
int purr_settings_cmd(purr_cli_t *cli, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SETTINGS_H */
