#pragma once
// Implements the small interfaces MeshCore's Mesh/Dispatcher require
// (mesh::Radio, mesh::MillisecondClock, mesh::RNG, mesh::MainBoard,
// mesh::RTCClock) against PurrOS's own catcall_radio_t, instead of
// vendoring MeshCore's own RadioLibWrapper (which expects Arduino's
// SPIClass and constructs its own RadioLib instance — incompatible with
// the instance already owned by source/drivers/radio/sx1262_rl/).

#include <MeshCore.h>
#include <Dispatcher.h>

// MeshCore's reference default preset (examples/companion_radio/MyMesh.h,
// upstream commit bbb58cceb852a9190b46d5f6984e8e5140e6991e): 915MHz region
// default, BW250kHz, SF10, CR 4/5, RadioLib's private LoRa sync word
// (0x12), 20dBm TX power. Region-specific PurrOS devices shipping outside
// the US ISM band would need a different MC_LORA_FREQ_HZ — same caveat
// this codebase already carries for Meshtastic's LONG_FAST preset.
#define MC_LORA_FREQ_HZ      915000000UL
#define MC_LORA_BW_HZ        250000UL
#define MC_LORA_SF           10
#define MC_LORA_CR           5
#define MC_LORA_SYNC_WORD    0x12
#define MC_LORA_TX_POWER_DBM 20

// Locks every catcall_radio_t call — mirrors meshtastic's mesh_radio_lock()
// exactly (RadioLib has no internal locking; concurrent RX-poll + send
// from independent tasks corrupts its state machine). MeshCore needs its
// own instance since Meshtastic's is a private static with no exported
// handle, and the two mesh modules are never loaded concurrently anyway.
void mc_radio_lock(void);
void mc_radio_unlock(void);

// Retunes the already-registered catcall_radio_t to MeshCore's preset.
// Returns false if no radio is registered yet.
bool mc_radio_apply_preset(void);

class PurrRadioAdapter : public mesh::Radio {
public:
    int recvRaw(uint8_t* bytes, int sz) override;
    uint32_t getEstAirtimeFor(int len_bytes) override;
    float packetScore(float snr, int packet_len) override;
    bool startSendRaw(const uint8_t* bytes, int len) override;
    bool isSendComplete() override;
    void onSendFinished() override;
    bool isInRecvMode() const override;
    float getLastRSSI() const override;
    float getLastSNR() const override;

private:
    bool last_send_ok_ = false;
};

class PurrMillisecondClock : public mesh::MillisecondClock {
public:
    unsigned long getMillis() override;
};

class PurrRNG : public mesh::RNG {
public:
    void random(uint8_t* dest, size_t sz) override;
};

class PurrMainBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override;
    const char* getManufacturerName() const override { return "PurrOS"; }
    void reboot() override;
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

// No RTC hardware wired up for meshcore in this pass (GPS-derived time is
// a future improvement) — tracks uptime-seconds as a monotonic stand-in.
// Used for advert timestamps; not wire-format-critical (any monotonically
// non-decreasing value is valid, receivers don't validate against wall
// clock), so this is safe to run with, not just a stub.
class PurrRTCClock : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override;
    void setCurrentTime(uint32_t time) override;

private:
    uint32_t offset_ = 0;
};
