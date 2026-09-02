// purr_probe_int.c — see purr_probe_int.h for what each measurement is for.

#include "purr_probe_int.h"
#include "purr_probe_spi.h"
#include "purr_probe_guard.h"

#include "soc/soc.h"
#include "soc/spi_reg.h"
#include "soc/interrupts.h"
#include "esp_intr_alloc.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "probe_int";

#define PROBE_SPI_IDX 2

// The ISR reads and writes SPI2 registers directly rather than through
// probe_guard_*(). That is consistent with the guard's contract, not an
// exception to it: the guard exists to gate addresses that arrive FROM A HOST,
// and every address here is a compile-time constant in an already-allowed
// window. The guard's accessors also log on refusal, and ESP_LOG from an ISR is
// not something to invite.
#define R(reg)      (*(volatile uint32_t *)(uintptr_t)(reg))

static intr_handle_t s_handle = NULL;

static volatile uint32_t s_count;
static volatile uint32_t s_cyc_isr;
static volatile uint32_t s_core;
static volatile uint32_t s_st_at_entry;
static volatile uint32_t s_raw_at_entry;

// Masks instead of clearing RAW. Two reasons, both load-bearing:
//
//   1. ETS_SPI2_INTR_SOURCE is level-triggered. A handler that returns with the
//      line still asserted re-enters immediately and livelocks the core. So it
//      must do SOMETHING before returning.
//   2. Clearing RAW would destroy the very thing being measured — whether RAW
//      survives independently of the enable mask. Masking deasserts the line
//      while leaving RAW intact for the caller to inspect.
static void spi_probe_isr(void *arg)
{
    (void)arg;
    if (s_count == 0) {
        s_cyc_isr      = esp_cpu_get_cycle_count();
        s_core         = esp_cpu_get_core_id();
        s_st_at_entry  = R(SPI_DMA_INT_ST_REG(PROBE_SPI_IDX));
        s_raw_at_entry = R(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));
    }
    s_count++;
    R(SPI_DMA_INT_ENA_REG(PROBE_SPI_IDX)) = 0;
}

// ── The safe half: no CPU interrupt is ever enabled ──────────────────────────

esp_err_t probe_int_matrix(probe_int_row_t *rows, size_t max, size_t *out_n)
{
    if (!probe_spi_is_open()) return ESP_ERR_INVALID_STATE;
    if (rows == NULL || out_n == NULL || max < 4) return ESP_ERR_INVALID_ARG;

    const uint32_t ENA  = SPI_DMA_INT_ENA_REG(PROBE_SPI_IDX);
    const uint32_t RAW  = SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX);
    const uint32_t ST   = SPI_DMA_INT_ST_REG(PROBE_SPI_IDX);
    const uint32_t CLR  = SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX);
    const uint32_t BIT_TD = SPI_TRANS_DONE_INT_RAW;

    size_t n = 0;
    uint32_t v;

    // Row 0 — ENA=0 with RAW clear. The baseline both other rows are read
    // against.
    (void)probe_guard_write32(ENA, 0);
    (void)probe_guard_write32(CLR, BIT_TD);
    rows[n].ena_written = 0;
    (void)probe_guard_read32(RAW, &v); rows[n].raw_before = v;
    (void)probe_guard_read32(ST,  &v); rows[n].st_before  = v;
    rows[n].raw_preset = false;
    rows[n].raw_after = rows[n].raw_before;
    rows[n].st_after  = rows[n].st_before;
    n++;

    // Row 1 — run a transfer with ENA still 0, so RAW sets while masked.
    // Does ST follow RAW regardless of the mask, or is it gated by it?
    uint8_t tx[4] = { 0x00, 0x00, 0x00, 0x00 };
    uint32_t cyc;
    if (probe_spi_start_async(tx, sizeof(tx), 0, &cyc) == ESP_OK) {
        probe_spi_finish_async();
    }
    rows[n].ena_written = 0;
    (void)probe_guard_read32(RAW, &v); rows[n].raw_before = v;
    (void)probe_guard_read32(ST,  &v); rows[n].st_before  = v;
    rows[n].raw_preset = (rows[n].raw_before & BIT_TD) != 0;
    rows[n].raw_after = rows[n].raw_before;
    rows[n].st_after  = rows[n].st_before;
    n++;

    // Row 2 — THE lost-wakeup question. RAW is already set from row 1. Arm ENA
    // now, after the fact. If ST goes high, a late-arming reader still sees the
    // event; if it stays low, the event was missed permanently and firmware
    // that arms after the hardware finished will block forever.
    rows[n].ena_written = BIT_TD;
    (void)probe_guard_read32(RAW, &v); rows[n].raw_before = v;
    (void)probe_guard_read32(ST,  &v); rows[n].st_before  = v;
    rows[n].raw_preset = (rows[n].raw_before & BIT_TD) != 0;
    (void)probe_guard_write32(ENA, BIT_TD);
    (void)probe_guard_read32(RAW, &v); rows[n].raw_after = v;
    (void)probe_guard_read32(ST,  &v); rows[n].st_after  = v;
    n++;

    // Row 3 — clear RAW while ENA stays armed. Does ST drop with it?
    rows[n].ena_written = BIT_TD;
    (void)probe_guard_read32(RAW, &v); rows[n].raw_before = v;
    (void)probe_guard_read32(ST,  &v); rows[n].st_before  = v;
    rows[n].raw_preset = (rows[n].raw_before & BIT_TD) != 0;
    (void)probe_guard_write32(CLR, BIT_TD);
    (void)probe_guard_read32(RAW, &v); rows[n].raw_after = v;
    (void)probe_guard_read32(ST,  &v); rows[n].st_after  = v;
    n++;

    // Leave the peripheral masked and quiet.
    (void)probe_guard_write32(ENA, 0);
    (void)probe_guard_write32(CLR, BIT_TD);

    *out_n = n;
    return ESP_OK;
}

// ── The half that routes a real interrupt to the CPU ─────────────────────────

esp_err_t probe_int_fire(const uint8_t *tx, size_t len, int dc_level,
                         probe_int_result_t *out)
{
    if (!probe_spi_is_open()) return ESP_ERR_INVALID_STATE;
    if (tx == NULL || out == NULL) return ESP_ERR_INVALID_ARG;

    const uint32_t ENA = SPI_DMA_INT_ENA_REG(PROBE_SPI_IDX);
    const uint32_t RAW = SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX);
    const uint32_t ST  = SPI_DMA_INT_ST_REG(PROBE_SPI_IDX);
    const uint32_t CLR = SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX);

    if (s_handle == NULL) {
        // Masked and RAW cleared BEFORE the handler is allowed to exist. A
        // level-triggered source with its raw bit already set would otherwise
        // fire the instant esp_intr_alloc() unmasks it, before the measurement
        // has even started — and the first thing that handler does is record a
        // latency relative to a transfer that has not happened yet.
        (void)probe_guard_write32(ENA, 0);
        (void)probe_guard_write32(CLR, SPI_TRANS_DONE_INT_CLR);

        esp_err_t e = esp_intr_alloc(ETS_SPI2_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1,
                                     spi_probe_isr, NULL, &s_handle);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_intr_alloc: %s", esp_err_to_name(e));
            return e;
        }
    }

    memset(out, 0, sizeof(*out));
    s_count = 0; s_cyc_isr = 0; s_core = 0; s_st_at_entry = 0; s_raw_at_entry = 0;

    (void)probe_guard_write32(CLR, SPI_TRANS_DONE_INT_CLR);
    (void)probe_guard_write32(ENA, SPI_TRANS_DONE_INT_ENA);

    uint32_t cyc_usr = 0;
    esp_err_t e = probe_spi_start_async(tx, len, dc_level, &cyc_usr);
    if (e != ESP_OK) {
        (void)probe_guard_write32(ENA, 0);
        return e;
    }
    out->cyc_usr_set = cyc_usr;

    // Spin, not vTaskDelay: the interval being measured is a few thousand
    // cycles and a tick delay would quantise it into meaninglessness. Bounded
    // so a non-firing interrupt reports "did not fire" instead of hanging the
    // command loop and taking the watchdog with it.
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (s_count == 0) {
        if (esp_cpu_get_cycle_count() - t0 > 160000000u / 10u) break;  // ~100 ms
    }

    probe_spi_finish_async();

    out->fired        = (s_count != 0);
    out->count        = s_count;
    out->cyc_isr      = s_cyc_isr;
    out->core         = s_core;
    out->st_at_entry  = s_st_at_entry;
    out->raw_at_entry = s_raw_at_entry;

    uint32_t v;
    (void)probe_guard_read32(RAW, &v); out->raw_after = v;
    (void)probe_guard_read32(ST,  &v); out->st_after  = v;

    (void)probe_guard_write32(ENA, 0);
    (void)probe_guard_write32(CLR, SPI_TRANS_DONE_INT_CLR);
    return ESP_OK;
}

void probe_int_uninstall(void)
{
    if (s_handle) {
        (void)probe_guard_write32(SPI_DMA_INT_ENA_REG(PROBE_SPI_IDX), 0);
        esp_intr_free(s_handle);
        s_handle = NULL;
    }
}
