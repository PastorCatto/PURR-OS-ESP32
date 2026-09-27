// sx1276.c — SX1276 LoRa radio driver
//
// Register-based SPI interface (address | 0x80 for write). Supports T-Deck
// Plus pin defaults via CONFIG macros. Default config: 868 MHz, SF7, BW125,
// CR4/5, PA_BOOST.
//
// Operate in LoRa mode (LongRangeMode=1). RX runs in RXCONTINUOUS mode;
// TX goes Standby → write FIFO → TX → poll TxDone → back to RXCONTINUOUS.

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

static const char *TAG = "sx1276";

// ── Pin config ────────────────────────────────────────────────────────────────

#ifndef SX1276_PIN_MOSI
#define SX1276_PIN_MOSI 6
#endif
#ifndef SX1276_PIN_MISO
#define SX1276_PIN_MISO 5
#endif
#ifndef SX1276_PIN_SCLK
#define SX1276_PIN_SCLK 7
#endif
#ifndef SX1276_PIN_CS
#define SX1276_PIN_CS   9
#endif
#ifndef SX1276_PIN_RST
#define SX1276_PIN_RST  17
#endif
#ifndef SX1276_PIN_IRQ
#define SX1276_PIN_IRQ  45
#endif

#define SX1276_SPI_HOST  SPI3_HOST
#define SX1276_SPI_FREQ  (8 * 1000 * 1000)

// ── SX1276 register map ───────────────────────────────────────────────────────

#define REG_FIFO                0x00
#define REG_OP_MODE             0x01
#define REG_FR_MSB              0x06
#define REG_FR_MID              0x07
#define REG_FR_LSB              0x08
#define REG_PA_CONFIG           0x09
#define REG_PA_RAMP             0x0A
#define REG_OCP                 0x0B
#define REG_LNA                 0x0C
#define REG_FIFO_ADDR_PTR       0x0D
#define REG_FIFO_TX_BASE_ADDR   0x0E
#define REG_FIFO_RX_BASE_ADDR   0x0F
#define REG_FIFO_RX_CURR_ADDR   0x10
#define REG_IRQ_FLAGS_MASK      0x11
#define REG_IRQ_FLAGS           0x12
#define REG_RX_NB_BYTES         0x13
#define REG_MODEM_STAT          0x18
#define REG_PKT_SNR_VALUE       0x19
#define REG_PKT_RSSI_VALUE      0x1A
#define REG_RSSI_VALUE          0x1B
#define REG_HOP_CHANNEL         0x1C
#define REG_MODEM_CONFIG1       0x1D
#define REG_MODEM_CONFIG2       0x1E
#define REG_SYMB_TIMEOUT_LSB    0x1F
#define REG_PREAMBLE_MSB        0x20
#define REG_PREAMBLE_LSB        0x21
#define REG_PAYLOAD_LENGTH      0x22
#define REG_MAX_PAYLOAD_LENGTH  0x23
#define REG_HOP_PERIOD          0x24
#define REG_FIFO_RX_BYTE_ADDR   0x25
#define REG_MODEM_CONFIG3       0x26
#define REG_DETECTION_OPTIMIZE  0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD           0x39
#define REG_DIO_MAPPING1        0x40
#define REG_VERSION             0x42

// Mode constants (bits [2:0] of RegOpMode)
#define MODE_LONG_RANGE_MODE    0x80
#define MODE_SLEEP              0x00
#define MODE_STDBY              0x01
#define MODE_TX                 0x03
#define MODE_RX_CONTINUOUS      0x05
#define MODE_RX_SINGLE          0x06

// IRQ flag bits in REG_IRQ_FLAGS
#define IRQ_TX_DONE_MASK        0x08  // bit 3
#define IRQ_RX_DONE_MASK        0x40  // bit 6
#define IRQ_PAYLOAD_CRC_ERROR   0x20  // bit 5

// PA_BOOST bit
#define PA_BOOST_BIT            0x80

// ── State ─────────────────────────────────────────────────────────────────────

static spi_device_handle_t s_spi;
static uint8_t             s_sf;
static uint32_t            s_bw_hz;
static uint8_t             s_cr;

// ── SPI helpers ───────────────────────────────────────────────────────────────

static uint8_t reg_read(uint8_t addr)
{
    uint8_t tx[2] = { addr & 0x7F, 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_spi, &t);
    return rx[1];
}

static void reg_write(uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = { addr | 0x80, val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(s_spi, &t);
}

static void reg_write_burst(uint8_t addr, const uint8_t *data, size_t len)
{
    // CS managed by spi_device, send addr then payload in one transaction
    uint8_t buf[256];
    buf[0] = addr | 0x80;
    size_t copy = len < 255 ? len : 255;
    memcpy(buf + 1, data, copy);
    spi_transaction_t t = { .length = (1 + copy) * 8, .tx_buffer = buf };
    spi_device_polling_transmit(s_spi, &t);
}

static void reg_read_burst(uint8_t addr, uint8_t *data, size_t len)
{
    uint8_t tx[256];
    uint8_t rx[256];
    size_t copy = len < 255 ? len : 255;
    tx[0] = addr & 0x7F;
    memset(tx + 1, 0, copy);
    spi_transaction_t t = { .length = (1 + copy) * 8, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_spi, &t);
    memcpy(data, rx + 1, copy);
}

// ── Radio helpers ─────────────────────────────────────────────────────────────

static void set_mode(uint8_t mode)
{
    reg_write(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode);
    // Allow PLL lock
    esp_rom_delay_us(100);
}

static uint8_t bw_reg(uint32_t bw_hz)
{
    // RegModemConfig1 BW bits [7:4]
    if (bw_hz <= 7800)    return 0x00;
    if (bw_hz <= 10400)   return 0x10;
    if (bw_hz <= 15600)   return 0x20;
    if (bw_hz <= 20800)   return 0x30;
    if (bw_hz <= 31250)   return 0x40;
    if (bw_hz <= 41700)   return 0x50;
    if (bw_hz <= 62500)   return 0x60;
    if (bw_hz <= 125000)  return 0x70;
    if (bw_hz <= 250000)  return 0x80;
    return 0x90; // 500 kHz
}

static void apply_modulation(uint8_t sf, uint32_t bw_hz, uint8_t cr)
{
    // CR: catcall 5-8 → reg 1-4
    uint8_t cr_code = (cr >= 5 && cr <= 8) ? (cr - 4) : 1;

    // RegModemConfig1: BW[7:4] | CR[3:1] | ImplicitHeader[0]=0
    uint8_t mc1 = bw_reg(bw_hz) | (cr_code << 1) | 0x00;
    reg_write(REG_MODEM_CONFIG1, mc1);

    // RegModemConfig2: SF[7:4] | TxContinuousMode[3]=0 | RxPayloadCrcOn[2]=1 | SymbTimeout[1:0]
    uint8_t mc2 = (sf << 4) | 0x04;
    reg_write(REG_MODEM_CONFIG2, mc2);

    // RegModemConfig3: LowDataRateOptimize when sym_time > 16 ms
    float sym_ms = (float)(1 << sf) / ((float)bw_hz / 1000.0f);
    uint8_t mc3 = (sym_ms > 16.0f) ? 0x08 : 0x00;
    // Also set AgcAutoOn
    mc3 |= 0x04;
    reg_write(REG_MODEM_CONFIG3, mc3);

    // SF6 special handling
    if (sf == 6) {
        reg_write(REG_DETECTION_OPTIMIZE,  0xC5);
        reg_write(REG_DETECTION_THRESHOLD, 0x0C);
    } else {
        reg_write(REG_DETECTION_OPTIMIZE,  0xC3);
        reg_write(REG_DETECTION_THRESHOLD, 0x0A);
    }
}

static esp_err_t sx1276_set_freq_internal(uint32_t hz)
{
    // fRF = Fstep * value, Fstep = 32e6 / 2^19 = ~61.035 Hz
    uint64_t frf = ((uint64_t)hz << 19) / 32000000UL;
    reg_write(REG_FR_MSB, (uint8_t)(frf >> 16));
    reg_write(REG_FR_MID, (uint8_t)(frf >>  8));
    reg_write(REG_FR_LSB, (uint8_t)(frf >>  0));
    return ESP_OK;
}

static void set_rx_continuous(void)
{
    reg_write(REG_DIO_MAPPING1, 0x00); // DIO0 = RxDone
    reg_write(REG_FIFO_RX_BASE_ADDR, 0x00);
    reg_write(REG_FIFO_ADDR_PTR, 0x00);
    set_mode(MODE_RX_CONTINUOUS);
}

// ── Catcall: init ─────────────────────────────────────────────────────────────

static esp_err_t sx1276_init(const radio_config_t *cfg)
{
    // GPIO RST output, IRQ input
    gpio_config_t gp = {
        .pin_bit_mask = (1ULL << SX1276_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gp);

    gp.pin_bit_mask = (1ULL << SX1276_PIN_IRQ);
    gp.mode         = GPIO_MODE_INPUT;
    gp.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&gp);

    // Reset
    gpio_set_level(SX1276_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(SX1276_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // SPI
    spi_bus_config_t bus = {
        .mosi_io_num     = SX1276_PIN_MOSI,
        .miso_io_num     = SX1276_PIN_MISO,
        .sclk_io_num     = SX1276_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t ret = spi_bus_initialize(SX1276_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SX1276_SPI_FREQ,
        .mode           = 0,
        .spics_io_num   = SX1276_PIN_CS,
        .queue_size     = 4,
    };
    ret = spi_bus_add_device(SX1276_SPI_HOST, &dev, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify chip version
    uint8_t ver = reg_read(REG_VERSION);
    if (ver != 0x12) {
        ESP_LOGE(TAG, "unexpected version 0x%02X (expected 0x12)", ver);
        purr_kernel_notify("LoRa unavailable", "SX1276 not detected", "sx1276");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t freq  = cfg ? cfg->frequency_hz     : 868000000UL;
    s_sf           = cfg ? cfg->spreading_factor  : 7;
    s_bw_hz        = cfg ? cfg->bandwidth_hz      : 125000;
    s_cr           = cfg ? cfg->coding_rate       : 5;
    uint8_t power  = cfg ? cfg->tx_power_dbm      : 17;

    // Go to sleep to allow LoRa mode switch
    reg_write(REG_OP_MODE, MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Enter LoRa mode in sleep
    reg_write(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Standby
    set_mode(MODE_STDBY);

    // FIFO base addresses
    reg_write(REG_FIFO_TX_BASE_ADDR, 0x00);
    reg_write(REG_FIFO_RX_BASE_ADDR, 0x00);

    // LNA: max gain, boost on
    reg_write(REG_LNA, 0x23);

    // PA: PA_BOOST, MaxPower=7, OutputPower
    uint8_t pa_val = PA_BOOST_BIT | 0x70 | ((power <= 17) ? (power - 2) : 0x0F);
    reg_write(REG_PA_CONFIG, pa_val);

    // OCP: 240 mA
    reg_write(REG_OCP, 0x2B);

    // Frequency
    sx1276_set_freq_internal(freq);

    // Modulation
    apply_modulation(s_sf, s_bw_hz, s_cr);

    // Preamble length = 8
    reg_write(REG_PREAMBLE_MSB, 0x00);
    reg_write(REG_PREAMBLE_LSB, 0x08);

    // Sync word = 0x12 (public LoRa network)
    reg_write(REG_SYNC_WORD, 0x12);

    // Start RX continuous
    set_rx_continuous();

    ESP_LOGI(TAG, "init OK ver=0x%02X freq=%luHz SF%u BW%lu CR4/%u pwr=%u",
             ver, (unsigned long)freq, s_sf, (unsigned long)(s_bw_hz / 1000), s_cr, power);
    return ESP_OK;
}

// ── Catcall: send ─────────────────────────────────────────────────────────────

static esp_err_t sx1276_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 255) return ESP_ERR_INVALID_ARG;

    set_mode(MODE_STDBY);

    // Point FIFO to TX base
    reg_write(REG_FIFO_ADDR_PTR, 0x00);
    reg_write(REG_PAYLOAD_LENGTH, (uint8_t)len);

    // Write payload
    reg_write_burst(REG_FIFO, data, len);

    // Map DIO0 = TxDone
    reg_write(REG_DIO_MAPPING1, 0x40);

    // Clear IRQ
    reg_write(REG_IRQ_FLAGS, 0xFF);

    // TX
    set_mode(MODE_TX);

    // Poll TxDone (bit 3) — timeout 5 s
    int64_t deadline = esp_timer_get_time() + 5000000LL;
    while (esp_timer_get_time() < deadline) {
        uint8_t irq = reg_read(REG_IRQ_FLAGS);
        if (irq & IRQ_TX_DONE_MASK) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Clear IRQ, return to RX
    reg_write(REG_IRQ_FLAGS, 0xFF);
    set_rx_continuous();

    ESP_LOGD(TAG, "sent %u bytes", (unsigned)len);
    return ESP_OK;
}

// ── Catcall: data_available ───────────────────────────────────────────────────

static bool sx1276_data_available(void)
{
    return (reg_read(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK) != 0;
}

// ── Catcall: receive ──────────────────────────────────────────────────────────

static int sx1276_receive(uint8_t *buf, size_t max_len)
{
    if (!buf || max_len == 0) return -1;

    uint8_t irq = reg_read(REG_IRQ_FLAGS);
    if (!(irq & IRQ_RX_DONE_MASK)) return 0;

    // CRC error?
    if (irq & IRQ_PAYLOAD_CRC_ERROR) {
        reg_write(REG_IRQ_FLAGS, 0xFF);
        ESP_LOGW(TAG, "CRC error");
        return -1;
    }

    uint8_t nb = reg_read(REG_RX_NB_BYTES);
    uint8_t rx_addr = reg_read(REG_FIFO_RX_CURR_ADDR);

    if (nb > max_len) nb = (uint8_t)max_len;

    reg_write(REG_FIFO_ADDR_PTR, rx_addr);
    reg_read_burst(REG_FIFO, buf, nb);

    // Clear IRQ
    reg_write(REG_IRQ_FLAGS, 0xFF);

    ESP_LOGD(TAG, "recv %u bytes", nb);
    return (int)nb;
}

// ── Catcall: rssi / snr ───────────────────────────────────────────────────────

static int sx1276_rssi(void)
{
    // For 868 MHz: RSSI = -157 + RegPktRssiValue
    return -157 + (int)reg_read(REG_PKT_RSSI_VALUE);
}

static float sx1276_snr(void)
{
    int8_t snr_raw = (int8_t)reg_read(REG_PKT_SNR_VALUE);
    return (float)snr_raw / 4.0f;
}

// ── Catcall: set_frequency ────────────────────────────────────────────────────

static esp_err_t sx1276_set_frequency(uint32_t hz)
{
    set_mode(MODE_STDBY);
    sx1276_set_freq_internal(hz);
    set_rx_continuous();
    return ESP_OK;
}

// ── Catcall: set_power ────────────────────────────────────────────────────────

static esp_err_t sx1276_set_power(uint8_t dbm)
{
    set_mode(MODE_STDBY);
    uint8_t pa_val = PA_BOOST_BIT | 0x70 | ((dbm <= 17) ? (dbm - 2) : 0x0F);
    reg_write(REG_PA_CONFIG, pa_val);
    set_rx_continuous();
    return ESP_OK;
}

// ── Catcall: set_modulation / set_sync_word ───────────────────────────────────
// SX1276's sync word is a single register (REG_SYNC_WORD) — much simpler
// than SX1262's split-nibble scheme. Both bracket with set_mode(STDBY)/
// set_rx_continuous() like set_power above.

static esp_err_t sx1276_set_modulation(uint8_t sf, uint32_t bw_hz, uint8_t cr)
{
    set_mode(MODE_STDBY);
    apply_modulation(sf, bw_hz, cr);
    set_rx_continuous();
    return ESP_OK;
}

static esp_err_t sx1276_set_sync_word(uint8_t sync)
{
    set_mode(MODE_STDBY);
    reg_write(REG_SYNC_WORD, sync);
    set_rx_continuous();
    return ESP_OK;
}

// ── Catcall: deinit ───────────────────────────────────────────────────────────

static esp_err_t sx1276_deinit(void)
{
    set_mode(MODE_SLEEP);
    spi_bus_remove_device(s_spi);
    spi_bus_free(SX1276_SPI_HOST);
    s_spi = NULL;
    ESP_LOGI(TAG, "deinit OK");
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_radio_t s_catcall = {
    .name            = "sx1276",
    .catcall_version = CATCALL_RADIO_VERSION,
    .init            = sx1276_init,
    .send            = sx1276_send,
    .receive         = sx1276_receive,
    .data_available  = sx1276_data_available,
    .rssi            = sx1276_rssi,
    .snr             = sx1276_snr,
    .set_frequency   = sx1276_set_frequency,
    .set_power       = sx1276_set_power,
    .set_modulation  = sx1276_set_modulation,
    .set_sync_word   = sx1276_set_sync_word,
    .deinit          = sx1276_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

static int module_init(void)
{
    static const radio_config_t default_cfg = {
        .frequency_hz     = 868000000UL,
        .tx_power_dbm     = 17,
        .spreading_factor = 7,
        .bandwidth_hz     = 125000,
        .coding_rate      = 5,
    };
    esp_err_t ret = sx1276_init(&default_cfg);
    if (ret != ESP_OK) return -1;
    purr_kernel_register_radio(&s_catcall);
    return 0;
}

static void module_deinit(void)
{
    sx1276_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(sx1276) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "sx1276",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_RADIO,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
