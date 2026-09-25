// purr_probe_dma.c — see purr_probe_dma.h for what each measurement is for.

#include "purr_probe_dma.h"
#include "purr_probe_spi.h"
#include "purr_probe_guard.h"

#include "esp_private/gdma.h"
#include "hal/gdma_types.h"
#include "hal/dma_types.h"
#include "soc/soc.h"
#include "soc/spi_reg.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "probe_dma";

#define PROBE_SPI_IDX  2
#define PROBE_PIN_CS   12
#define PROBE_PIN_DC   11
#define DMA_TIMEOUT_CYCLES 24000000u   // ~150 ms at 160 MHz

static gdma_channel_handle_t s_tx_chan = NULL;
static dma_descriptor_t     *s_desc = NULL;
static uint8_t              *s_buf  = NULL;

static gdma_channel_handle_t s_rx_chan = NULL;
static dma_descriptor_t     *s_rx_desc = NULL;
static uint8_t              *s_rx_buf  = NULL;

static uint32_t rq(uint32_t a) { uint32_t v = 0; (void)probe_guard_read32(a, &v); return v; }
static void     wq(uint32_t a, uint32_t v) { (void)probe_guard_write32(a, v); }

esp_err_t probe_dma_open(void)
{
    if (s_tx_chan) return ESP_OK;

    gdma_channel_alloc_config_t cfg = { .direction = GDMA_CHANNEL_DIRECTION_TX };
    esp_err_t e = gdma_new_ahb_channel(&cfg, &s_tx_chan);
    if (e != ESP_OK) { ESP_LOGE(TAG, "gdma_new_ahb_channel: %s", esp_err_to_name(e)); return e; }

    e = gdma_connect(s_tx_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "gdma_connect: %s", esp_err_to_name(e));
        gdma_del_channel(s_tx_chan); s_tx_chan = NULL;
        return e;
    }

    // Descriptor and buffer must both be in DMA-capable INTERNAL memory. PSRAM
    // would need cache writeback handling before the engine could see the data,
    // and that is a second variable this measurement does not need.
    s_desc = heap_caps_calloc(1, sizeof(dma_descriptor_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf  = heap_caps_calloc(1, PROBE_DMA_MAX_XFER,       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_desc || !s_buf) {
        ESP_LOGE(TAG, "no DMA-capable memory");
        probe_dma_close();
        return ESP_ERR_NO_MEM;
    }
    // RX channel, for the full-duplex path. Allocated alongside TX rather than
    // lazily: a failure here should surface at `dmaopen` where it is obvious,
    // not midway through a measurement.
    gdma_channel_alloc_config_t rxcfg = { .direction = GDMA_CHANNEL_DIRECTION_RX };
    e = gdma_new_ahb_channel(&rxcfg, &s_rx_chan);
    if (e == ESP_OK) {
        e = gdma_connect(s_rx_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2));
        if (e != ESP_OK) { gdma_del_channel(s_rx_chan); s_rx_chan = NULL; }
    } else {
        s_rx_chan = NULL;
    }
    if (s_rx_chan) {
        s_rx_desc = heap_caps_calloc(1, sizeof(dma_descriptor_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_rx_buf  = heap_caps_calloc(1, PROBE_DMA_MAX_XFER,       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (!s_rx_chan || !s_rx_desc || !s_rx_buf) {
        // TX-only still works and is worth having, so this degrades rather than
        // failing the whole open — but say so, because a silently TX-only probe
        // would make `dmarx` look broken for the wrong reason.
        ESP_LOGW(TAG, "RX channel unavailable (%s) — TX-only", esp_err_to_name(e));
    }

    ESP_LOGI(TAG, "GDMA connected to SPI2; tx desc=%p buf=%p rx desc=%p buf=%p",
             s_desc, s_buf, s_rx_desc, s_rx_buf);
    return ESP_OK;
}

void probe_dma_close(void)
{
    if (s_tx_chan) { gdma_disconnect(s_tx_chan); gdma_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { gdma_disconnect(s_rx_chan); gdma_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_desc)    { heap_caps_free(s_desc);    s_desc    = NULL; }
    if (s_buf)     { heap_caps_free(s_buf);     s_buf     = NULL; }
    if (s_rx_desc) { heap_caps_free(s_rx_desc); s_rx_desc = NULL; }
    if (s_rx_buf)  { heap_caps_free(s_rx_buf);  s_rx_buf  = NULL; }
}

bool probe_dma_is_open(void) { return s_tx_chan != NULL; }

esp_err_t probe_dma_channel_info(int *tx_id, int *rx_id,
                                 uint32_t *tx_desc_addr, uint32_t *rx_desc_addr)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    if (tx_id) { *tx_id = -1; gdma_get_channel_id(s_tx_chan, tx_id); }
    if (rx_id) { *rx_id = -1; if (s_rx_chan) gdma_get_channel_id(s_rx_chan, rx_id); }
    if (tx_desc_addr) *tx_desc_addr = (uint32_t)(uintptr_t)s_desc;
    if (rx_desc_addr) *rx_desc_addr = (uint32_t)(uintptr_t)s_rx_desc;
    return ESP_OK;
}

static void snap_desc_of(const dma_descriptor_t *d, probe_dma_desc_t *out)
{
    const volatile uint32_t *w = (const volatile uint32_t *)d;
    out->dw0    = w[0];
    out->buffer = w[1];
    out->next   = w[2];
    out->size    = (out->dw0 >>  0) & 0xFFF;
    out->length  = (out->dw0 >> 12) & 0xFFF;
    out->suc_eof = (out->dw0 >> 30) & 0x1;
    out->owner   = (out->dw0 >> 31) & 0x1;
}

esp_err_t probe_dma_xfer(uint8_t pattern, size_t len, int dc_level,
                         probe_dma_result_t *out)
{
    if (!s_tx_chan)            return ESP_ERR_INVALID_STATE;
    if (!probe_spi_is_open())  return ESP_ERR_INVALID_STATE;
    if (out == NULL)           return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > PROBE_DMA_MAX_XFER) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->bytes = len;
    memset(s_buf, pattern, len);

    // Build the descriptor by hand rather than via a helper, so the exact bit
    // layout an emulator must decode is visible in one place.
    s_desc->dw0.size    = len;
    s_desc->dw0.length  = len;
    s_desc->dw0.suc_eof = 1;
    s_desc->dw0.owner   = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    s_desc->buffer      = s_buf;
    s_desc->next        = NULL;
    snap_desc_of(s_desc, &out->before);

    // TX-only: MOSI on, MISO off. Full duplex over DMA would need an RX
    // channel and descriptor too, which is a separate characterisation.
    wq(SPI_USER_REG(PROBE_SPI_IDX), SPI_USR_MOSI);
    wq(SPI_MS_DLEN_REG(PROBE_SPI_IDX), (uint32_t)(len * 8 - 1) & SPI_MS_DATA_BITLEN_V);

    // Reset the DMA and buffer AFIFOs before arming. Skipping this leaves stale
    // bytes from a previous transfer at the head of the stream — a failure that
    // looks like a corrupt image rather than a DMA bug.
    wq(SPI_DMA_CONF_REG(PROBE_SPI_IDX), SPI_DMA_AFIFO_RST | SPI_BUF_AFIFO_RST);
    wq(SPI_DMA_CONF_REG(PROBE_SPI_IDX), SPI_DMA_TX_ENA);

    wq(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);
    uint32_t tu = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_UPDATE);
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_UPDATE) {
        if (esp_cpu_get_cycle_count() - tu > DMA_TIMEOUT_CYCLES) break;
    }

    esp_err_t e = gdma_start(s_tx_chan, (intptr_t)s_desc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "gdma_start: %s", esp_err_to_name(e)); return e; }

    gpio_set_level(PROBE_PIN_DC, dc_level ? 1 : 0);
    gpio_set_level(PROBE_PIN_CS, 0);

    uint32_t t0 = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_USR);

    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_USR) {
        if (esp_cpu_get_cycle_count() - t0 > DMA_TIMEOUT_CYCLES) { out->timed_out = true; break; }
    }
    uint32_t t1 = esp_cpu_get_cycle_count();
    gpio_set_level(PROBE_PIN_CS, 1);

    out->cycles_to_done        = out->timed_out ? 0 : (t1 - t0);
    out->spi_dma_int_raw_after = rq(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));
    out->spi_dma_conf_after    = rq(SPI_DMA_CONF_REG(PROBE_SPI_IDX));
    out->trans_done_seen       = (out->spi_dma_int_raw_after & SPI_TRANS_DONE_INT_RAW) != 0;

    snap_desc_of(s_desc, &out->after);
    out->owner_cleared  = (out->before.owner == 1 && out->after.owner == 0);
    out->length_updated = (out->before.length != out->after.length);

    gdma_stop(s_tx_chan);
    wq(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);
    wq(SPI_DMA_CONF_REG(PROBE_SPI_IDX), 0);
    return out->timed_out ? ESP_ERR_TIMEOUT : ESP_OK;
}

// ── Full duplex ──────────────────────────────────────────────────────────────
//
// The path a panel READ takes, and the one a display-read timeout lives on.

esp_err_t probe_dma_xfer_duplex(uint8_t pattern, size_t len, int dc_level,
                                probe_dma_result_t *out, probe_dma_rx_t *rx)
{
    if (!s_tx_chan || !s_rx_chan) return ESP_ERR_INVALID_STATE;
    if (!probe_spi_is_open())     return ESP_ERR_INVALID_STATE;
    if (out == NULL || rx == NULL) return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > PROBE_DMA_MAX_XFER) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    memset(rx, 0, sizeof(*rx));
    out->bytes = len;
    memset(s_buf, pattern, len);

    // Sentinel, not zero. If the engine writes nothing, a zero-filled buffer is
    // indistinguishable from a real all-zero read — and MISO on this board
    // genuinely does read 0x00, so that ambiguity is guaranteed to bite.
    memset(s_rx_buf, 0xEE, len);

    s_desc->dw0.size    = len;
    s_desc->dw0.length  = len;
    s_desc->dw0.suc_eof = 1;
    s_desc->dw0.owner   = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    s_desc->buffer      = s_buf;
    s_desc->next        = NULL;
    snap_desc_of(s_desc, &out->before);

    // RX descriptor: `size` is the space available, `length` starts at 0 for
    // the engine to fill in with what actually arrived. That asymmetry with TX
    // (where length is what to send) is itself worth an emulator noting.
    s_rx_desc->dw0.size    = len;
    s_rx_desc->dw0.length  = 0;
    s_rx_desc->dw0.suc_eof = 0;
    s_rx_desc->dw0.owner   = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    s_rx_desc->buffer      = s_rx_buf;
    s_rx_desc->next        = NULL;
    snap_desc_of(s_rx_desc, &rx->rx_before);

    // Full duplex needs DOUTDIN as well as both phase enables.
    wq(SPI_USER_REG(PROBE_SPI_IDX), SPI_DOUTDIN | SPI_USR_MOSI | SPI_USR_MISO);
    wq(SPI_MS_DLEN_REG(PROBE_SPI_IDX), (uint32_t)(len * 8 - 1) & SPI_MS_DATA_BITLEN_V);

    wq(SPI_DMA_CONF_REG(PROBE_SPI_IDX), SPI_DMA_AFIFO_RST | SPI_BUF_AFIFO_RST);
    // RX_EOF_EN so the engine raises EOF once MS_DLEN bytes have landed, rather
    // than waiting for a descriptor boundary it will never reach.
    wq(SPI_DMA_CONF_REG(PROBE_SPI_IDX),
       SPI_DMA_TX_ENA | SPI_DMA_RX_ENA | SPI_RX_EOF_EN);

    wq(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);
    uint32_t tu = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_UPDATE);
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_UPDATE) {
        if (esp_cpu_get_cycle_count() - tu > DMA_TIMEOUT_CYCLES) break;
    }

    // RX armed before TX. The receive path has to be ready to accept bytes
    // before any are clocked out, or the first ones are lost with nothing
    // reporting it.
    esp_err_t e = gdma_start(s_rx_chan, (intptr_t)s_rx_desc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "gdma_start rx: %s", esp_err_to_name(e)); return e; }
    e = gdma_start(s_tx_chan, (intptr_t)s_desc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "gdma_start tx: %s", esp_err_to_name(e)); gdma_stop(s_rx_chan); return e; }

    gpio_set_level(PROBE_PIN_DC, dc_level ? 1 : 0);
    gpio_set_level(PROBE_PIN_CS, 0);

    uint32_t t0 = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_USR);
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_USR) {
        if (esp_cpu_get_cycle_count() - t0 > DMA_TIMEOUT_CYCLES) { out->timed_out = true; break; }
    }
    uint32_t t1 = esp_cpu_get_cycle_count();
    gpio_set_level(PROBE_PIN_CS, 1);

    out->cycles_to_done        = out->timed_out ? 0 : (t1 - t0);
    out->spi_dma_int_raw_after = rq(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));
    out->spi_dma_conf_after    = rq(SPI_DMA_CONF_REG(PROBE_SPI_IDX));
    out->trans_done_seen       = (out->spi_dma_int_raw_after & SPI_TRANS_DONE_INT_RAW) != 0;

    snap_desc_of(s_desc,    &out->after);
    snap_desc_of(s_rx_desc, &rx->rx_after);
    out->owner_cleared  = (out->before.owner == 1 && out->after.owner == 0);
    out->length_updated = (out->before.length != out->after.length);

    rx->rx_owner_cleared  = (rx->rx_before.owner == 1 && rx->rx_after.owner == 0);
    rx->rx_length_updated = (rx->rx_before.length != rx->rx_after.length);
    rx->rx_length         = rx->rx_after.length;

    for (size_t i = 0; i < 8 && i < len; i++) rx->rx_first[i] = s_rx_buf[i];
    for (size_t i = 0; i < len; i++) {
        if (s_rx_buf[i] != 0xEE) { rx->rx_buffer_changed = true; break; }
    }

    gdma_stop(s_tx_chan);
    gdma_stop(s_rx_chan);
    wq(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);
    wq(SPI_DMA_CONF_REG(PROBE_SPI_IDX), 0);
    return out->timed_out ? ESP_ERR_TIMEOUT : ESP_OK;
}
