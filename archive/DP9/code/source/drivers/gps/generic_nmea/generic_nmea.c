// Generic NMEA GPS driver — UART, parses $GPRMC and $GPGGA
// Used on: tdeck_plus
//
// $GPRMC's date/time fields feed purr_kernel_time_set(PURR_TIME_SOURCE_GPS,
// ...) directly from this driver (see parse_gprmc()) — a receiver can
// decode UTC before it has a position lock, so this can sync the clock
// before gps_fix_t.valid ever goes true. See catcall_gps.h v2.

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_gps.h"

static const char *TAG = "generic_nmea";

#ifndef CONFIG_GPS_TX_PIN
#define CONFIG_GPS_TX_PIN 43
#endif
#ifndef CONFIG_GPS_RX_PIN
#define CONFIG_GPS_RX_PIN 44
#endif
#ifndef CONFIG_GPS_BAUD
#define CONFIG_GPS_BAUD 9600
#endif
#ifndef CONFIG_GPS_UART_PORT
#define CONFIG_GPS_UART_PORT UART_NUM_1
#endif

static gps_fix_t       s_fix;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t    s_task;
static bool            s_running;

// Convert DDDMM.MMMM to decimal degrees
static double nmea_to_deg(const char *str) {
    double raw = atof(str);
    int deg = (int)(raw / 100);
    double min = raw - deg * 100.0;
    return deg + min / 60.0;
}

// Parse comma-separated field n from sentence into buf
static bool get_field(const char *sentence, int n, char *buf, size_t buflen) {
    const char *p = sentence;
    for (int i = 0; i < n; i++) {
        p = strchr(p, ',');
        if (!p) return false;
        p++;
    }
    const char *end = strchr(p, ',');
    if (!end) end = strchr(p, '*');
    if (!end) end = p + strlen(p);
    size_t len = (size_t)(end - p);
    if (len >= buflen) len = buflen - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return len > 0;
}

// Two-digit ASCII pair -> int, e.g. "16" -> 16. No bounds validation beyond
// what atoi already gives us — get_field()'s length check on the caller side
// is what actually guards against a short/malformed field.
static uint16_t two_digits(const char *p) {
    char b[3] = { p[0], p[1], '\0' };
    return (uint16_t)atoi(b);
}

static void parse_gprmc(const char *sentence) {
    char status[4] = "";
    get_field(sentence, 2, status, sizeof(status));

    // Time/date (fields 1 and 9) are independent of the position fix — a
    // receiver can decode UTC from the almanac before it has enough
    // satellites for a position lock — so these are parsed regardless of
    // `status`, ahead of the early-return below. A receiver with no fix at
    // all yet typically leaves these fields blank rather than zeroed, which
    // is what have_time/have_date are actually gating on.
    char time_f[16] = "", date_f[8] = "";
    get_field(sentence, 1, time_f, sizeof(time_f));
    get_field(sentence, 9, date_f, sizeof(date_f));
    bool have_time = strlen(time_f) >= 6;
    bool have_date = strlen(date_f) == 6;

    if (have_time && have_date) {
        uint8_t  hh = (uint8_t)two_digits(time_f);
        uint8_t  mm = (uint8_t)two_digits(time_f + 2);
        uint8_t  ss = (uint8_t)two_digits(time_f + 4);
        uint8_t  dd = (uint8_t)two_digits(date_f);
        uint8_t  mo = (uint8_t)two_digits(date_f + 2);
        uint16_t yy = (uint16_t)(2000 + two_digits(date_f + 4));   // NMEA gives a 2-digit year

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_fix.hour = hh; s_fix.minute = mm; s_fix.second = ss;
        s_fix.day  = dd; s_fix.month  = mo; s_fix.year   = yy;
        s_fix.valid_time = true;
        xSemaphoreGive(s_mutex);

        // Outside the mutex — NVS I/O in purr_kernel_time_set() has no
        // business holding up the UART reader task that owns s_fix.
        purr_kernel_time_set(PURR_TIME_SOURCE_GPS,
            purr_kernel_time_from_utc_calendar(yy, mo, dd, hh, mm, ss));
    }

    if (status[0] != 'A') {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_fix.valid = false;
        xSemaphoreGive(s_mutex);
        return;
    }
    char lat[16] = "", ns[4] = "", lon[16] = "", ew[4] = "", spd[16] = "";
    get_field(sentence, 3, lat, sizeof(lat));
    get_field(sentence, 4, ns, sizeof(ns));
    get_field(sentence, 5, lon, sizeof(lon));
    get_field(sentence, 6, ew, sizeof(ew));
    get_field(sentence, 7, spd, sizeof(spd));

    double latitude  = nmea_to_deg(lat);
    double longitude = nmea_to_deg(lon);
    if (ns[0] == 'S') latitude  = -latitude;
    if (ew[0] == 'W') longitude = -longitude;
    float speed_mps = (float)(atof(spd) * 0.51444);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_fix.latitude  = latitude;
    s_fix.longitude = longitude;
    s_fix.speed_mps = speed_mps;
    s_fix.valid     = true;
    xSemaphoreGive(s_mutex);
}

static void parse_gpgga(const char *sentence) {
    char quality[4] = "", sats[4] = "", hdop[16] = "", alt[16] = "";
    get_field(sentence, 6, quality, sizeof(quality));
    if (atoi(quality) == 0) return;
    get_field(sentence, 7, sats, sizeof(sats));
    get_field(sentence, 8, hdop, sizeof(hdop));
    get_field(sentence, 9, alt, sizeof(alt));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_fix.satellites = (uint8_t)atoi(sats);
    s_fix.hdop       = (float)atof(hdop);
    s_fix.altitude_m = (float)atof(alt);
    xSemaphoreGive(s_mutex);
}

static void nmea_task(void *arg) {
    (void)arg;
    char line[128];
    int  pos = 0;
    uint8_t ch;
    while (s_running) {
        int n = uart_read_bytes(CONFIG_GPS_UART_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (n != 1) continue;
        if (ch == '\r') continue;
        if (ch == '\n' || pos >= (int)sizeof(line) - 1) {
            line[pos] = '\0';
            if (strncmp(line, "$GPRMC", 6) == 0) parse_gprmc(line);
            else if (strncmp(line, "$GPGGA", 6) == 0) parse_gpgga(line);
            pos = 0;
        } else {
            line[pos++] = (char)ch;
        }
    }
    vTaskDelete(NULL);
}

static const catcall_gps_t s_catcall;

static esp_err_t nmea_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(&s_fix, 0, sizeof(s_fix));

    uart_config_t uart_cfg = {
        .baud_rate  = CONFIG_GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CONFIG_GPS_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_GPS_UART_PORT,
        CONFIG_GPS_TX_PIN, CONFIG_GPS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_GPS_UART_PORT, 512, 0, 0, NULL, 0));

    s_running = true;
    xTaskCreate(nmea_task, "gps_nmea", 2048, NULL, 5, &s_task);

    ESP_LOGI(TAG, "generic_nmea GPS ready (UART%d TX=%d RX=%d %dbaud)",
             CONFIG_GPS_UART_PORT, CONFIG_GPS_TX_PIN, CONFIG_GPS_RX_PIN, CONFIG_GPS_BAUD);
    purr_kernel_register_gps(&s_catcall);
    return ESP_OK;
}

static bool nmea_get_fix(gps_fix_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_fix;
    xSemaphoreGive(s_mutex);
    return out->valid;
}

static esp_err_t nmea_deinit(void) {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_driver_delete(CONFIG_GPS_UART_PORT);
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
    return ESP_OK;
}

static const catcall_gps_t s_catcall = {
    .name            = "generic_nmea",
    .catcall_version = 2,
    .init            = nmea_init,
    .get_fix         = nmea_get_fix,
    .deinit          = nmea_deinit,
};

// purr_module_header_t.deinit is void(*)(void); nmea_deinit returns esp_err_t.
// GCC 13 let the mismatch through as a warning, GCC 14 (IDF 5.5 toolchain)
// makes it a hard error — same wrapper shape every other driver already uses.
static void nmea_drv_deinit(void)
{
    nmea_deinit();
}

PURR_MODULE_REGISTER(generic_nmea) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "generic_nmea",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = CATCALL_FLAG_GPS,
    .required_catcalls = 0,
    .init              = nmea_init,
    .deinit            = nmea_drv_deinit,
};
