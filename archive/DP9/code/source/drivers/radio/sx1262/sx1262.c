// sx1262.c — SX1262 LoRa radio driver
//
// SPI command-based interface (not register-map). Supports Heltec and T-Deck
// pin layouts via CONFIG macros. Default config: 868 MHz, SF7, BW125 kHz,
// CR4/5, +14 dBm.
//
// All SPI transactions wait for BUSY to fall before issuing the next command.
// Receive runs in SetRx continuous mode; data_available() polls the IRQ line
// via GetIrqStatus.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_radio.h"
#include "purr_quirk.h"
#include "sx1262.h"

static const char *TAG = "sx1262";

// Layout for a "sx1262.pins" .purr v2 quirk block — see purr_quirk_pkg.h.
// Plain int32_t fields matching sx1262_configure()'s own parameter list —
// same convention as gt911_pin_quirk_t/adc_battery_quirk_t.
typedef struct {
    int32_t mosi;
    int32_t miso;
    int32_t sclk;
    int32_t cs;
    int32_t rst;
    int32_t busy;
    int32_t irq;
} sx1262_pin_quirk_t;

// ── Pin config ────────────────────────────────────────────────────────────────
// Defaults match Heltec's wiring. Real per-device pins (e.g. T-Deck Plus,
// which shares its SPI bus with the display/SD card) are set at runtime via
// sx1262_configure(), called by a device's specialized kernel boot before
// the module loader runs — same pattern as gt911_configure()/st7789_configure().

#define SX1262_DEFAULT_MOSI 10
#define SX1262_DEFAULT_MISO 11
#define SX1262_DEFAULT_SCLK 9
#define SX1262_DEFAULT_CS   8
#define SX1262_DEFAULT_RST  12
#define SX1262_DEFAULT_BUSY 13
#define SX1262_DEFAULT_IRQ  14

static int s_pin_mosi = SX1262_DEFAULT_MOSI;
static int s_pin_miso = SX1262_DEFAULT_MISO;
static int s_pin_sclk = SX1262_DEFAULT_SCLK;
static int s_pin_cs   = SX1262_DEFAULT_CS;
static int s_pin_rst  = SX1262_DEFAULT_RST;
static int s_pin_busy = SX1262_DEFAULT_BUSY;
static int s_pin_irq  = SX1262_DEFAULT_IRQ;

void sx1262_configure(int mosi, int miso, int sclk, int cs, int rst, int busy, int irq)
{
    // A loaded .purr v2 quirk package's "sx1262.pins" block, if present and
    // correctly sized, overrides the CALLER's own values — same precedence
    // gt911_configure()/adc_battery's own quirk checks already use.
    size_t qsz = 0;
    const void *qblk = purr_quirk_get_block("sx1262.pins", &qsz);
    if (qblk && qsz == sizeof(sx1262_pin_quirk_t)) {
        const sx1262_pin_quirk_t *q = (const sx1262_pin_quirk_t *)qblk;
        mosi = q->mosi; miso = q->miso; sclk = q->sclk;
        cs = q->cs; rst = q->rst; busy = q->busy; irq = q->irq;
        ESP_LOGI(TAG, "using loaded quirk package's pin assignment instead of caller's "
                      "(mosi=%d miso=%d sclk=%d cs=%d rst=%d busy=%d irq=%d)",
                 mosi, miso, sclk, cs, rst, busy, irq);
    } else if (qblk) {
        ESP_LOGW(TAG, "quirk block 'sx1262.pins' has wrong size (%u, expected %u) — ignoring",
                 (unsigned)qsz, (unsigned)sizeof(sx1262_pin_quirk_t));
    }

    s_pin_mosi = mosi;
    s_pin_miso = miso;
    s_pin_sclk = sclk;
    s_pin_cs   = cs;
    s_pin_rst  = rst;
    s_pin_busy = busy;
    s_pin_irq  = irq;
}

// SPI host — overridable at runtime via sx1262_set_spi_host(), default
// preserved for every device that doesn't call it.
#define SX1262_SPI_HOST  SPI2_HOST
static spi_host_device_t s_spi_host = SX1262_SPI_HOST;
#define SX1262_SPI_FREQ  (8 * 1000 * 1000)  // 8 MHz

void sx1262_set_spi_host(spi_host_device_t host)
{
    s_spi_host = host;
}

// ── SX1262 SPI commands ───────────────────────────────────────────────────────

#define CMD_SET_SLEEP           0x84
#define CMD_SET_STANDBY         0x80
#define CMD_SET_PACKET_TYPE     0x8A
#define CMD_SET_RF_FREQUENCY    0x86
#define CMD_SET_MOD_PARAMS      0x8B
#define CMD_SET_PACKET_PARAMS   0x8C
#define CMD_SET_TX_PARAMS       0x8E
#define CMD_SET_TX              0x83
#define CMD_SET_RX              0x82
#define CMD_GET_IRQ_STATUS      0x12
#define CMD_CLR_IRQ_STATUS      0x02
#define CMD_WRITE_BUFFER        0x0E
#define CMD_READ_BUFFER         0x1E
#define CMD_GET_RX_BUF_STATUS   0x13
#define CMD_GET_PACKET_STATUS   0x14
#define CMD_SET_DIO_IRQ_PARAMS  0x08
#define CMD_SET_REGULATOR_MODE  0xA0
#define CMD_SET_PA_CONFIG       0x95
#define CMD_WRITE_REGISTER      0x0D
#define CMD_SET_DIO2_AS_RF_SWITCH_CTRL 0x9D

// LoRa sync word lives at these two register addresses. Each byte packs the
// sync word nibble with a fixed 0x04 low nibble — a known SX126x quirk (not
// a plain single-byte register write), matching the convention used by
// reference implementations (e.g. RadioLib) for this chip family.
#define REG_LORA_SYNC_WORD_MSB  0x0740
#define REG_LORA_SYNC_WORD_LSB  0x0741
#define CMD_CALIBRATE_IMAGE     0x98

// SX1262 standby modes
#define STDBY_RC    0x00
#define STDBY_XOSC  0x01

// LoRa packet type
#define PACKET_TYPE_LORA 0x01

// BW codes
#define BW_125000  0x04
#define BW_250000  0x05
#define BW_500000  0x06

// IRQ bits
#define IRQ_TX_DONE  (1 << 0)
#define IRQ_RX_DONE  (1 << 1)
#define IRQ_CRC_ERR  (1 << 6)

// BUSY timeout
#define BUSY_TIMEOUT_US (3 * 1000 * 1000)  // 3 s

// ── State ─────────────────────────────────────────────────────────────────────

static spi_device_handle_t s_spi;
static bool                s_rx_continuous;

// Cached last packet stats
static int8_t  s_last_rssi_pkt;
static int8_t  s_last_snr;
static int8_t  s_last_signal_rssi;

// ── SPI helpers ───────────────────────────────────────────────────────────────

// BUSY normally falls within microseconds, so a short initial busy-spin
// keeps the common-case latency low. But esp_rom_delay_us() never yields to
// the scheduler — if BUSY ever stays high for real (a plausible transient
// after switching frequency bands/recalibrating, or any other one-off
// hardware hiccup), the old all-spin version could monopolize this task's
// CPU core for the full BUSY_TIMEOUT_US (3s) with nothing else on that core
// able to run at all, including whatever UI task happens to share it —
// live-confirmed as the cause of a MiniWin "UI TASK UNRESPONSIVE" crash-
// guard strike (6s heartbeat threshold, tripped by two such stalls back to
// back) the moment Meshtastic's mesh_task() started polling this radio
// continuously. Past the short spin window, fall back to vTaskDelay(1) —
// a real yield — for the remainder of the timeout so a stuck BUSY line
// costs this task time, not every other task's scheduling on its core.
#define WAIT_BUSY_SPIN_US 200

static void wait_busy(void)
{
    int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_US;
    int64_t spin_until = esp_timer_get_time() + WAIT_BUSY_SPIN_US;
    while (gpio_get_level(s_pin_busy) && esp_timer_get_time() < deadline) {
        if (esp_timer_get_time() < spin_until) {
            esp_rom_delay_us(10);
        } else {
            vTaskDelay(1);
        }
    }
}

// Low-level SPI: pull CS low, transfer, pull CS high.
// The SX1262 BUSY check must precede every command.
static void spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    wait_busy();

    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void write_command(uint8_t cmd, const uint8_t *params, size_t param_len)
{
    uint8_t buf[64];
    buf[0] = cmd;
    if (params && param_len > 0) {
        size_t copy = param_len < 63 ? param_len : 63;
        memcpy(buf + 1, params, copy);
    }
    spi_transfer(buf, NULL, 1 + param_len);
}

// WriteRegister: [cmd][addr_msb][addr_lsb][data...]
static void write_register(uint16_t addr, const uint8_t *data, size_t len)
{
    uint8_t buf[3 + 8];
    buf[0] = CMD_WRITE_REGISTER;
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)(addr & 0xFF);
    if (data && len > 0) memcpy(buf + 3, data, len);
    spi_transfer(buf, NULL, 3 + len);
}

// Read response: sends cmd + status_byte + param_len bytes, returns data
static void read_command(uint8_t cmd, uint8_t *out, size_t out_len)
{
    // TX: [cmd][NOP...], RX: [status][data...]
    size_t total = 1 + 1 + out_len; // cmd + status + data
    uint8_t tx[total];
    uint8_t rx[total];
    memset(tx, 0x00, total);
    tx[0] = cmd;

    spi_transfer(tx, rx, total);
    // data starts at rx[2] (rx[0]=status during cmd, rx[1]=status after cmd)
    if (out && out_len > 0) {
        memcpy(out, rx + 2, out_len);
    }
}

// ── Radio helpers ─────────────────────────────────────────────────────────────

static uint8_t bw_code(uint32_t bw_hz)
{
    if (bw_hz <= 125000) return BW_125000;
    if (bw_hz <= 250000) return BW_250000;
    return BW_500000;
}

// Compute LDRO: mandatory if symbol_time > 16 ms
// symbol_time = 2^SF / BW
static uint8_t ldro_required(uint8_t sf, uint32_t bw_hz)
{
    float sym_ms = (float)(1 << sf) / ((float)bw_hz / 1000.0f);
    return (sym_ms > 16.0f) ? 1 : 0;
}

static esp_err_t set_frequency_internal(uint32_t hz)
{
    // fRF = freq_word * 32e6 / 2^25
    uint32_t freq_word = (uint32_t)(((uint64_t)hz << 25) / 32000000UL);
    uint8_t p[4] = {
        (uint8_t)((freq_word >> 24) & 0xFF),
        (uint8_t)((freq_word >> 16) & 0xFF),
        (uint8_t)((freq_word >>  8) & 0xFF),
        (uint8_t)((freq_word >>  0) & 0xFF),
    };
    write_command(CMD_SET_RF_FREQUENCY, p, 4);
    return ESP_OK;
}

// SX126x's image-rejection calibration is band-specific (Semtech's own
// reference driver recalibrates on every frequency change, not just once
// at init) — this was previously only ever called from sx1262_init() using
// whatever frequency_hz happened to be in that call's radio_config_t
// (868 MHz default on this board, per its own default config below), and
// never again. Meshtastic's mesh_radio_apply_preset() retunes to the real
// channel frequency (906.875 MHz US default) purely via set_frequency(),
// which re-armed RX correctly but left the radio calibrated for the wrong
// band the whole time — a confirmed-live root cause for "radio inits fine,
// TX/RX report success, but never actually hears anything": degraded
// sensitivity/image rejection at the real operating frequency, with
// nothing in the command sequence ever failing to make that visible.
static void calibrate_image(uint32_t hz)
{
    uint8_t cal[2];
    if (hz < 446000000UL)      { cal[0] = 0x6B; cal[1] = 0x6F; }
    else if (hz < 734000000UL) { cal[0] = 0x75; cal[1] = 0x81; }
    else if (hz < 828000000UL) { cal[0] = 0xC1; cal[1] = 0xC5; }
    else if (hz < 877000000UL) { cal[0] = 0xD7; cal[1] = 0xDB; }
    else                        { cal[0] = 0xE1; cal[1] = 0xE9; }
    write_command(CMD_CALIBRATE_IMAGE, cal, 2);
}

static esp_err_t set_modulation(uint8_t sf, uint32_t bw_hz, uint8_t cr)
{
    // CR: catcall uses 5–8 (CR4/5=5 ... CR4/8=8)
    // SX1262 CR param: 0x01=4/5 ... 0x04=4/8
    uint8_t cr_code = (cr >= 5 && cr <= 8) ? (cr - 4) : 0x01;
    uint8_t p[4] = { sf, bw_code(bw_hz), cr_code, ldro_required(sf, bw_hz) };
    write_command(CMD_SET_MOD_PARAMS, p, 4);
    return ESP_OK;
}

static void set_packet_params(uint8_t payload_len)
{
    // preamble=8, explicit header, payload_len, CRC on, IQ standard
    uint8_t p[6] = { 0x00, 0x08, 0x00, payload_len, 0x01, 0x00 };
    write_command(CMD_SET_PACKET_PARAMS, p, 6);
}

static void standby(void)
{
    uint8_t p = STDBY_RC;
    write_command(CMD_SET_STANDBY, &p, 1);
}

static void start_rx_continuous(void)
{
    // timeout = 0xFFFFFF = continuous
    uint8_t p[3] = { 0xFF, 0xFF, 0xFF };
    write_command(CMD_SET_RX, p, 3);
    s_rx_continuous = true;
}

// ── Catcall: init ─────────────────────────────────────────────────────────────

static esp_err_t sx1262_init(const radio_config_t *cfg)
{
    // GPIO: RST, BUSY, IRQ
    gpio_config_t gp = {
        .pin_bit_mask = (1ULL << s_pin_rst),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gp);

    gp.pin_bit_mask = (1ULL << s_pin_busy) | (1ULL << s_pin_irq);
    gp.mode         = GPIO_MODE_INPUT;
    gp.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&gp);

    // Hardware reset
    gpio_set_level(s_pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // SPI bus — max_transfer_sz must cover whatever the largest consumer on a
    // shared bus needs (e.g. T-Deck Plus shares this bus with the display),
    // not just this driver's own 256-byte LoRa payload cap. Only the first
    // spi_bus_initialize() call for a given host actually takes effect
    // (later calls just return ESP_ERR_INVALID_STATE, handled below), so
    // whichever driver initializes the bus first "wins" this value — 256
    // would silently cap a shared bus other devices need larger transfers
    // on. Use a size generous enough for a full small-display frame.
    spi_bus_config_t bus = {
        .mosi_io_num     = s_pin_mosi,
        .miso_io_num     = s_pin_miso,
        .sclk_io_num     = s_pin_sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 320 * 240 * 2 + 8,
    };
    esp_err_t ret = spi_bus_initialize(s_spi_host, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SX1262_SPI_FREQ,
        .mode           = 0,
        .spics_io_num   = s_pin_cs,
        .queue_size     = 4,
        .pre_cb         = NULL,
    };
    ret = spi_bus_add_device(s_spi_host, &dev, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Use defaults if no config provided
    uint32_t freq  = cfg ? cfg->frequency_hz    : 868000000UL;
    uint8_t  sf    = cfg ? cfg->spreading_factor : 7;
    uint32_t bw    = cfg ? cfg->bandwidth_hz     : 125000;
    uint8_t  cr    = cfg ? cfg->coding_rate      : 5;
    uint8_t  power = cfg ? cfg->tx_power_dbm     : 14;

    // Stand-by mode
    standby();

    // Use DC-DC regulator (LDO mode 0x00, DCDC mode 0x01)
    { uint8_t p = 0x01; write_command(CMD_SET_REGULATOR_MODE, &p, 1); }

    // LoRa packet type
    { uint8_t p = PACKET_TYPE_LORA; write_command(CMD_SET_PACKET_TYPE, &p, 1); }

    // PA config for SX1262: hp_max=0x07, device_sel=0x00 (SX1262), pa_lut=0x01
    { uint8_t p[4] = { 0x04, 0x07, 0x00, 0x01 }; write_command(CMD_SET_PA_CONFIG, p, 4); }

    // TX params: power, ramp time (0x04 = 200 µs)
    { uint8_t p[2] = { power, 0x04 }; write_command(CMD_SET_TX_PARAMS, p, 2); }

    // Frequency
    set_frequency_internal(freq);

    // Calibrate image for the chosen band
    calibrate_image(freq);

    // Modulation
    set_modulation(sf, bw, cr);

    // Packet params (unknown payload at init — will be set per-send)
    set_packet_params(255);

    // Wire IRQ: TxDone + RxDone + CrcErr on DIO1
    { uint8_t p[8] = { 0x00, IRQ_TX_DONE | IRQ_RX_DONE | IRQ_CRC_ERR,
                        0x00, IRQ_TX_DONE | IRQ_RX_DONE | IRQ_CRC_ERR,
                        0x00, 0x00, 0x00, 0x00 };
      write_command(CMD_SET_DIO_IRQ_PARAMS, p, 8); }

    // Most integrated SX1262 modules (this board's included, per the
    // official Meshtastic firmware's own T-Deck board definition —
    // variants/esp32s3/t-deck/variant.h sets SX126X_DIO2_AS_RF_SWITCH and
    // confirms "the TTGO module hooks the SX1262-DIO2 in to control the
    // TX/RX switch") wire DIO2 to automatically drive the antenna's TX/RX
    // switch — without telling the chip to actually use DIO2 that way, the
    // switch's state is undefined, and RX can silently never see any real
    // RF no matter how correctly everything else is configured. Opcode and
    // data byte match RadioLib's SX126x::setDio2AsRfSwitch(true) exactly
    // (RADIOLIB_SX126X_CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D,
    // RADIOLIB_SX126X_DIO2_AS_RF_SWITCH = 0x01) — Meshtastic's own firmware
    // is built on RadioLib, so this is the exact command real Meshtastic
    // nodes send. First attempt placed this right after regulator mode and
    // hung sx1262_init() outright (meshtastic's own boot-time "ready" log
    // line, normally present within ~1-2s, never appeared even 30s after a
    // fresh reset) — moved here to match RadioLib's begin() ordering
    // exactly: after packet type/sync word/frequency/modulation are
    // already configured, immediately before the first RX/TX.
    { uint8_t p = 0x01; write_command(CMD_SET_DIO2_AS_RF_SWITCH_CTRL, &p, 1); }

    // Start in receive mode
    start_rx_continuous();

    ESP_LOGI(TAG, "init OK  freq=%luHz SF%u BW%lu CR4/%u pwr=%ddBm",
             (unsigned long)freq, sf, (unsigned long)(bw / 1000), cr, power);
    return ESP_OK;
}

// ── Catcall: send ─────────────────────────────────────────────────────────────

static esp_err_t sx1262_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 255) return ESP_ERR_INVALID_ARG;

    standby();

    // Write payload into TX buffer at offset 0
    {
        uint8_t cmd[2 + len];
        cmd[0] = CMD_WRITE_BUFFER;
        cmd[1] = 0x00; // offset
        memcpy(cmd + 2, data, len);
        spi_transfer(cmd, NULL, sizeof(cmd));
    }

    // Update packet params for this payload length
    set_packet_params((uint8_t)len);

    // Clear IRQ
    { uint8_t p[2] = { 0xFF, 0xFF }; write_command(CMD_CLR_IRQ_STATUS, p, 2); }

    // Start TX (timeout = 0 = no timeout)
    { uint8_t p[3] = { 0, 0, 0 }; write_command(CMD_SET_TX, p, 3); }

    // Poll for TxDone (bit 0) — timeout 5 s
    int64_t deadline = esp_timer_get_time() + 5000000LL;
    while (esp_timer_get_time() < deadline) {
        uint8_t irq[2];
        read_command(CMD_GET_IRQ_STATUS, irq, 2);
        uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
        if (flags & IRQ_TX_DONE) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Clear IRQ and return to RX
    { uint8_t p[2] = { 0xFF, 0xFF }; write_command(CMD_CLR_IRQ_STATUS, p, 2); }
    start_rx_continuous();

    ESP_LOGD(TAG, "sent %u bytes", (unsigned)len);
    return ESP_OK;
}

// ── Catcall: data_available ───────────────────────────────────────────────────

static bool sx1262_data_available(void)
{
    uint8_t irq[2];
    read_command(CMD_GET_IRQ_STATUS, irq, 2);
    uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
    return (flags & IRQ_RX_DONE) != 0;
}

// ── Catcall: receive ──────────────────────────────────────────────────────────

static int sx1262_receive(uint8_t *buf, size_t max_len)
{
    if (!buf || max_len == 0) return -1;

    uint8_t irq[2];
    read_command(CMD_GET_IRQ_STATUS, irq, 2);
    uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];

    if (!(flags & IRQ_RX_DONE)) return 0; // nothing yet

    // CRC error?
    if (flags & IRQ_CRC_ERR) {
        uint8_t p[2] = { 0xFF, 0xFF };
        write_command(CMD_CLR_IRQ_STATUS, p, 2);
        ESP_LOGW(TAG, "CRC error");
        return -1;
    }

    // GetRxBufferStatus: [payload_len][rx_start_buffer_ptr]
    uint8_t buf_stat[2];
    read_command(CMD_GET_RX_BUF_STATUS, buf_stat, 2);
    uint8_t payload_len = buf_stat[0];
    uint8_t start_ptr   = buf_stat[1];

    if (payload_len > max_len) payload_len = (uint8_t)max_len;

    // ReadBuffer: [cmd][offset][NOP][data...]
    {
        size_t total = 3 + payload_len;
        uint8_t tx[total];
        uint8_t rx[total];
        memset(tx, 0x00, total);
        tx[0] = CMD_READ_BUFFER;
        tx[1] = start_ptr;
        // tx[2] = NOP (status byte slot)

        wait_busy();
        spi_transaction_t t = {
            .length    = total * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        spi_device_polling_transmit(s_spi, &t);
        memcpy(buf, rx + 3, payload_len);
    }

    // GetPacketStatus: [rssi_pkt][snr_pkt][signal_rssi_pkt]
    {
        uint8_t pstat[3];
        read_command(CMD_GET_PACKET_STATUS, pstat, 3);
        s_last_rssi_pkt    = -(int8_t)(pstat[0] >> 1); // -RssiPkt/2
        s_last_snr         = (int8_t)pstat[1] / 4;
        s_last_signal_rssi = -(int8_t)(pstat[2] >> 1);
    }

    // Clear IRQ
    { uint8_t p[2] = { 0xFF, 0xFF }; write_command(CMD_CLR_IRQ_STATUS, p, 2); }

    ESP_LOGD(TAG, "recv %u bytes RSSI=%d SNR=%d", payload_len, s_last_rssi_pkt, s_last_snr);
    return (int)payload_len;
}

// ── Catcall: rssi / snr ───────────────────────────────────────────────────────

static int sx1262_rssi(void) { return (int)s_last_rssi_pkt; }
static float sx1262_snr(void)  { return (float)s_last_snr; }

// ── Catcall: set_frequency ────────────────────────────────────────────────────

static esp_err_t sx1262_set_frequency(uint32_t hz)
{
    standby();
    esp_err_t ret = set_frequency_internal(hz);
    calibrate_image(hz);   // band-specific — see calibrate_image()'s comment
    start_rx_continuous();
    return ret;
}

// ── Catcall: set_power ────────────────────────────────────────────────────────

static esp_err_t sx1262_set_power(uint8_t dbm)
{
    standby();
    uint8_t p[2] = { dbm, 0x04 };
    write_command(CMD_SET_TX_PARAMS, p, 2);
    start_rx_continuous();
    return ESP_OK;
}

// ── Catcall: set_modulation / set_sync_word ───────────────────────────────────
// Both bracket with standby()/start_rx_continuous() like set_frequency/
// set_power above — modulation params and the sync word can't be changed
// while the radio is actively receiving.

static esp_err_t sx1262_set_modulation(uint8_t sf, uint32_t bw_hz, uint8_t cr)
{
    standby();
    esp_err_t ret = set_modulation(sf, bw_hz, cr);
    start_rx_continuous();
    return ret;
}

static esp_err_t sx1262_set_sync_word(uint8_t sync)
{
    standby();
    uint8_t msb = (uint8_t)((sync & 0xF0) | 0x04);
    uint8_t lsb = (uint8_t)(((sync & 0x0F) << 4) | 0x04);
    write_register(REG_LORA_SYNC_WORD_MSB, &msb, 1);
    write_register(REG_LORA_SYNC_WORD_LSB, &lsb, 1);
    start_rx_continuous();
    return ESP_OK;
}

// ── Catcall: deinit ───────────────────────────────────────────────────────────

static esp_err_t sx1262_deinit(void)
{
    standby();
    { uint8_t p = 0x00; write_command(CMD_SET_SLEEP, &p, 1); }
    spi_bus_remove_device(s_spi);
    spi_bus_free(s_spi_host);
    s_spi = NULL;
    ESP_LOGI(TAG, "deinit OK");
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_radio_t s_catcall = {
    .name            = "sx1262",
    .catcall_version = CATCALL_RADIO_VERSION,
    .init            = sx1262_init,
    .send            = sx1262_send,
    .receive         = sx1262_receive,
    .data_available  = sx1262_data_available,
    .rssi            = sx1262_rssi,
    .snr             = sx1262_snr,
    .set_frequency   = sx1262_set_frequency,
    .set_power       = sx1262_set_power,
    .set_modulation  = sx1262_set_modulation,
    .set_sync_word   = sx1262_set_sync_word,
    .deinit          = sx1262_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

static int module_init(void)
{
    // Default config — callers can reconfigure via set_frequency / set_power
    static const radio_config_t default_cfg = {
        .frequency_hz    = 868000000UL,
        .tx_power_dbm    = 14,
        .spreading_factor = 7,
        .bandwidth_hz    = 125000,
        .coding_rate     = 5,
    };
    esp_err_t ret = sx1262_init(&default_cfg);
    if (ret != ESP_OK) return -1;
    purr_kernel_register_radio(&s_catcall);
    return 0;
}

static void module_deinit(void)
{
    sx1262_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(sx1262) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "sx1262",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_RADIO,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
