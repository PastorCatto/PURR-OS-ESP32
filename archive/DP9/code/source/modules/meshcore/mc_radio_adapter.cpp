#include "mc_radio_adapter.h"
#include "purr_kernel.h"
#include "catcall_radio.h"

#include <math.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── radio lock ──────────────────────────────────────────────────────────

static SemaphoreHandle_t s_radio_mutex = nullptr;

static void ensure_mutex() {
    if (!s_radio_mutex) {
        s_radio_mutex = xSemaphoreCreateMutex();
    }
}

void mc_radio_lock(void) {
    ensure_mutex();
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
}

void mc_radio_unlock(void) {
    if (s_radio_mutex) xSemaphoreGive(s_radio_mutex);
}

bool mc_radio_apply_preset(void) {
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio) return false;

    mc_radio_lock();
    bool ok = true;
    if (radio->set_frequency)  ok = ok && (radio->set_frequency(MC_LORA_FREQ_HZ) == ESP_OK);
    if (radio->set_modulation) ok = ok && (radio->set_modulation(MC_LORA_SF, MC_LORA_BW_HZ, MC_LORA_CR) == ESP_OK);
    if (radio->set_sync_word)  ok = ok && (radio->set_sync_word(MC_LORA_SYNC_WORD) == ESP_OK);
    if (radio->set_power)      ok = ok && (radio->set_power(MC_LORA_TX_POWER_DBM) == ESP_OK);
    mc_radio_unlock();
    return ok;
}

// ── PurrRadioAdapter ────────────────────────────────────────────────────

int PurrRadioAdapter::recvRaw(uint8_t* bytes, int sz) {
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio) return 0;

    mc_radio_lock();
    int len = 0;
    if (radio->data_available && radio->data_available()) {
        len = radio->receive(bytes, (size_t)sz);
    }
    mc_radio_unlock();
    return len > 0 ? len : 0;
}

// Standard LoRa symbol/airtime formula (Semtech AN1200.13), for the fixed
// MeshCore preset this adapter always runs at. Local scheduling heuristic
// only (used by Dispatcher's own retransmit-delay/duty-cycle logic) — not
// part of the wire format, so it doesn't need to match upstream's own
// RadioLibWrapper implementation byte-for-byte to interoperate.
uint32_t PurrRadioAdapter::getEstAirtimeFor(int len_bytes) {
    const double bw_khz = MC_LORA_BW_HZ / 1000.0;
    const int sf = MC_LORA_SF;
    const int cr = MC_LORA_CR;
    const bool low_data_rate_opt = false;  // symbol time well under 16ms at SF10/BW250
    const int preamble_len = 16;
    const int crc_on = 1;
    const int explicit_header = 1;

    double symbol_time_ms = (1 << sf) / bw_khz;
    double preamble_time_ms = (preamble_len + 4.25) * symbol_time_ms;

    double numerator = 8.0 * len_bytes - 4.0 * sf + 28 + 16 * crc_on - 20 * (1 - explicit_header);
    double denominator = 4.0 * (sf - (low_data_rate_opt ? 2 : 0));
    double payload_symb_count = 8 + fmax(ceil(numerator / denominator) * cr, 0);
    double payload_time_ms = payload_symb_count * symbol_time_ms;

    return (uint32_t)(preamble_time_ms + payload_time_ms + 0.5);
}

float PurrRadioAdapter::packetScore(float snr, int packet_len) {
    (void)packet_len;
    // Normalize SNR into a rough 0..1 quality score — clamp to LoRa's
    // typical usable range at this SF/BW (~-15dB floor .. +10dB ceiling).
    float s = (snr + 15.0f) / 25.0f;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

bool PurrRadioAdapter::startSendRaw(const uint8_t* bytes, int len) {
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->send) {
        last_send_ok_ = false;
        return false;
    }

    // catcall_radio_t::send() is synchronous (confirmed: sx1262_rl's
    // implementation calls RadioLib's blocking transmit()) — so by the
    // time this returns, the send has already completed.
    mc_radio_lock();
    last_send_ok_ = (radio->send(bytes, (size_t)len) == ESP_OK);
    mc_radio_unlock();
    return last_send_ok_;
}

bool PurrRadioAdapter::isSendComplete() {
    return true;  // startSendRaw() already blocked until done
}

void PurrRadioAdapter::onSendFinished() {
    // no-op — nothing to clean up for a synchronous send
}

bool PurrRadioAdapter::isInRecvMode() const {
    // catcall_radio_t has no explicit "current mode" query; sx1262_rl's
    // send() re-arms continuous RX immediately after transmit(), and RX is
    // otherwise always active, so this is true except mid-send (which
    // startSendRaw() already blocks through).
    return purr_kernel_radio() != nullptr;
}

float PurrRadioAdapter::getLastRSSI() const {
    const catcall_radio_t *radio = purr_kernel_radio();
    return (radio && radio->rssi) ? (float)radio->rssi() : 0.0f;
}

float PurrRadioAdapter::getLastSNR() const {
    const catcall_radio_t *radio = purr_kernel_radio();
    return (radio && radio->snr) ? radio->snr() : 0.0f;
}

// ── PurrMillisecondClock ────────────────────────────────────────────────

unsigned long PurrMillisecondClock::getMillis() {
    return (unsigned long)purr_kernel_uptime_ms();
}

// ── PurrRNG ─────────────────────────────────────────────────────────────

void PurrRNG::random(uint8_t* dest, size_t sz) {
    esp_fill_random(dest, sz);  // ESP32 hardware TRNG
}

// ── PurrMainBoard ───────────────────────────────────────────────────────

uint16_t PurrMainBoard::getBattMilliVolts() {
    int mv = purr_kernel_battery_voltage_mv();
    return mv < 0 ? 0 : (uint16_t)mv;
}

void PurrMainBoard::reboot() {
    esp_restart();
}

// ── PurrRTCClock ────────────────────────────────────────────────────────

uint32_t PurrRTCClock::getCurrentTime() {
    return offset_ + (uint32_t)(purr_kernel_uptime_ms() / 1000);
}

void PurrRTCClock::setCurrentTime(uint32_t time) {
    offset_ = time - (uint32_t)(purr_kernel_uptime_ms() / 1000);
}
