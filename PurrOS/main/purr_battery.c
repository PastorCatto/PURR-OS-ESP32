/* purr_battery.c - see purr_battery.h. */
#include "purr_battery.h"

#include <stdint.h>

#include "esp_timer.h"

#include "purr_driver.h"

#define REFRESH_US (20LL * 1000000)

static int s_have, s_pct, s_mv;
static int64_t s_last_us;

static void refresh(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_us != 0 && now - s_last_us < REFRESH_US) {
        return;
    }
    s_last_us = now;
    uint8_t b[4];
    if (purr_device_read("battery", b, sizeof(b)) != 4 || !(b[3] & 1)) {
        s_have = 0;
        return;
    }
    int pct = b[0], mv = b[1] | (b[2] << 8);
    if (!s_have) {
        s_pct = pct;
        s_mv = mv;
    } else {
        s_pct = (s_pct * 3 + pct + 2) / 4;                 /* a slow follower: the load on the cell moves single readings by several percent */
        s_mv = (s_mv * 3 + mv + 2) / 4;
    }
    s_have = 1;
}

int purr_battery_get(int *percent, int *millivolts)
{
    refresh();
    if (!s_have) {
        return 0;
    }
    if (percent != NULL) {
        *percent = s_pct;
    }
    if (millivolts != NULL) {
        *millivolts = s_mv;
    }
    return 1;
}

int purr_battery_percent(void)
{
    int p;
    return purr_battery_get(&p, NULL) ? p : -1;
}
