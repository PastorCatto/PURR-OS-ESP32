/* purr_clock.c - see purr_clock.h. */
#include "purr_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include "commands.h"
#include "purr_fs.h"
#include "purr_settings.h"

static const char *TAG = "clock";

#define SAVED_PATH "/etc/lasttime"
#define DEFAULT_SERVER "pool.ntp.org"
#define SAVE_EVERY_S (10 * 60)                   /* between saves of the time, so the flash is not worn for nothing */

static int s_started;                            /* the SNTP client is running */
static int s_online;                             /* an address has been handed out since boot */
static int64_t s_synced_us;                      /* esp_timer time of the last sync, 0 = none yet */
static time_t s_last_saved;
static char s_server[48];

static int rd_cb(void *ctx, const void *data, uint32_t n)
{
    char *buf = ctx;
    size_t have = strlen(buf);
    if (have + n >= 24) {
        return -1;
    }
    memcpy(buf + have, data, n);
    buf[have + n] = '\0';
    return 0;
}

static void save_time(void)
{
    time_t now = time(NULL);
    if (now < 1000000000) {
        return;
    }
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs)) {
        return;
    }
    char t[24];
    int n = snprintf(t, sizeof(t), "%lld\n", (long long)now);
    purr_fs_mkdir(fs, "/etc");
    if (purr_fs_write(fs, SAVED_PATH, t, (uint32_t)n) >= 0) {
        s_last_saved = now;
    }
}

static void restore_time(void)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs) || time(NULL) >= 1000000000) {
        return;
    }
    char t[24] = "";
    if (purr_fs_read(fs, SAVED_PATH, rd_cb, t) < 0) {
        return;
    }
    long long v = atoll(t);
    if (v > 1700000000LL) {                      /* a plausible date; the network replaces it when there is one */
        struct timeval tv = {.tv_sec = (time_t)v, .tv_usec = 0};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "no network time yet: using the last known time (%lld)", v);
    }
}

static void synced_cb(struct timeval *tv)
{
    (void)tv;
    s_synced_us = esp_timer_get_time();
    ESP_LOGI(TAG, "time synced from %s", s_server);
    save_time();
}

int purr_clock_ntp_enabled(void)
{
    return purr_settings_kv_int("time.ntp", 1) != 0;
}

static void start_sntp(void)
{
    if (!purr_clock_ntp_enabled() || !s_online) {
        return;
    }
    if (s_started) {
        esp_sntp_stop();
        s_started = 0;
    }
    if (purr_settings_kv_get("time.ntp_server", s_server, sizeof(s_server)) != 0 || s_server[0] == '\0') {
        snprintf(s_server, sizeof(s_server), "%s", DEFAULT_SERVER);
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_server);
    sntp_set_time_sync_notification_cb(synced_cb);
    esp_sntp_init();
    s_started = 1;
}

void purr_clock_set_ntp_enabled(int on)
{
    purr_settings_kv_set("time.ntp", on ? "1" : "0");
    if (on) {
        start_sntp();
    } else if (s_started) {
        esp_sntp_stop();
        s_started = 0;
    }
}

void purr_clock_sync_now(void)
{
    if (s_online && purr_clock_ntp_enabled()) {
        start_sntp();                             /* stopping and starting again asks at once */
    }
}

static void ip_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_online = 1;
    start_sntp();
}

void purr_clock_boot(void)
{
    restore_time();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_cb, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_cb, NULL);
}

void purr_clock_status(char *out, size_t cap)
{
    if (!purr_clock_ntp_enabled()) {
        snprintf(out, cap, "Automatic time is off.");
    } else if (s_synced_us != 0) {
        int64_t min = (esp_timer_get_time() - s_synced_us) / 60000000LL;
        if (min < 1) {
            snprintf(out, cap, "Synced just now (%s).", s_server);
        } else if (min < 120) {
            snprintf(out, cap, "Synced %d min ago (%s).", (int)min, s_server);
        } else {
            snprintf(out, cap, "Synced %d h ago (%s).", (int)(min / 60), s_server);
        }
    } else if (!s_online) {
        snprintf(out, cap, "Waiting for the network...");
    } else {
        snprintf(out, cap, "Not synced yet.");
    }
}
