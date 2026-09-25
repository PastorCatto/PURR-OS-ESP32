#pragma once
// catcall_radio.h — radio (LoRa / CC1101 / etc.) catcall contract

#include <stdint.h>
#include "esp_err.h"

#define CATCALL_RADIO_VERSION 3

typedef struct {
    uint32_t frequency_hz;
    uint8_t  tx_power_dbm;
    uint8_t  spreading_factor;  // LoRa: 7–12, 0 = N/A
    uint32_t bandwidth_hz;
    uint8_t  coding_rate;       // LoRa: 5–8, 0 = N/A
} radio_config_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;

    esp_err_t  (*init)(const radio_config_t *cfg);
    esp_err_t  (*send)(const uint8_t *data, size_t len);
    int        (*receive)(uint8_t *buf, size_t max_len);  // returns bytes read, -1 on error
    bool       (*data_available)(void);
    int        (*rssi)(void);
    float      (*snr)(void);
    esp_err_t  (*set_frequency)(uint32_t hz);
    esp_err_t  (*set_power)(uint8_t dbm);
    // Retune modulation params after init() — needed by anything that must
    // match a specific radio preset (e.g. Meshtastic's LONG_FAST: SF11,
    // BW250kHz, CR4/5, sync 0x2B), since radio_config_t's sf/bandwidth_hz/
    // coding_rate are otherwise only applied once, at init() time.
    esp_err_t  (*set_modulation)(uint8_t sf, uint32_t bw_hz, uint8_t cr);
    esp_err_t  (*set_sync_word)(uint8_t sync);
    esp_err_t  (*deinit)(void);
    // Optional — NULL if the driver doesn't support it, in which case the
    // caller must fall back to its own fixed-interval polling. Blocks up
    // to timeout_ms waiting for an RX-ready signal (e.g. a radio IRQ pin
    // edge), returning true if signaled, false on timeout. Lets a poll
    // loop wait on a real hardware event instead of unconditionally
    // re-checking data_available() (a full SPI transaction) on a fixed
    // short interval — see sx1262_rl.cpp's implementation and
    // meshtastic_module.c's mesh_task() for the reference use.
    bool       (*wait_rx_signal)(uint32_t timeout_ms);
    // Optional — NULL if unsupported. Wakes a task currently blocked in
    // wait_rx_signal() immediately, without waiting for its timeout —
    // used when something OTHER than an RX event needs that task to run
    // again promptly (e.g. a newly-queued outgoing message). Safe to call
    // from normal task context only, not from an ISR.
    void       (*wake_rx_wait)(void);
} catcall_radio_t;
