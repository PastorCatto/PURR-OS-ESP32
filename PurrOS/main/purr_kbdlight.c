/* purr_kbdlight.c - see purr_kbdlight.h. */
#include "purr_kbdlight.h"

#include <stdint.h>
#include <stdio.h>

#include "purr_driver.h"
#include "purr_settings.h"

#define KEY "keyboard.backlight"
#define DEV "kbd-backlight"

/* The driver's control channel (Modules/drivers/kbd_backlight_module.c): read() with 0xA5 and a level in the buffer sets it. */
static const uint8_t k_level_value[4] = {0, 64, 128, 255};

static int s_screen_on = 1;

int purr_kbdlight_present(void)
{
    return purr_device_state(DEV) == PURR_DEV_BOUND;
}

int purr_kbdlight_get(void)
{
    int v = purr_settings_kv_int(KEY, 2);
    return v < 0 ? 0 : (v > 3 ? 3 : v);
}

static int apply(int level)
{
    uint8_t b[2] = {0xA5, k_level_value[level < 0 ? 0 : (level > 3 ? 3 : level)]};
    return purr_device_read(DEV, b, sizeof(b)) == 1 ? 0 : -1;
}

int purr_kbdlight_set(int level)
{
    if (!purr_kbdlight_present() || level < 0 || level > 3) {
        return -1;
    }
    char v[4];
    snprintf(v, sizeof(v), "%d", level);
    purr_settings_kv_set(KEY, v);
    return s_screen_on ? apply(level) : 0;
}

const char *purr_kbdlight_name(int level)
{
    static const char *const n[] = {"Off", "Low", "Medium", "High"};
    return level >= 0 && level <= 3 ? n[level] : "?";
}

void purr_kbdlight_boot(void)
{
    if (purr_kbdlight_present()) {
        apply(purr_kbdlight_get());
    }
}

void purr_kbdlight_screen(int on)
{
    s_screen_on = on;
    if (purr_kbdlight_present()) {
        apply(on ? purr_kbdlight_get() : 0);
    }
}
