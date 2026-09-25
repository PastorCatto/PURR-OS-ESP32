// sx1262_rl.cpp — SX1262 LoRa radio driver backed by vendored RadioLib.
//
// Same catcall_radio_t contract as drivers/radio/sx1262 (the hand-rolled
// SPI-command driver), but chip bring-up/RX/TX go through RadioLib's own
// SX126x::begin()/setDio2AsRfSwitch()/startReceive()/readData()/transmit()
// — the exact same proven code path Meshtastic's real firmware uses —
// instead of re-deriving SX126x's bring-up quirks by hand one bug at a
// time (image calibration band, sync word register encoding, DIO2 RF
// switch ordering all bit the hand-rolled driver in turn; a fourth,
// TCXO enable on DIO3 at 1.8V — confirmed from Meshtastic's own T-Deck
// variant.h, SX126X_DIO3_TCXO_VOLTAGE 1.8 — was never even attempted
// there, and RadioLib's begin() handles it as one of its arguments).
//
// Polling-style, not IRQ-callback-style: meshtastic_module.c's mesh_task()
// already polls catcall_radio_t.data_available()/receive() every 10ms, so
// this driver just answers those polls via RadioLib's getIrqFlags()/
// readData() instead of running its own SPI command sequence.

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_radio.h"
#include "purr_quirk.h"
#include "sx1262_rl.h"
}

// RadioLib.h itself #includes hal/ESP-IDF/EspHal.h (guarded to no-op under
// Arduino), so EspHal is already declared — no separate include needed.
#include "RadioLib.h"

static const char *TAG = "sx1262_rl";

// ── Pin config ────────────────────────────────────────────────────────────
// Same runtime-configure pattern as sx1262.c — a device's kernel boot calls
// sx1262_rl_configure() before purr_kernel_load_static_modules() runs this
// driver's module_init().

#define SX1262_RL_DEFAULT_MOSI 10
#define SX1262_RL_DEFAULT_MISO 11
#define SX1262_RL_DEFAULT_SCLK 9
#define SX1262_RL_DEFAULT_CS   8
#define SX1262_RL_DEFAULT_RST  12
#define SX1262_RL_DEFAULT_BUSY 13
#define SX1262_RL_DEFAULT_IRQ  14

static int s_pin_mosi = SX1262_RL_DEFAULT_MOSI;
static int s_pin_miso = SX1262_RL_DEFAULT_MISO;
static int s_pin_sclk = SX1262_RL_DEFAULT_SCLK;
static int s_pin_cs   = SX1262_RL_DEFAULT_CS;
static int s_pin_rst  = SX1262_RL_DEFAULT_RST;
static int s_pin_busy = SX1262_RL_DEFAULT_BUSY;
static int s_pin_irq  = SX1262_RL_DEFAULT_IRQ;
static spi_host_device_t s_spi_host = SPI2_HOST;
#define SX1262_RL_SPI_FREQ (8 * 1000 * 1000)  // 8 MHz, matches sx1262.c

// Layout for a "sx1262_rl.pins" .purr v2 quirk block — see purr_quirk_pkg.h.
// Plain int32_t fields matching sx1262_rl_configure()'s own parameter list —
// deliberately a SEPARATE block name from sx1262.c's "sx1262.pins" even
// though the layout is identical, since a device could in principle
// (never does today) select either driver and the two should never be
// confused for one another.
typedef struct {
    int32_t mosi;
    int32_t miso;
    int32_t sclk;
    int32_t cs;
    int32_t rst;
    int32_t busy;
    int32_t irq;
} sx1262_rl_pin_quirk_t;

extern "C" void sx1262_rl_configure(int mosi, int miso, int sclk, int cs, int rst, int busy, int irq)
{
    // A loaded .purr v2 quirk package's "sx1262_rl.pins" block, if present
    // and correctly sized, overrides the CALLER's own values — same
    // precedence gt911_configure()/adc_battery's own quirk checks already use.
    size_t qsz = 0;
    const void *qblk = purr_quirk_get_block("sx1262_rl.pins", &qsz);
    if (qblk && qsz == sizeof(sx1262_rl_pin_quirk_t)) {
        const sx1262_rl_pin_quirk_t *q = (const sx1262_rl_pin_quirk_t *)qblk;
        mosi = q->mosi; miso = q->miso; sclk = q->sclk;
        cs = q->cs; rst = q->rst; busy = q->busy; irq = q->irq;
        ESP_LOGI(TAG, "using loaded quirk package's pin assignment instead of caller's "
                      "(mosi=%d miso=%d sclk=%d cs=%d rst=%d busy=%d irq=%d)",
                 mosi, miso, sclk, cs, rst, busy, irq);
    } else if (qblk) {
        ESP_LOGW(TAG, "quirk block 'sx1262_rl.pins' has wrong size (%u, expected %u) — ignoring",
                 (unsigned)qsz, (unsigned)sizeof(sx1262_rl_pin_quirk_t));
    }

    s_pin_mosi = mosi;
    s_pin_miso = miso;
    s_pin_sclk = sclk;
    s_pin_cs   = cs;
    s_pin_rst  = rst;
    s_pin_busy = busy;
    s_pin_irq  = irq;
}

extern "C" void sx1262_rl_set_spi_host(spi_host_device_t host)
{
    s_spi_host = host;
}

// This board's SX1262 module runs off a TCXO enabled via DIO3, not a bare
// crystal — confirmed from Meshtastic's own T-Deck variant.h
// (SX126X_DIO3_TCXO_VOLTAGE 1.8, "if DIO3 is high the TXCO is enabled").
// The hand-rolled sx1262.c driver never drove DIO3 at all; RadioLib's
// begin() does it as part of its normal sequence when given this voltage.
#define SX1262_RL_TCXO_VOLTAGE 1.8f

// ── State ─────────────────────────────────────────────────────────────────

static EspHal *s_hal    = nullptr;
static Module *s_module = nullptr;
static SX1262 *s_radio  = nullptr;

static float s_last_rssi;
static float s_last_snr;

// ── Interrupt-driven RX signal ───────────────────────────────────────────
//
// meshtastic_module.c's mesh_task() used to poll data_available() (a real
// SPI getIrqFlags() transaction) unconditionally every 10ms, forever —
// confirmed this session, via a direct comparison against Meshtastic's own
// official firmware (which runs stably on this same shared-SPI2-bus
// hardware), that this is a large, unforced source of SPI traffic/bus
// contention their reference design doesn't have: they wake a worker task
// off a DIO1 GPIO edge interrupt instead, only touching the radio via SPI
// when something actually happened. This is the PURR-OS side of that same
// pattern — sx1262_rl_wait_rx_signal() below is what mesh_task() now
// blocks on instead of a flat vTaskDelay(10).
//
// The ISR itself must never touch SPI/RadioLib state (mesh_task() does
// all the actual SPI work, same as before) and must only call ISR-safe
// FreeRTOS primitives — xSemaphoreGiveFromISR(), not mesh_radio_lock()'s
// blocking xSemaphoreTake(..., portMAX_DELAY), which is illegal from
// interrupt context.
static SemaphoreHandle_t s_rx_sem = nullptr;

static void IRAM_ATTR sx1262_rl_dio1_isr(void)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

extern "C" bool sx1262_rl_wait_rx_signal(uint32_t timeout_ms)
{
    if (!s_rx_sem) return false;
    return xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// Task-context-only wake — see catcall_radio_t.wake_rx_wait's doc comment.
// A plain xSemaphoreGive() (not the ISR variant) is correct here since
// this is only ever called from a normal task (e.g. mesh_manager_send_
// text(), itself called from a UI button press), never from interrupt
// context. Reuses the same semaphore as the RX ISR — mesh_task() doesn't
// need to know WHY it woke up, only that it should go check both RX and
// its TX queue again.
extern "C" void sx1262_rl_wake_rx_wait(void)
{
    if (s_rx_sem) xSemaphoreGive(s_rx_sem);
}

static uint8_t clamp_coding_rate(uint8_t cr)
{
    // catcall_radio_t uses 5..8 for CR4/5..CR4/8 — same raw convention
    // RadioLib's begin()/setCodingRate() take directly.
    return (cr >= 5 && cr <= 8) ? cr : 5;
}

// ── Catcall: init ─────────────────────────────────────────────────────────

static esp_err_t sx1262_rl_init(const radio_config_t *cfg)
{
    uint32_t freq_hz = cfg ? cfg->frequency_hz     : 868000000UL;
    uint8_t  sf      = cfg ? cfg->spreading_factor  : 7;
    uint32_t bw_hz   = cfg ? cfg->bandwidth_hz      : 125000;
    uint8_t  cr      = cfg ? cfg->coding_rate       : 5;
    uint8_t  power   = cfg ? cfg->tx_power_dbm      : 14;

    if (!s_hal) {
        s_hal = new EspHal(s_pin_sclk, s_pin_miso, s_pin_mosi, s_spi_host, SX1262_RL_SPI_FREQ);
    }
    if (!s_module) {
        s_module = new Module(s_hal, s_pin_cs, s_pin_irq, s_pin_rst, s_pin_busy);
    }
    if (!s_radio) {
        s_radio = new SX1262(s_module);
    }

    float freq_mhz = (float)freq_hz / 1.0e6f;
    float bw_khz   = (float)bw_hz / 1.0e3f;

    // useRegulatorLDO=false — DC-DC regulator, matches sx1262.c's original
    // CMD_SET_REGULATOR_MODE=0x01 choice.
    int16_t state = s_radio->begin(freq_mhz, bw_khz, sf, clamp_coding_rate(cr),
                                    RADIOLIB_SX126X_SYNC_WORD_PRIVATE, (int8_t)power,
                                    8, SX1262_RL_TCXO_VOLTAGE, false);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "begin() failed, code %d", state);
        return ESP_FAIL;
    }

    // Most integrated SX1262 modules (this board's included) wire DIO2 to
    // auto-drive the antenna's TX/RX switch — see sx1262.c's matching
    // comment for the full story (this exact command, previously hand-
    // rolled and ordering-sensitive, is now just one RadioLib call issued
    // at the right point in RadioLib's own internal sequencing).
    state = s_radio->setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "setDio2AsRfSwitch() failed, code %d", state);
    }

    state = s_radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "startReceive() failed, code %d", state);
        return ESP_FAIL;
    }

    // Attached AFTER startReceive() succeeds, not before — starting RX
    // clears stale on-chip interrupt-pending bits (confirmed via
    // Meshtastic's own SX126xInterface::startReceive(), which documents
    // this exact ordering requirement), so attaching the host-side GPIO
    // interrupt first risks it firing once immediately on leftover stale
    // state. This is a one-time host-side GPIO attach (EspHal::
    // attachInterrupt() -> gpio_isr_handler_add()) — subsequent
    // startReceive() re-arms elsewhere in this file (set_frequency etc.)
    // only touch the on-chip IRQ mask over SPI, they don't need to redo
    // this attach.
    if (!s_rx_sem) {
        s_rx_sem = xSemaphoreCreateBinary();
    }
    s_radio->setDio1Action(sx1262_rl_dio1_isr);

    ESP_LOGI(TAG, "init OK (RadioLib)  freq=%luHz SF%u BW%lu CR4/%u pwr=%ddBm tcxo=%.1fV",
             (unsigned long)freq_hz, sf, (unsigned long)(bw_hz / 1000), cr, power,
             (double)SX1262_RL_TCXO_VOLTAGE);
    return ESP_OK;
}

// ── Catcall: send ─────────────────────────────────────────────────────────

static esp_err_t sx1262_rl_send(const uint8_t *data, size_t len)
{
    if (!s_radio || !data || len == 0 || len > 255) return ESP_ERR_INVALID_ARG;

    int16_t state = s_radio->transmit((uint8_t *)data, len);

    // transmit() leaves the radio in standby — re-arm continuous RX,
    // matching sx1262.c's send()->start_rx_continuous() pattern.
    s_radio->startReceive();

    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "transmit() failed, code %d", state);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "sent %u bytes", (unsigned)len);
    return ESP_OK;
}

// ── Catcall: data_available ─────────────────────────────────────────────

static bool sx1262_rl_data_available(void)
{
    if (!s_radio) return false;
    uint32_t irq = s_radio->getIrqFlags();
    return (irq & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
}

// ── Catcall: receive ─────────────────────────────────────────────────────

static int sx1262_rl_receive(uint8_t *buf, size_t max_len)
{
    if (!s_radio || !buf || max_len == 0) return -1;

    uint32_t irq = s_radio->getIrqFlags();
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) return 0;  // nothing yet

    // readData() internally re-queries the real packet length + RX buffer
    // offset via its own GetRxBufferStatus call and honors max_len itself
    // (truncates if smaller) — a separate getPacketLength() call before
    // this was redundant (an extra independent SPI round trip querying the
    // same state readData() immediately re-queries anyway) and is removed;
    // fewer independent SPI transactions per packet, less chance of the
    // chip's internal state moving between them.
    int16_t state = s_radio->readData(buf, max_len);
    size_t  pkt_len = s_radio->getPacketLength(false);
    if (pkt_len > max_len) pkt_len = max_len;

    // readData() leaves the radio in standby regardless of outcome —
    // re-arm continuous RX before returning, same as sx1262.c.
    s_radio->startReceive();

    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        ESP_LOGW(TAG, "CRC error");
        return -1;
    }
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "readData() failed, code %d", state);
        return -1;
    }
    if (pkt_len == 0) return 0;

    s_last_rssi = s_radio->getRSSI();
    s_last_snr  = s_radio->getSNR();

    ESP_LOGD(TAG, "recv %u bytes RSSI=%.1f SNR=%.1f", (unsigned)pkt_len,
             (double)s_last_rssi, (double)s_last_snr);
    return (int)pkt_len;
}

// ── Catcall: rssi / snr ───────────────────────────────────────────────────

static int   sx1262_rl_rssi(void) { return (int)s_last_rssi; }
static float sx1262_rl_snr(void)  { return s_last_snr; }

// ── Catcall: set_frequency / set_power / set_modulation / set_sync_word ──

static esp_err_t sx1262_rl_set_frequency(uint32_t hz)
{
    if (!s_radio) return ESP_ERR_INVALID_STATE;
    int16_t state = s_radio->setFrequency((float)hz / 1.0e6f);
    s_radio->startReceive();
    return state == RADIOLIB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

static esp_err_t sx1262_rl_set_power(uint8_t dbm)
{
    if (!s_radio) return ESP_ERR_INVALID_STATE;
    int16_t state = s_radio->setOutputPower((int8_t)dbm);
    s_radio->startReceive();
    return state == RADIOLIB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

static esp_err_t sx1262_rl_set_modulation(uint8_t sf, uint32_t bw_hz, uint8_t cr)
{
    if (!s_radio) return ESP_ERR_INVALID_STATE;
    int16_t state = s_radio->setBandwidth((float)bw_hz / 1.0e3f);
    if (state == RADIOLIB_ERR_NONE) state = s_radio->setSpreadingFactor(sf);
    if (state == RADIOLIB_ERR_NONE) state = s_radio->setCodingRate(clamp_coding_rate(cr));
    s_radio->startReceive();
    return state == RADIOLIB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

static esp_err_t sx1262_rl_set_sync_word(uint8_t sync)
{
    if (!s_radio) return ESP_ERR_INVALID_STATE;
    // controlBits defaults to 0x44 — the same "0x04 low nibble in each
    // register byte" SX126x quirk sx1262.c's set_sync_word() hand-encoded.
    int16_t state = s_radio->setSyncWord(sync);
    s_radio->startReceive();
    return state == RADIOLIB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

// ── Catcall: deinit ───────────────────────────────────────────────────────

static esp_err_t sx1262_rl_deinit(void)
{
    if (s_radio) { s_radio->sleep(); }
    ESP_LOGI(TAG, "deinit OK");
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────

static const catcall_radio_t s_catcall = {
    .name            = "sx1262_rl",
    .catcall_version = CATCALL_RADIO_VERSION,
    .init            = sx1262_rl_init,
    .send            = sx1262_rl_send,
    .receive         = sx1262_rl_receive,
    .data_available  = sx1262_rl_data_available,
    .rssi            = sx1262_rl_rssi,
    .snr             = sx1262_rl_snr,
    .set_frequency   = sx1262_rl_set_frequency,
    .set_power       = sx1262_rl_set_power,
    .set_modulation  = sx1262_rl_set_modulation,
    .set_sync_word   = sx1262_rl_set_sync_word,
    .deinit          = sx1262_rl_deinit,
    .wait_rx_signal  = sx1262_rl_wait_rx_signal,
    .wake_rx_wait    = sx1262_rl_wake_rx_wait,
};

// ── Module lifecycle ──────────────────────────────────────────────────────

static int module_init(void)
{
    static const radio_config_t default_cfg = {
        .frequency_hz     = 868000000UL,
        .tx_power_dbm     = 14,
        .spreading_factor = 7,
        .bandwidth_hz     = 125000,
        .coding_rate      = 5,
    };
    esp_err_t ret = sx1262_rl_init(&default_cfg);
    if (ret != ESP_OK) return -1;
    purr_kernel_register_radio(&s_catcall);
    return 0;
}

static void module_deinit(void)
{
    sx1262_rl_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────
extern "C" {
#include "../../../kernel/core/purr_module.h"

PURR_MODULE_REGISTER(sx1262_rl) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "sx1262_rl",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_RADIO,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
}
