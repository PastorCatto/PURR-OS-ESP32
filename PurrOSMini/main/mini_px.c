/* mini_px.c - see mini_px.h. */
#include "mini_px.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "purr_px_esp.h"
#include "purr_px_link.h"
#include "uecc/uECC.h"

static const char *TAG = "mini-px";

static purr_pxl_t s_link;
static purr_pxl_io_t s_io;
static SemaphoreHandle_t s_lock;
static bool s_up;

static int rng_fn(uint8_t *dest, unsigned size) { return purr_pxe_rand(dest, size) == 0 ? 1 : 0; }
static void e_random(uint8_t *out, int n) { purr_pxe_rand(out, (size_t)n); }
static int e_keypair(uint8_t pub[64], uint8_t priv[32]) { return uECC_make_key(pub, priv, uECC_secp256r1()); }
static int e_shared(const uint8_t pub[64], const uint8_t priv[32], uint8_t out[32]) { return uECC_shared_secret(pub, priv, out, uECC_secp256r1()); }
static int e_sign(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig[64]) { return uECC_sign(priv, hash, 32, sig, uECC_secp256r1()); }
static int e_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]) { return uECC_verify(pub, hash, 32, sig, uECC_secp256r1()); }
static const purr_px_ecc_t s_ecc = {.random = e_random, .keypair = e_keypair, .shared = e_shared, .sign = e_sign, .verify = e_verify};

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

/* the one blob, in NVS */
static int store_load(uint8_t *buf, size_t cap)
{
    nvs_handle_t nh;
    if (nvs_open("mini", NVS_READONLY, &nh) != ESP_OK) {
        return 0;
    }
    size_t n = 0;
    int r = 0;
    if (nvs_get_blob(nh, "pxlink", NULL, &n) == ESP_OK && n > 0 && n <= cap) {
        r = nvs_get_blob(nh, "pxlink", buf, &n) == ESP_OK ? (int)n : -1;
    }
    nvs_close(nh);
    return r;
}

static int store_save(const uint8_t *data, size_t len)
{
    nvs_handle_t nh;
    if (nvs_open("mini", NVS_READWRITE, &nh) != ESP_OK) {
        return -1;
    }
    esp_err_t e = nvs_set_blob(nh, "pxlink", data, len);
    if (e == ESP_OK) {
        e = nvs_commit(nh);
    }
    nvs_close(nh);
    return e == ESP_OK ? 0 : -1;
}

static void px_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            purr_pxl_ops.tick(&s_link, 0);
            xSemaphoreGive(s_lock);
        }
    }
}

void mini_px_start(void)
{
    if (s_up) {
        return;
    }
    uECC_set_rng(rng_fn);
    s_io = (purr_pxl_io_t){
        .ecc = &s_ecc, .now_us = now_us,
        .radio_send = purr_pxe_radio_send, .radio_recv = purr_pxe_radio_recv,
        .udp_open = purr_pxe_udp_open, .udp_send = purr_pxe_udp_send, .udp_recv = purr_pxe_udp_recv, .udp_close = purr_pxe_udp_close,
        .store_load = store_load, .store_save = store_save,
        .net_saved_get = NULL, .net_saved_add = NULL,         /* the Mini keeps one network, and a network handed over would replace it: not taken yet */
        .klass = PURR_PX_CLASS_MINI, .caps = PURR_PX_CAP_DISPLAY, .headless = 0, .name_prefix = "Mini",
    };
    s_lock = xSemaphoreCreateMutex();
    if (purr_pxl_init(&s_link, &s_io) != 0) {
        ESP_LOGE(TAG, "the link did not start");
        return;
    }
    s_up = true;
    xTaskCreate(px_task, "mini_px", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "link up as %s", s_link.name);
}

#define LOCKED(expr)                                         \
    do {                                                     \
        if (!s_up || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) { \
            return -1;                                       \
        }                                                    \
        int r_ = (expr);                                     \
        xSemaphoreGive(s_lock);                              \
        return r_;                                           \
    } while (0)

int mini_px_hosts(purr_link_peer_t *out, int max)
{
    static purr_link_peer_t all[16];
    if (!s_up || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    int n = purr_pxl_ops.nearby(&s_link, all, 16);
    xSemaphoreGive(s_lock);
    int k = 0;
    for (int i = 0; i < n && i < 16 && k < max; i++) {
        if ((all[i].caps_lo & PURR_PX_CAP_DISPLAY_HOST) && all[i].online) {
            out[k++] = all[i];
        }
    }
    return k;
}

bool mini_px_paired_host(purr_link_peer_t *out)
{
    purr_link_peer_t hosts[8];
    int n = mini_px_hosts(hosts, 8);
    for (int i = 0; i < n; i++) {
        if (hosts[i].paired) {
            *out = hosts[i];
            return true;
        }
    }
    return false;
}

bool mini_px_have_paired(void)
{
    if (!s_up || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    int r = purr_pxl_has_peers(&s_link);
    xSemaphoreGive(s_lock);
    return r != 0;
}

int mini_px_pair(const uint8_t id[8]) { LOCKED(purr_pxl_ops.pair(&s_link, id)); }
int mini_px_prompt(purr_link_prompt_t *out) { LOCKED(purr_pxl_ops.prompt(&s_link, out)); }
int mini_px_answer(int yes) { LOCKED(purr_pxl_ops.answer(&s_link, yes)); }

bool mini_px_is_paired(const uint8_t id[8])
{
    return s_up && purr_px_store_find_peer(&s_link.store, id) != NULL;
}

void mini_px_forget_all(void)
{
    if (!s_up || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        if (s_link.store.peers[i].used) {
            purr_pxl_ops.forget(&s_link, s_link.store.peers[i].id);
        }
    }
    xSemaphoreGive(s_lock);
}

int mini_px_stream_start(const uint8_t host_id[8], uint8_t hello1[152]) { LOCKED(purr_pxl_stream_start(&s_link, host_id, hello1, 152)); }
int mini_px_stream_finish(int stream, const uint8_t hello2[144]) { LOCKED(purr_pxl_stream_finish(&s_link, stream, hello2, 144)); }
int mini_px_stream_seal(int stream, const uint8_t *pt, size_t n, uint8_t *out, size_t cap) { LOCKED(purr_pxl_stream_seal(&s_link, stream, 0, pt, n, out, cap)); }

int mini_px_stream_open(int stream, const uint8_t *rec, size_t n, uint8_t *pt, size_t cap)
{
    uint8_t ch;
    LOCKED(purr_pxl_stream_open(&s_link, stream, rec, n, &ch, pt, cap));
}

void mini_px_stream_close(int stream)
{
    if (s_up && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        purr_pxl_stream_close(&s_link, stream);
        xSemaphoreGive(s_lock);
    }
}

void mini_px_forget(const uint8_t id[8])
{
    if (s_up && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        purr_pxl_ops.forget(&s_link, id);
        xSemaphoreGive(s_lock);
    }
}

bool mini_px_paired_name(char *out, size_t cap)
{
    if (!s_up || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    bool found = false;
    for (int i = 0; i < PURR_PX_MAX_PEERS && !found; i++) {
        if (s_link.store.peers[i].used) {
            strlcpy(out, s_link.store.peers[i].name, cap);
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

void mini_px_open_window(int seconds)
{
    if (s_up && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        purr_pxl_ops.option(&s_link, PURR_LINK_OPT_PAIR_WINDOW, seconds);
        xSemaphoreGive(s_lock);
    }
}
