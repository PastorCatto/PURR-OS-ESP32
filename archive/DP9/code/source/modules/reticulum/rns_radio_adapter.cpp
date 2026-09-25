// rns_radio_adapter.cpp — Reticulum's radio interface, talking to the
// SAME already-initialized RadioLib SX1262 instance sx1262_rl.cpp owns,
// via the catcall_radio_t abstraction (purr_kernel_radio()) — NOT a
// second independent RadioLib/EspHal/Module/SX1262 construction. Two
// RadioLib instances fighting over one physical radio was never viable;
// this is the same "one physical radio, one catcall_radio_t slot" rule
// meshtastic_module.c/meshcore_module.cpp already enforce via purr_
// kernel_mesh_backend_get(), applied here as this module's own consumer
// of that same shared driver instead of a rival one.
//
// microReticulum's own reference LoRaInterface (examples/common/
// lora_interface/) was not vendored — it's #ifdef ARDUINO-gated and
// constructs its own RadioLib instance via Arduino's SPI class. This file
// borrows its wire format (see rns_radio_adapter.h's own comment) but
// reimplements the actual radio calls against catcall_radio_t.

#include "rns_radio_adapter.h"

extern "C" {
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_radio.h"
#include "esp_random.h"
#include "esp_log.h"
}

static const char *TAG = "reticulum";

static inline bool    is_split_packet(uint8_t h) { return (h & RnsRadioAdapter::HEADER_SPLIT) != 0; }
static inline uint8_t packet_sequence(uint8_t h) { return h & RnsRadioAdapter::HEADER_SEQ_MASK; }

RnsRadioAdapter::RnsRadioAdapter(const char *name) : RNS::InterfaceImpl(name)
{
    _IN  = true;
    _OUT = true;
    // 508 = 2 * LORA_MAX_PAYLOAD (254) — the largest payload a 2-frame
    // split send can carry, matching the reference interface's own MTU.
    // Bitrate is left at 0 ("unknown" to Reticulum's own bandwidth-aware
    // logic) rather than computed from SF/BW/CR: unlike the reference,
    // this adapter doesn't own those parameters — sx1262_rl's own module_
    // init() already configured them before this interface ever starts.
    _HW_MTU = 508;
}

RnsRadioAdapter::~RnsRadioAdapter()
{
    stop();
}

bool RnsRadioAdapter::start()
{
    _online = false;
    // Already brought up and left in continuous RX by sx1262_rl's own
    // module_init() (a PURR_MOD_DRIVER, loaded before any PURR_MOD_SYSTEM
    // module like this one gets a chance to run) — nothing to initialize
    // here, just confirm it's actually there.
    if (!purr_kernel_radio()) {
        ESP_LOGE(TAG, "radio adapter: no catcall_radio_t registered");
        return false;
    }
    _online = true;
    ESP_LOGI(TAG, "radio adapter: online");
    return true;
}

void RnsRadioAdapter::stop()
{
    _online = false;
}

void RnsRadioAdapter::loop()
{
    if (!_online) return;
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->data_available()) return;

    uint8_t rx_raw[255];
    int len = radio->receive(rx_raw, sizeof(rx_raw));
    if (len <= 1) return;   // nothing new, a read error, or just the bare header byte

    uint8_t hdr = rx_raw[0];
    uint8_t seq = packet_sequence(hdr);

    if (is_split_packet(hdr)) {
        if (rx_seq_ == SEQ_UNSET || rx_seq_ != seq) {
            // First half of a split (or a restart after a lost first half).
            rx_seq_ = seq;
            rx_buffer_.clear();
            rx_buffer_.append(rx_raw + 1, len - 1);
        } else {
            // Second half — sequence matches what we're waiting for.
            rx_buffer_.append(rx_raw + 1, len - 1);
            rx_seq_ = SEQ_UNSET;
            handle_incoming(rx_buffer_);
        }
    } else {
        // Non-split — discard any stale partial reassembly, deliver now.
        if (rx_seq_ != SEQ_UNSET) {
            rx_buffer_.clear();
            rx_seq_ = SEQ_UNSET;
        }
        rx_buffer_.clear();
        rx_buffer_.append(rx_raw + 1, len - 1);
        handle_incoming(rx_buffer_);
    }
}

bool RnsRadioAdapter::send_outgoing(const RNS::Bytes &data)
{
    bool success = true;
    if (_online) {
        const catcall_radio_t *radio = purr_kernel_radio();
        if (!radio) {
            success = false;
        } else {
            uint8_t tx_raw[255];
            // High nibble of the header is random padding against traffic
            // analysis (same reasoning RNode firmware / the reference
            // interface use it for) — not part of the split/sequence
            // encoding, which lives entirely in the low nibble.
            uint8_t rand_nibble = (uint8_t)(esp_random() & 0xF0);

            if ((int)data.size() <= LORA_MAX_PAYLOAD) {
                tx_raw[0] = rand_nibble;
                memcpy(tx_raw + 1, data.data(), data.size());
                if (radio->send(tx_raw, 1 + data.size()) != ESP_OK) {
                    ESP_LOGW(TAG, "radio adapter: send failed");
                    success = false;
                }
            } else {
                // 2-frame split send — HW_MTU (508) guarantees Reticulum
                // itself never hands this more than 2*LORA_MAX_PAYLOAD.
                uint8_t seq       = (tx_seq_ctr_++) & HEADER_SEQ_MASK;
                uint8_t split_hdr = (uint8_t)(rand_nibble | HEADER_SPLIT | seq);

                tx_raw[0] = split_hdr;
                memcpy(tx_raw + 1, data.data(), LORA_MAX_PAYLOAD);
                if (radio->send(tx_raw, 1 + LORA_MAX_PAYLOAD) != ESP_OK) {
                    ESP_LOGW(TAG, "radio adapter: send (part 1) failed");
                    success = false;
                }

                size_t remainder = data.size() - LORA_MAX_PAYLOAD;
                tx_raw[0] = split_hdr;
                memcpy(tx_raw + 1, data.data() + LORA_MAX_PAYLOAD, remainder);
                if (radio->send(tx_raw, 1 + remainder) != ESP_OK) {
                    ESP_LOGW(TAG, "radio adapter: send (part 2) failed");
                    success = false;
                }
            }
        }
    }
    // Post-send housekeeping (traffic counters etc.) — same call the
    // reference interface makes after its own transmit, regardless of
    // outcome.
    InterfaceImpl::handle_outgoing(data);
    return success;
}
