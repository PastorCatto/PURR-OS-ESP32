// rnode_proto.c — see rnode_proto.h for scope/design. Command byte table
// and wire encodings below are all confirmed from RNS's own live
// RNodeInterface.py (plan doc's "Stage 1 protocol constants" section) —
// reproduced here, not re-derived.

#include "rnode_proto.h"
#include "rnode_kiss.h"

#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "rnode_proto";

// ── RNode command bytes ──────────────────────────────────────────────────

#define CMD_DATA          0x00
#define CMD_FREQUENCY     0x01
#define CMD_BANDWIDTH     0x02
#define CMD_TXPOWER       0x03
#define CMD_SF            0x04
#define CMD_CR            0x05
#define CMD_RADIO_STATE   0x06
#define CMD_RADIO_LOCK    0x07
#define CMD_DETECT        0x08
#define CMD_LEAVE         0x0A
#define CMD_ST_ALOCK      0x0B
#define CMD_LT_ALOCK      0x0C
#define CMD_READY         0x0F
#define CMD_PLATFORM      0x48
#define CMD_MCU           0x49
#define CMD_FW_VERSION    0x50
#define CMD_RESET         0x55
#define CMD_ERROR         0x90

#define DETECT_REQ        0x73
#define DETECT_RESP       0x46

#define RADIO_STATE_OFF   0x00
#define RADIO_STATE_ON    0x01

#define PLATFORM_ESP32    0x80
// CMD_MCU's reply is stored by RNS but never validated/compared against
// anything (confirmed live from the actual handler: `self.mcu = byte`,
// nothing downstream reads it) — reusing PLATFORM_ESP32 here is just an
// honest-enough single byte, not a value RNS requires.
#define MCU_ESP32         0x80

// Firmware version RNS is told we're running: reply is two raw integer
// bytes {maj, min}, required maj > 1, or (maj >= 1 and min >= 52) —
// confirmed from validate_firmware() itself. {1, 75} mirrors RNode_
// Firmware_CE's own real version (1.75) with real margin over the 1.52
// floor.
#define FW_VERSION_MAJ    1
#define FW_VERSION_MIN    75

// ── RNode-compatible split-packet framing (radio side) ──────────────────
// Same wire format rns_radio_adapter.cpp already uses for the vendored
// Reticulum stack — see that file's own header comment for the original
// citation (borrowed from microReticulum's reference LoRaInterface, which
// itself matches real RNode firmware): one header byte (random high
// nibble for traffic-analysis padding, a split flag, a 3-bit sequence
// number) in front of up to LORA_MAX_PAYLOAD bytes, matching catcall_
// radio_t's own 255-byte send()/receive() cap. Ported to plain uint8_t
// buffers here rather than shared as one file — this module has no
// dependency on the vendored C++ Reticulum stack, it's a sibling consumer
// of catcall_radio_t, not a client of reticulum_module.cpp.
#define HEADER_SPLIT       0x08
#define HEADER_SEQ_MASK    0x07
#define SEQ_UNSET          0xFF
#define LORA_MAX_PAYLOAD   254
#define RNODE_HW_MTU       (2 * LORA_MAX_PAYLOAD)   // 508 — matches rns_radio_adapter.cpp's own _HW_MTU

// ── State ─────────────────────────────────────────────────────────────

static rnode_proto_output_fn s_output = NULL;
static SemaphoreHandle_t     s_radio_mutex = NULL;

// Cached radio parameters — catcall_radio_t's set_modulation() bundles
// SF/bandwidth/coding-rate into one call, but RNode's protocol sends them
// as three separate commands (see the plan doc's own "radio parameter
// mapping" section), so each set-command updates its one field here and
// re-issues set_modulation() with all three current values, never a
// stale partial one.
typedef struct {
    uint32_t frequency_hz;
    uint32_t bandwidth_hz;
    uint8_t  spreading_factor;
    uint8_t  coding_rate;
    uint8_t  power_dbm;
    uint8_t  radio_state;   // RADIO_STATE_OFF/ON — gates CMD_DATA TX/RX both directions
} radio_params_t;

// LoRa defaults (868MHz EU ISM band, SF7/BW125/CR4:5) — same starting
// point sx1262_rl.cpp's own module_init() already brings the radio up
// with; only matters here as this cache's own initial values until the
// host actually sends CMD_FREQUENCY/etc, which is always the very first
// thing RNodeInterface.py's initRadio() does anyway.
static radio_params_t s_params = {
    .frequency_hz      = 868000000,
    .bandwidth_hz      = 125000,
    .spreading_factor  = 7,
    .coding_rate       = 5,
    .power_dbm         = 14,
    .radio_state       = RADIO_STATE_OFF,
};

static uint8_t s_tx_seq_ctr = 0;

typedef struct {
    uint8_t buf[RNODE_HW_MTU];
    size_t  len;
    uint8_t seq;   // SEQ_UNSET when no partial reassembly is in progress
} rx_reassembly_t;

static rx_reassembly_t s_rx = { .seq = SEQ_UNSET };

// ── Output helper ─────────────────────────────────────────────────────

static void send_reply(uint8_t cmd, const uint8_t *data, size_t len) {
    if (!s_output) return;
    // Escaping can at most double the (cmd + data) portion — see rnode_
    // kiss.h's own RNODE_KISS_MAX_FRAME margin. RNODE_HW_MTU (508) is the
    // largest data payload this file ever sends in one frame (CMD_DATA),
    // so 2*(1+508)+2 is a safe upper bound; RNODE_KISS_MAX_FRAME (600) is
    // sized for the encoded (not 2x) case since the decoder only ever
    // holds one *decoded* frame — the encode buffer here is separate and
    // sized for the worst case explicitly.
    uint8_t encoded[2 * (1 + RNODE_HW_MTU) + 2];
    size_t n = rnode_kiss_encode(cmd, data, len, encoded, sizeof(encoded));
    if (n == 0) { ESP_LOGW(TAG, "encode failed for cmd=0x%02X len=%u", cmd, (unsigned)len); return; }
    s_output(encoded, n);
}

// ── Radio TX (host CMD_DATA -> physical LoRa frame(s)) ───────────────────

static void radio_send_split(const uint8_t *data, size_t len) {
    if (len > RNODE_HW_MTU) {
        ESP_LOGW(TAG, "CMD_DATA payload too large (%u > %u), dropped", (unsigned)len, (unsigned)RNODE_HW_MTU);
        return;
    }
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio) return;

    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);

    uint8_t rand_nibble = (uint8_t)(esp_random() & 0xF0);
    uint8_t tx_raw[255];

    if (len <= LORA_MAX_PAYLOAD) {
        tx_raw[0] = rand_nibble;
        memcpy(tx_raw + 1, data, len);
        esp_err_t err = radio->send(tx_raw, 1 + len);
        ESP_LOGD(TAG, "CMD_DATA tx: %u bytes -> radio->send() = %d", (unsigned)len, err);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send failed");
        }
    } else {
        uint8_t seq       = (s_tx_seq_ctr++) & HEADER_SEQ_MASK;
        uint8_t split_hdr = (uint8_t)(rand_nibble | HEADER_SPLIT | seq);

        tx_raw[0] = split_hdr;
        memcpy(tx_raw + 1, data, LORA_MAX_PAYLOAD);
        if (radio->send(tx_raw, 1 + LORA_MAX_PAYLOAD) != ESP_OK) {
            ESP_LOGW(TAG, "send (part 1) failed");
        }

        size_t remainder = len - LORA_MAX_PAYLOAD;
        tx_raw[0] = split_hdr;
        memcpy(tx_raw + 1, data + LORA_MAX_PAYLOAD, remainder);
        if (radio->send(tx_raw, 1 + remainder) != ESP_OK) {
            ESP_LOGW(TAG, "send (part 2) failed");
        }
    }

    xSemaphoreGive(s_radio_mutex);
}

// ── Command dispatch ─────────────────────────────────────────────────────

static void handle_frequency(const uint8_t *data, size_t len) {
    if (len != 4) return;
    uint32_t hz = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                | ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->set_frequency) return;   // no reply — see header comment on "fail honestly"
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    esp_err_t err = radio->set_frequency(hz);
    xSemaphoreGive(s_radio_mutex);
    if (err != ESP_OK) return;
    s_params.frequency_hz = hz;
    send_reply(CMD_FREQUENCY, data, 4);
}

static esp_err_t apply_modulation_locked(void) {
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->set_modulation) return ESP_ERR_INVALID_STATE;
    return radio->set_modulation(s_params.spreading_factor, s_params.bandwidth_hz, s_params.coding_rate);
}

static void handle_bandwidth(const uint8_t *data, size_t len) {
    if (len != 4) return;
    uint32_t bw = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                | ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    uint32_t prev = s_params.bandwidth_hz;
    s_params.bandwidth_hz = bw;
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    esp_err_t err = apply_modulation_locked();
    xSemaphoreGive(s_radio_mutex);
    if (err != ESP_OK) { s_params.bandwidth_hz = prev; return; }
    send_reply(CMD_BANDWIDTH, data, 4);
}

static void handle_sf(const uint8_t *data, size_t len) {
    if (len != 1) return;
    uint8_t prev = s_params.spreading_factor;
    s_params.spreading_factor = data[0];
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    esp_err_t err = apply_modulation_locked();
    xSemaphoreGive(s_radio_mutex);
    ESP_LOGD(TAG, "handle_sf: sf=%u -> err=%d", data[0], err);
    if (err != ESP_OK) { s_params.spreading_factor = prev; return; }
    send_reply(CMD_SF, data, 1);
}

static void handle_cr(const uint8_t *data, size_t len) {
    if (len != 1) return;
    uint8_t prev = s_params.coding_rate;
    s_params.coding_rate = data[0];
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    esp_err_t err = apply_modulation_locked();
    xSemaphoreGive(s_radio_mutex);
    ESP_LOGD(TAG, "handle_cr: cr=%u -> err=%d", data[0], err);
    if (err != ESP_OK) { s_params.coding_rate = prev; return; }
    send_reply(CMD_CR, data, 1);
}

static void handle_txpower(const uint8_t *data, size_t len) {
    if (len != 1) return;
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->set_power) return;
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    esp_err_t err = radio->set_power(data[0]);
    xSemaphoreGive(s_radio_mutex);
    ESP_LOGD(TAG, "handle_txpower: dbm=%u -> err=%d", data[0], err);
    if (err != ESP_OK) return;
    s_params.power_dbm = data[0];
    send_reply(CMD_TXPOWER, data, 1);
}

static void handle_radio_state(const uint8_t *data, size_t len) {
    if (len != 1) return;
    ESP_LOGD(TAG, "handle_radio_state: state=%u radio=%p", data[0], (void*)purr_kernel_radio());
    if (data[0] == RADIO_STATE_ON) {
        // Already brought up by its own driver's module_init() (a
        // PURR_MOD_DRIVER, loaded before this PURR_MOD_SYSTEM module gets
        // a chance to run) — same "already online, just confirm it's
        // there" pattern rns_radio_adapter.cpp's start() already
        // established. Nothing to initialize, just gate CMD_DATA on it.
        if (!purr_kernel_radio()) return;   // no reply — real RNode looks like this when its radio isn't there either
        s_params.radio_state = RADIO_STATE_ON;
    } else {
        s_params.radio_state = RADIO_STATE_OFF;
    }
    send_reply(CMD_RADIO_STATE, data, 1);
}

static void handle_detect(const uint8_t *data, size_t len) {
    if (len < 1 || data[0] != DETECT_REQ) return;
    uint8_t resp = DETECT_RESP;
    send_reply(CMD_DETECT, &resp, 1);
}

static void handle_fw_version(void) {
    uint8_t reply[2] = { FW_VERSION_MAJ, FW_VERSION_MIN };
    send_reply(CMD_FW_VERSION, reply, sizeof(reply));
}

static void handle_platform(void) {
    uint8_t reply = PLATFORM_ESP32;
    send_reply(CMD_PLATFORM, &reply, 1);
}

static void handle_mcu(void) {
    uint8_t reply = MCU_ESP32;
    send_reply(CMD_MCU, &reply, 1);
}

static void handle_data(const uint8_t *data, size_t len) {
    if (s_params.radio_state != RADIO_STATE_ON) return;
    radio_send_split(data, len);
}

void rnode_proto_handle_frame(const uint8_t *frame, size_t len) {
    if (len < 1) return;
    uint8_t cmd = frame[0];
    const uint8_t *data = frame + 1;
    size_t data_len = len - 1;

    switch (cmd) {
        case CMD_DATA:        handle_data(data, data_len);       break;
        case CMD_FREQUENCY:   handle_frequency(data, data_len);  break;
        case CMD_BANDWIDTH:   handle_bandwidth(data, data_len);  break;
        case CMD_TXPOWER:     handle_txpower(data, data_len);    break;
        case CMD_SF:          handle_sf(data, data_len);         break;
        case CMD_CR:          handle_cr(data, data_len);         break;
        case CMD_RADIO_STATE: handle_radio_state(data, data_len); break;
        case CMD_DETECT:      handle_detect(data, data_len);     break;
        case CMD_FW_VERSION:  handle_fw_version();                break;
        case CMD_PLATFORM:    handle_platform();                  break;
        case CMD_MCU:         handle_mcu();                       break;

        // Optional/airtime-lock commands RNS's own initRadio() only sends
        // "if configured" — not exercised by this plan's own test flow.
        // Accepted silently (no reply, no error) rather than falling into
        // the default/unknown case below, since a host that DOES send
        // these isn't doing anything wrong.
        case CMD_ST_ALOCK:
        case CMD_LT_ALOCK:
        case CMD_LEAVE:
        case CMD_READY:
        case CMD_RADIO_LOCK:
            break;

        default:
            ESP_LOGD(TAG, "unhandled cmd=0x%02X len=%u", cmd, (unsigned)data_len);
            break;
    }
}

void rnode_proto_poll_radio(void) {
    if (s_params.radio_state != RADIO_STATE_ON) return;
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio) return;

    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    bool have_data = radio->data_available && radio->data_available();
    uint8_t raw[255];
    int n = -1;
    if (have_data) n = radio->receive(raw, sizeof(raw));
    xSemaphoreGive(s_radio_mutex);

    if (n <= 1) return;   // nothing new, a read error, or just the bare header byte

    ESP_LOGD(TAG, "radio rx: %d bytes (hdr=0x%02X)", n, raw[0]);
    uint8_t hdr = raw[0];
    uint8_t seq = hdr & HEADER_SEQ_MASK;
    bool    split = (hdr & HEADER_SPLIT) != 0;

    if (split) {
        if (s_rx.seq == SEQ_UNSET || s_rx.seq != seq) {
            // First half of a split (or a restart after a lost first half).
            s_rx.seq = seq;
            s_rx.len = 0;
            size_t n1 = (size_t)(n - 1);
            if (n1 > sizeof(s_rx.buf)) n1 = sizeof(s_rx.buf);
            memcpy(s_rx.buf, raw + 1, n1);
            s_rx.len = n1;
        } else {
            // Second half — sequence matches what we're waiting for.
            size_t n1 = (size_t)(n - 1);
            if (s_rx.len + n1 > sizeof(s_rx.buf)) n1 = sizeof(s_rx.buf) - s_rx.len;
            memcpy(s_rx.buf + s_rx.len, raw + 1, n1);
            s_rx.len += n1;
            s_rx.seq = SEQ_UNSET;
            send_reply(CMD_DATA, s_rx.buf, s_rx.len);
        }
    } else {
        // Non-split — discard any stale partial reassembly, deliver now.
        s_rx.seq = SEQ_UNSET;
        size_t n1 = (size_t)(n - 1);
        if (n1 > sizeof(s_rx.buf)) n1 = sizeof(s_rx.buf);
        memcpy(s_rx.buf, raw + 1, n1);
        s_rx.len = n1;
        send_reply(CMD_DATA, s_rx.buf, s_rx.len);
    }
}

void rnode_proto_init(rnode_proto_output_fn output) {
    s_output = output;
    if (!s_radio_mutex) s_radio_mutex = xSemaphoreCreateMutex();
    s_tx_seq_ctr = 0;
    s_rx.seq = SEQ_UNSET;
    s_rx.len = 0;
    s_params.radio_state = RADIO_STATE_OFF;
}

void rnode_proto_deinit(void) {
    s_output = NULL;
    s_params.radio_state = RADIO_STATE_OFF;
}
