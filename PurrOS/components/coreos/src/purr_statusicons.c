/* purr_statusicons.c - see purr_statusicons.h. */
#include "purr_statusicons.h"

static const char *const k_battery[PURR_SI_BATTERY_LEVELS] = {"st_b0", "st_b10", "st_b20", "st_b40", "st_b60", "st_b80", "st_b100"};
static const char *const k_charge[PURR_SI_CHARGE_FRAMES] = {"st_bc0", "st_bc1", "st_bc2", "st_bc3", "st_bc4", "st_bc5"};
static const char *const k_wifi[5] = {"st_w0", "st_w1", "st_w2", "st_w3", "st_w4"};
static const char *const k_service[5] = {"st_s0", "st_s1", "st_s2", "st_s3", "st_s4"};

int purr_si_battery_level(int percent)
{
    if (percent <= 4) {
        return 0;
    }
    if (percent <= 14) {
        return 1;
    }
    if (percent <= 29) {
        return 2;
    }
    if (percent <= 49) {
        return 3;
    }
    if (percent <= 69) {
        return 4;
    }
    if (percent <= 89) {
        return 5;
    }
    return 6;
}

const char *purr_si_battery_asset(int percent) { return k_battery[purr_si_battery_level(percent)]; }

const char *purr_si_battery_unknown_asset(void) { return "st_bunk"; }

int purr_si_charge_start(int percent)
{
    if (percent <= 14) {
        return 0;
    }
    if (percent <= 29) {
        return 1;
    }
    if (percent <= 49) {
        return 2;
    }
    if (percent <= 69) {
        return 3;
    }
    if (percent <= 89) {
        return 4;
    }
    return 5;
}

int purr_si_charge_next(int frame, int start)
{
    if (start < 0 || start >= PURR_SI_CHARGE_FRAMES) {
        start = 0;
    }
    return frame >= PURR_SI_CHARGE_FRAMES - 1 || frame < start ? start : frame + 1;
}

int purr_si_charge_ms(int frame, int start) { return frame == start ? 2000 : 1000; }

const char *purr_si_charge_asset(int frame)
{
    return k_charge[frame < 0 ? 0 : (frame >= PURR_SI_CHARGE_FRAMES ? PURR_SI_CHARGE_FRAMES - 1 : frame)];
}

int purr_si_wifi_level(int rssi_dbm)
{
    if (rssi_dbm <= -100) {
        return 0;
    }
    if (rssi_dbm >= -55) {
        return 4;
    }
    return (rssi_dbm + 100) / 11;           /* (-55 - -100) / 4 = 11 */
}

const char *purr_si_wifi_asset(int level) { return k_wifi[level < 0 ? 0 : (level > 4 ? 4 : level)]; }

int purr_si_lora_level(int heard, int rssi_dbm, uint32_t age_ms)
{
    if (!heard || age_ms > PURR_SI_LORA_STALE_MS) {
        return 0;
    }
    return rssi_dbm >= -85 ? 4 : rssi_dbm >= -100 ? 3 : rssi_dbm >= -112 ? 2 : 1;
}

const char *purr_si_service_asset(int bars)
{
    return bars < 0 ? "st_snull" : k_service[bars > 4 ? 4 : bars];
}
