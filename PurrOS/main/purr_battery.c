/* purr_battery.c - see purr_battery.h. */
#include "purr_battery.h"

#include <stdint.h>

#include "esp_timer.h"

#include "purr_battmodel.h"
#include "purr_driver.h"

#define REFRESH_US (20LL * 1000000)

static purr_battmodel_t s_model;
static int s_init;
static int64_t s_last_us;

static void refresh(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_us != 0 && now - s_last_us < REFRESH_US) {
        return;
    }
    s_last_us = now;
    if (!s_init) {
        purr_battmodel_init(&s_model);
        s_init = 1;
    }
    uint8_t b[4];
    int ok = purr_device_read("battery", b, sizeof(b)) == 4;
    if (!ok || !(b[3] & 1)) {
        purr_battmodel_feed(&s_model, 0, 0, 0, 0);
        return;
    }
    /* bit 1: the line is held above a resting cell's (USB or the charger); the rules for that are in purr_battmodel.c */
    purr_battmodel_feed(&s_model, 1, (b[3] & 2) != 0, b[0], b[1] | (b[2] << 8));
}

int purr_battery_get(int *percent, int *millivolts)
{
    refresh();
    return purr_battmodel_get(&s_model, percent, millivolts);
}

int purr_battery_charging(void)
{
    refresh();
    return purr_battmodel_charging(&s_model);
}

int purr_battery_percent(void)
{
    int p;
    return purr_battery_get(&p, NULL) ? p : -1;
}
