/* purr_catrt_radio.c - see purr_catrt_radio.h. A thin lease in front of the LoRa driver class (purr_lora_ops_t): the OS does not know any network, the app brings its own modem settings. */
#include "purr_catrt_radio.h"

#include <string.h>

#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "purr_app_abi.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_catrt_power.h"
#include "purr_driver.h"
#include "purr_driver_class.h"
#include "purr_perm.h"

static SemaphoreHandle_t s_mu;
static char s_holder[32];
static int s_yielding;                  /* the holder was asked to hand the radio back */
static int64_t s_yield_deadline_us;
static char s_lost[32];                 /* the app whose lease was taken back: told on its next call */

static const purr_lora_ops_t *radio_ops(void **h)
{
    return purr_class_ops(PURR_DRV_CLASS_LORA, h);
}

static void lock(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

/* The holder is gone when its app is no longer running (it stopped, or crashed): the lease goes with it. */
static void reap(void)
{
    if (s_holder[0] != '\0' && purr_catrt_slot_of(s_holder) < 0) {
        s_holder[0] = '\0';
        s_yielding = 0;
    }
    if (s_yielding && esp_timer_get_time() > s_yield_deadline_us) {
        strlcpy(s_lost, s_holder, sizeof(s_lost));
        s_holder[0] = '\0';
        s_yielding = 0;
    }
}

static int gate(char *app, size_t cap)
{
    if (purr_catrt_caller(app, cap) < 0) {
        return PURR_RADIO_BAD;
    }
    int g = purr_catrt_perm_gate(PURR_PERM_RADIO);
    if (g != 0) {
        return g == -3 ? PURR_RADIO_ASK : PURR_RADIO_DENIED;
    }
    return 0;
}

static int cc_count(void)
{
    void *h;
    return radio_ops(&h) != NULL ? 1 : 0;
}

static int cc_lease(const purr_cc_radio_cfg_t *cfg)
{
    char app[32];
    int g = gate(app, sizeof(app));
    if (g != 0) {
        return g;
    }
    if (cfg == NULL) {
        return PURR_RADIO_BAD;
    }
    void *h;
    const purr_lora_ops_t *ops = radio_ops(&h);
    if (ops == NULL) {
        return PURR_RADIO_NORADIO;
    }
    lock();
    reap();
    if (s_holder[0] != '\0' && strcmp(s_holder, app) != 0) {
        unlock();
        return PURR_RADIO_BUSY;
    }
    strlcpy(s_holder, app, sizeof(s_holder));
    s_yielding = 0;
    if (strcmp(s_lost, app) == 0) {
        s_lost[0] = '\0';
    }
    purr_lora_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.freq_hz = cfg->freq_hz;
    c.bw_hz = cfg->bw_hz;
    c.sf = cfg->sf;
    c.cr = cfg->cr;
    c.tx_dbm = cfg->tx_dbm;
    c.crc_on = cfg->crc_on;
    c.preamble = cfg->preamble;
    c.sync_word = cfg->sync_word;
    int rc = ops->configure(h, &c);
    if (rc < 0) {
        s_holder[0] = '\0';
    }
    unlock();
    return rc < 0 ? PURR_RADIO_BAD : PURR_RADIO_OK;
}

static void cc_release(void)
{
    char app[32];
    if (purr_catrt_caller(app, sizeof(app)) < 0) {
        return;
    }
    void *h;
    const purr_lora_ops_t *ops = radio_ops(&h);
    lock();
    if (strcmp(s_holder, app) == 0) {
        s_holder[0] = '\0';
        s_yielding = 0;
        if (ops != NULL && ops->sleep != NULL) {
            ops->sleep(h);
        }
    }
    unlock();
}

/* 0 when the caller holds the radio, else the answer to give. */
static int holding(const char *app)
{
    lock();
    reap();
    int r = 0;
    if (strcmp(s_holder, app) != 0) {
        r = strcmp(s_lost, app) == 0 ? PURR_RADIO_LOST : (s_holder[0] != '\0' ? PURR_RADIO_BUSY : PURR_RADIO_LOST);
    }
    unlock();
    return r;
}

static int cc_send(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    char app[32];
    int g = gate(app, sizeof(app));
    if (g != 0) {
        return g;
    }
    int hr = holding(app);
    if (hr != 0) {
        return hr;
    }
    void *h;
    const purr_lora_ops_t *ops = radio_ops(&h);
    if (ops == NULL || data == NULL || len == 0 || len > 255) {
        return ops == NULL ? PURR_RADIO_NORADIO : PURR_RADIO_BAD;
    }
    return ops->send(h, data, len, timeout_ms) < 0 ? PURR_RADIO_TIMEOUT : PURR_RADIO_OK;
}

static int cc_receive(uint8_t *buf, size_t cap, purr_cc_radio_rx_t *info, uint32_t timeout_ms)
{
    char app[32];
    int g = gate(app, sizeof(app));
    if (g != 0) {
        return g;
    }
    int hr = holding(app);
    if (hr != 0) {
        return hr;
    }
    void *h;
    const purr_lora_ops_t *ops = radio_ops(&h);
    if (ops == NULL || buf == NULL) {
        return ops == NULL ? PURR_RADIO_NORADIO : PURR_RADIO_BAD;
    }
    purr_lora_rxinfo_t ri;
    memset(&ri, 0, sizeof(ri));
    int n = ops->receive(h, buf, cap, timeout_ms, &ri);
    if (n > 0 && info != NULL) {
        info->rssi_dbm = ri.rssi_dbm;
        info->snr_db = ri.snr_db;
    }
    return n;
}

static int cc_holder(char *app, size_t cap)
{
    lock();
    reap();
    int r = s_holder[0] != '\0';
    if (r && app != NULL && cap > 0) {
        strlcpy(app, s_holder, cap);
    }
    unlock();
    return r;
}

static uint32_t cc_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
}

static uint32_t cc_random32(void) { return esp_random(); }

static const purr_cc_radio_t s_cc = {
    .version = 1, .count = cc_count, .lease = cc_lease, .release = cc_release, .send = cc_send, .receive = cc_receive, .holder = cc_holder,
    .device_id = cc_device_id, .random32 = cc_random32,
};

int purr_radio_dev_holder(char *app, unsigned cap) { return cc_holder(app, cap); }

int purr_radio_dev_take_back(unsigned ms)
{
    lock();
    reap();
    if (s_holder[0] == '\0') {
        unlock();
        return -1;
    }
    s_yielding = 1;
    s_yield_deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    int slot = purr_catrt_slot_of(s_holder);
    unlock();
    if (slot >= 0) {
        purr_catrt_post_event(slot, PURR_APPEV_USER, 0, "radio.yield");
    }
    return 0;
}

void purr_catrt_radio_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    purr_catrt_set_radio(&s_cc);
}
