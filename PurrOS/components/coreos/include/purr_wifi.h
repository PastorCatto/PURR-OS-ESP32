/*
 * purr_wifi.h - saved Wi-Fi networks and which one to try (Network/SPEC.md).
 *
 * Plain data and logic, no radio and no filesystem: the kernel reads and writes the
 * saved list as a file (plain text in /etc/wifi, per the spec) and drives the radio.
 * This is what makes that logic testable on a PC.
 */
#ifndef PURR_WIFI_H
#define PURR_WIFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_WIFI_SSID_LEN 33     /* 32 characters + a null */
#define PURR_WIFI_PASS_LEN 65     /* 64 characters + a null */
#define PURR_WIFI_MAX_SAVED 8

typedef struct {
    char ssid[PURR_WIFI_SSID_LEN];
    char pass[PURR_WIFI_PASS_LEN];
} purr_wifi_net_t;

typedef struct {
    purr_wifi_net_t nets[PURR_WIFI_MAX_SAVED];
    int count;
} purr_wifi_list_t;

void purr_wifi_list_init(purr_wifi_list_t *l);

enum {
    PURR_WIFI_ADDED = 1,
    PURR_WIFI_UPDATED = 2,
    PURR_WIFI_INVALID = -1,       /* a tab or newline in ssid or pass */
    PURR_WIFI_FULL = -2,          /* new network, but the list has no room */
};

/* Adds a network, or replaces the password if the ssid is already saved. */
int purr_wifi_list_add(purr_wifi_list_t *l, const char *ssid, const char *pass);

/* Returns 1 if it was there and is now removed, 0 if it was not saved. */
int purr_wifi_list_remove(purr_wifi_list_t *l, const char *ssid);

const purr_wifi_net_t *purr_wifi_list_find(const purr_wifi_list_t *l, const char *ssid);

/*
 * The save file is plain text, one network per line: ssid, a tab, the password. Used
 * for /etc/wifi (Network/SPEC.md section 1); real protection needs flash encryption.
 */
int purr_wifi_list_format(const purr_wifi_list_t *l, char *buf, size_t bufsize);   /* bytes written, or -1 */
void purr_wifi_list_parse(purr_wifi_list_t *l, const char *buf, size_t len);       /* malformed lines are skipped */

typedef struct {
    char ssid[PURR_WIFI_SSID_LEN];
    int rssi;
} purr_wifi_seen_t;

/*
 * Of the networks seen in a scan, which saved one to try: the strongest signal among
 * those that are both saved and in range. Returns the index into `seen`, or -1 if none
 * of the networks in range are saved.
 */
int purr_wifi_pick(const purr_wifi_list_t *saved, const purr_wifi_seen_t *seen, int nseen);

#ifdef __cplusplus
}
#endif

#endif /* PURR_WIFI_H */
