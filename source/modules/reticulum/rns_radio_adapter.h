#pragma once
// rns_radio_adapter.h — RNS::InterfaceImpl backed by the existing
// catcall_radio_t (purr_kernel_radio()), NOT a second independent RadioLib
// instance. See this file's own .cpp top comment for why.

#include "Interface.h"

class RnsRadioAdapter : public RNS::InterfaceImpl {
public:
    explicit RnsRadioAdapter(const char *name = "RnsRadioAdapter");
    ~RnsRadioAdapter() override;

    bool start() override;
    void stop() override;
    void loop() override;

    // RNode-compatible split-packet framing — same wire format
    // microReticulum's own reference LoRaInterface uses (examples/common/
    // lora_interface/), which itself matches real RNode firmware: one
    // header byte (a random high nibble for traffic-analysis padding, a
    // split flag, a 3-bit sequence number) in front of up to 254 payload
    // bytes, matching catcall_radio_t's own 255-byte send()/receive() cap
    // exactly. Following this wire format (not inventing a different one)
    // is what makes this interoperable with real RNode-based Reticulum
    // hardware later, not just other PURR OS devices. Public so the free
    // helper functions in the .cpp (is_split_packet()/packet_sequence())
    // can use them without needing to be members themselves.
    static constexpr uint8_t HEADER_SPLIT    = 0x08;
    static constexpr uint8_t HEADER_SEQ_MASK = 0x07;
    static constexpr uint8_t SEQ_UNSET       = 0xFF;
    static constexpr int     LORA_MAX_PAYLOAD = 254;

private:
    bool send_outgoing(const RNS::Bytes &data) override;

    RNS::Bytes rx_buffer_;
    uint8_t    rx_seq_     = SEQ_UNSET;
    uint8_t    tx_seq_ctr_ = 0;
};
