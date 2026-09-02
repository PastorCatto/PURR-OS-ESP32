// purr_probe_spi.c — see purr_probe_spi.h for what this is for.

#include "purr_probe_spi.h"
#include "purr_probe_guard.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/spi_reg.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "probe_spi";

// T-Deck Plus display bus. Mirrors kernel_tdp_boot.c's TDP_DISPLAY_* /
// TDP_SD_MISO — the SD card, LoRa radio and panel all hang off SPI2 here.
#define PROBE_SPI_HOST   SPI2_HOST
#define PROBE_SPI_IDX    2          // REG_SPI_BASE(2) == DR_REG_SPI2_BASE
#define PROBE_PIN_MOSI   41
#define PROBE_PIN_MISO   38
#define PROBE_PIN_SCLK   40
#define PROBE_PIN_CS     12
#define PROBE_PIN_DC     11

// Cap on how long a poll loop will spin before declaring the transfer stuck.
// 24M cycles is ~100 ms at 240 MHz — six orders of magnitude longer than a
// 64-byte transfer needs, so hitting it is a finding, not a tuning problem.
#define PROBE_POLL_TIMEOUT_CYCLES  24000000u

// How many poll iterations get recorded before the loop goes quiet. Eight is
// enough to show the transition on a healthy transfer (which completes in one
// or two) and to establish the steady-state read value on a stuck one.
#define PROBE_POLL_TRACE_LIMIT     8u

static bool                s_open = false;
static probe_spi_ev_t      s_ev[PROBE_SPI_MAX_EVENTS];
static size_t              s_ev_n = 0;
static bool                s_ev_overflow = false;
static probe_spi_timing_t  s_timing;

// ── Traced accessors ─────────────────────────────────────────────────────────
//
// Every SPI2 access in this file goes through these two, so the trace is
// complete by construction rather than by remembering to log. Both delegate to
// the guard; a refusal is recorded with value 0xDEADBEEF and the bus untouched.

static void ev_push(uint32_t addr, uint32_t value, bool write)
{
    if (s_ev_n >= PROBE_SPI_MAX_EVENTS) { s_ev_overflow = true; return; }
    s_ev[s_ev_n].cycle = esp_cpu_get_cycle_count();
    s_ev[s_ev_n].addr  = addr;
    s_ev[s_ev_n].value = value;
    s_ev[s_ev_n].write = write ? 1 : 0;
    s_ev_n++;
}

static uint32_t tr(uint32_t addr)
{
    uint32_t v = 0xDEADBEEFu;
    probe_access_t r = probe_guard_read32(addr, &v);
    if (r != PROBE_ACCESS_OK) {
        ESP_LOGE(TAG, "guard refused read 0x%08x (%s)", (unsigned)addr, probe_guard_reason(r));
    }
    ev_push(addr, v, false);
    return v;
}

static void tw(uint32_t addr, uint32_t value)
{
    probe_access_t r = probe_guard_write32(addr, value);
    if (r != PROBE_ACCESS_OK) {
        ESP_LOGE(TAG, "guard refused write 0x%08x (%s)", (unsigned)addr, probe_guard_reason(r));
    }
    ev_push(addr, value, true);
}

// Untraced accessors, for the poll loop's inner spin once the trace budget is
// spent, and for the timing paths where a traced read's own cost would swamp
// the interval being measured. The guard still gates both — only the recording
// is skipped.
static uint32_t rq(uint32_t addr)
{
    uint32_t v = 0xDEADBEEFu;
    (void)probe_guard_read32(addr, &v);
    return v;
}

static void wq(uint32_t addr, uint32_t value)
{
    (void)probe_guard_write32(addr, value);
}

// ── Register naming, so a dumped trace reads as SPI and not as hex ───────────

const char *probe_spi_reg_name(uint32_t addr)
{
    struct { uint32_t a; const char *n; } map[] = {
        { SPI_CMD_REG(PROBE_SPI_IDX),          "CMD"          },
        { SPI_ADDR_REG(PROBE_SPI_IDX),         "ADDR"         },
        { SPI_CTRL_REG(PROBE_SPI_IDX),         "CTRL"         },
        { SPI_CLOCK_REG(PROBE_SPI_IDX),        "CLOCK"        },
        { SPI_USER_REG(PROBE_SPI_IDX),         "USER"         },
        { SPI_USER1_REG(PROBE_SPI_IDX),        "USER1"        },
        { SPI_USER2_REG(PROBE_SPI_IDX),        "USER2"        },
        { SPI_MS_DLEN_REG(PROBE_SPI_IDX),      "MS_DLEN"      },
        { SPI_MISC_REG(PROBE_SPI_IDX),         "MISC"         },
        { SPI_DMA_CONF_REG(PROBE_SPI_IDX),     "DMA_CONF"     },
        { SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX),  "DMA_INT_CLR"  },
        { SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX),  "DMA_INT_RAW"  },
        { SPI_DMA_INT_ST_REG(PROBE_SPI_IDX),   "DMA_INT_ST"   },
        { SPI_SLAVE_REG(PROBE_SPI_IDX),        "SLAVE"        },
        { SPI_CLK_GATE_REG(PROBE_SPI_IDX),     "CLK_GATE"     },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (map[i].a == addr) return map[i].n;
    }
    uint32_t w0 = SPI_W0_REG(PROBE_SPI_IDX);
    if (addr >= w0 && addr <= SPI_W15_REG(PROBE_SPI_IDX) && ((addr - w0) & 3u) == 0) {
        static char buf[8];
        snprintf(buf, sizeof(buf), "W%u", (unsigned)((addr - w0) / 4u));
        return buf;
    }
    return NULL;
}

// ── Config latch ─────────────────────────────────────────────────────────────
//
// On this chip the CONF-state registers do not take effect until CMD.UPDATE is
// set, and UPDATE self-clears when the peripheral has consumed them. That
// self-clear is itself a read-side behaviour an emulator has to reproduce, so
// it is traced rather than blind-delayed.

static void spi_apply_conf(void)
{
    tw(SPI_CMD_REG(PROBE_SPI_IDX), SPI_UPDATE);
    uint32_t start = esp_cpu_get_cycle_count();
    for (int i = 0; i < 16; i++) {
        uint32_t cmd = tr(SPI_CMD_REG(PROBE_SPI_IDX));
        if (!(cmd & SPI_UPDATE)) return;
        if (esp_cpu_get_cycle_count() - start > PROBE_POLL_TIMEOUT_CYCLES) break;
    }
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_UPDATE) {
        if (esp_cpu_get_cycle_count() - start > PROBE_POLL_TIMEOUT_CYCLES) {
            ESP_LOGW(TAG, "CMD.UPDATE never self-cleared");
            return;
        }
    }
}

// ── Open / close ─────────────────────────────────────────────────────────────

esp_err_t probe_spi_open(uint32_t hz, uint8_t spi_mode)
{
    if (s_open) probe_spi_close();
    if (spi_mode > 3) return ESP_ERR_INVALID_ARG;

    // The IDF driver owns bus bring-up: GPIO matrix routing and the peripheral
    // clock gate. Deliberately NOT reimplemented here — that part is well
    // understood and a hand-rolled version would only risk a broken baseline.
    // No device is added, so nothing in IDF touches the transaction registers
    // below and the trace stays ours alone. CS and DC are driven as plain
    // GPIOs for the same reason.
    spi_bus_config_t bus = {
        .mosi_io_num     = PROBE_PIN_MOSI,
        .miso_io_num     = PROBE_PIN_MISO,
        .sclk_io_num     = PROBE_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = PROBE_SPI_MAX_XFER,
    };
    esp_err_t err = spi_bus_initialize(PROBE_SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already up
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PROBE_PIN_CS) | (1ULL << PROBE_PIN_DC),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PROBE_PIN_CS, 1);   // idle high
    gpio_set_level(PROBE_PIN_DC, 1);

    probe_spi_trace_clear();

    // Start the SPI master clock. NOT optional, and not obvious: confirmed on
    // hardware that CLK_GATE reads 0x00000000 after spi_bus_initialize() alone.
    // IDF sets these bits when a DEVICE is added, not when the bus is brought
    // up, and this code deliberately adds no device — so without this write the
    // peripheral has no master clock, CMD.USR latches high and never clears,
    // and every transfer times out. The first version of this file had exactly
    // that bug; the symptom is indistinguishable from a dead panel, which is
    // precisely the kind of false lead this tool exists to eliminate.
    //   bit0 CLK_EN, bit1 MST_CLK_ACTIVE, bit2 MST_CLK_SEL (1 = PLL_F80M,
    //   which is the 80 MHz the divider math above assumes).
    tw(SPI_CLK_GATE_REG(PROBE_SPI_IDX),
       SPI_CLK_EN | SPI_MST_CLK_ACTIVE | SPI_MST_CLK_SEL);

    // Master, full duplex, CPU-driven (no DMA), hardware CS lines all off.
    tw(SPI_SLAVE_REG(PROBE_SPI_IDX),    0);
    // Reads back 0x3 rather than 0 on this chip — the low DMA_CONF bits are
    // reset controls that re-assert themselves. Harmless, and worth knowing
    // before someone treats it as a failed write.
    tw(SPI_DMA_CONF_REG(PROBE_SPI_IDX), 0);

    // Clock divider: f = 80 MHz APB / ((pre+1) * (n+1)). Search for the
    // closest achievable rate at or below the request rather than rounding up
    // — overclocking a panel to hit a round number is how you get a display
    // that works on the bench and not in the field.
    const uint32_t apb = 80000000u;
    uint32_t best_pre = 0, best_n = 63;
    uint32_t best_err = 0xFFFFFFFFu;
    if (hz == 0) hz = 1000000u;
    for (uint32_t pre = 0; pre < 16; pre++) {
        for (uint32_t n = 1; n < 64; n++) {
            uint32_t f = apb / ((pre + 1) * (n + 1));
            if (f > hz) continue;
            uint32_t e = hz - f;
            if (e < best_err) { best_err = e; best_pre = pre; best_n = n; }
        }
    }
    uint32_t clock = ((best_pre & SPI_CLKDIV_PRE_V) << SPI_CLKDIV_PRE_S)
                   | ((best_n   & SPI_CLKCNT_N_V)   << SPI_CLKCNT_N_S)
                   | (((best_n + 1) / 2 - 1) << SPI_CLKCNT_H_S)
                   | ((best_n   & SPI_CLKCNT_L_V)   << SPI_CLKCNT_L_S);
    tw(SPI_CLOCK_REG(PROBE_SPI_IDX), clock);
    ESP_LOGI(TAG, "clock: requested %u Hz, configured %u Hz (pre=%u n=%u)",
             (unsigned)hz, (unsigned)(apb / ((best_pre + 1) * (best_n + 1))),
             (unsigned)best_pre, (unsigned)best_n);

    // Mode: CPOL is CK_IDLE_EDGE in MISC, CPHA is CK_OUT_EDGE in USER.
    uint32_t cpol = (spi_mode & 2) ? SPI_CK_IDLE_EDGE : 0;
    // All six hardware CS outputs disabled (bits 5:0) — CS is a GPIO here.
    tw(SPI_MISC_REG(PROBE_SPI_IDX), 0x3Fu | cpol);

    // Full duplex, data phase only: no command, address or dummy phases.
    uint32_t user = SPI_DOUTDIN | SPI_USR_MOSI | SPI_USR_MISO;
    if (spi_mode & 1) user |= (1u << 9);   // CK_OUT_EDGE
    tw(SPI_USER_REG(PROBE_SPI_IDX),  user);
    tw(SPI_USER1_REG(PROBE_SPI_IDX), 0);
    tw(SPI_USER2_REG(PROBE_SPI_IDX), 0);
    tw(SPI_CTRL_REG(PROBE_SPI_IDX),  0);

    spi_apply_conf();

    s_open = true;
    ESP_LOGI(TAG, "SPI2 open at register level (mode %u)", (unsigned)spi_mode);
    return ESP_OK;
}

void probe_spi_close(void)
{
    if (!s_open) return;
    spi_bus_free(PROBE_SPI_HOST);
    s_open = false;
}

bool probe_spi_is_open(void) { return s_open; }

// ── The transaction ──────────────────────────────────────────────────────────

esp_err_t probe_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len, int dc_level)
{
    if (!s_open)                     return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > PROBE_SPI_MAX_XFER) return ESP_ERR_INVALID_ARG;
    if (tx == NULL)                  return ESP_ERR_INVALID_ARG;

    probe_spi_trace_clear();

    gpio_set_level(PROBE_PIN_DC, dc_level ? 1 : 0);

    // Load TX into the W registers. Little-endian packing: byte 0 goes in the
    // low 8 bits of W0, which is the order the peripheral shifts out.
    uint32_t words[PROBE_SPI_MAX_XFER / 4];
    memset(words, 0, sizeof(words));
    memcpy(words, tx, len);
    size_t nwords = (len + 3) / 4;
    for (size_t i = 0; i < nwords; i++) {
        tw(SPI_W0_REG(PROBE_SPI_IDX) + i * 4, words[i]);
    }

    tw(SPI_MS_DLEN_REG(PROBE_SPI_IDX), (uint32_t)(len * 8 - 1) & SPI_MS_DATA_BITLEN_V);
    spi_apply_conf();

    // Clear a stale TRANS_DONE, then confirm it actually cleared — "write 1 to
    // clear" is an assumption worth checking once rather than trusting.
    tw(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);
    tr(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));

    memset(&s_timing, 0, sizeof(s_timing));

    gpio_set_level(PROBE_PIN_CS, 0);

    // ── the moment under study ───────────────────────────────────────────────
    s_timing.cyc_usr_set = esp_cpu_get_cycle_count();
    tw(SPI_CMD_REG(PROBE_SPI_IDX), SPI_USR);

    // Poll CMD.USR and DMA_INT_RAW together, recording both, so the trace shows
    // which of the two changes first and how far apart. An emulator that clears
    // USR and sets TRANS_DONE in the same instant will pass a test that only
    // looks at one of them and hang firmware that waits on the other.
    uint32_t polls = 0;
    while (1) {
        uint32_t cmd, raw;
        // Only the first few iterations are traced. A stuck transfer polls tens
        // of thousands of times, and tracing all of them buries the interesting
        // dozen writes under hundreds of identical lines — measured: a single
        // timed-out transfer filled the entire 256-event budget with the same
        // two reads repeating. The terminal state is recorded explicitly after
        // the loop, so nothing meaningful is lost by capping this.
        if (polls < PROBE_POLL_TRACE_LIMIT && s_ev_n + 4 < PROBE_SPI_MAX_EVENTS) {
            cmd = tr(SPI_CMD_REG(PROBE_SPI_IDX));
            raw = tr(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));
        } else {
            cmd = rq(SPI_CMD_REG(PROBE_SPI_IDX));
            raw = rq(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));
        }
        polls++;

        if (!s_timing.trans_done_seen && (raw & SPI_TRANS_DONE_INT_RAW)) {
            s_timing.trans_done_seen = true;
            s_timing.cyc_trans_done  = esp_cpu_get_cycle_count();
        }
        if (!(cmd & SPI_USR)) {
            s_timing.usr_cleared      = true;
            s_timing.cyc_usr_clear    = esp_cpu_get_cycle_count();
            s_timing.polls_until_clear = polls;
            break;
        }
        if (esp_cpu_get_cycle_count() - s_timing.cyc_usr_set > PROBE_POLL_TIMEOUT_CYCLES) {
            s_timing.polls_until_clear = polls;
            ESP_LOGW(TAG, "CMD.USR still set after %u polls — transfer stuck",
                     (unsigned)polls);
            break;
        }
    }

    // Terminal state, always traced even when the poll loop above went quiet.
    // On a healthy transfer this shows USR clear and TRANS_DONE set; on a stuck
    // one it shows exactly what the peripheral settled at, which is the single
    // most useful line in a failed capture.
    //
    // The TRANS_DONE read here doubles as its own question: does the bit latch
    // until acknowledged, or is it already gone? That decides whether an
    // emulator needs a sticky bit, and a write log can never answer it.
    tr(SPI_CMD_REG(PROBE_SPI_IDX));
    uint32_t raw_final = tr(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX));
    if (!s_timing.trans_done_seen && (raw_final & SPI_TRANS_DONE_INT_RAW)) {
        s_timing.trans_done_seen = true;
        s_timing.cyc_trans_done  = esp_cpu_get_cycle_count();
    }

    gpio_set_level(PROBE_PIN_CS, 1);

    // MISO readback out of the same W registers.
    if (rx) {
        uint32_t rw[PROBE_SPI_MAX_XFER / 4];
        memset(rw, 0, sizeof(rw));
        for (size_t i = 0; i < nwords; i++) {
            rw[i] = tr(SPI_W0_REG(PROBE_SPI_IDX) + i * 4);
        }
        memcpy(rx, rw, len);
    }

    tw(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);

    return s_timing.usr_cleared ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ── Async start, for interrupt characterisation ──────────────────────────────

esp_err_t probe_spi_start_async(const uint8_t *tx, size_t len, int dc_level,
                                uint32_t *cyc_usr_set)
{
    if (!s_open)                              return ESP_ERR_INVALID_STATE;
    if (tx == NULL || cyc_usr_set == NULL)    return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > PROBE_SPI_MAX_XFER) return ESP_ERR_INVALID_ARG;

    uint32_t words[PROBE_SPI_MAX_XFER / 4];
    memset(words, 0, sizeof(words));
    memcpy(words, tx, len);
    size_t nwords = (len + 3) / 4;

    gpio_set_level(PROBE_PIN_DC, dc_level ? 1 : 0);
    for (size_t i = 0; i < nwords; i++) {
        wq(SPI_W0_REG(PROBE_SPI_IDX) + i * 4, words[i]);
    }
    wq(SPI_MS_DLEN_REG(PROBE_SPI_IDX), (uint32_t)(len * 8 - 1) & SPI_MS_DATA_BITLEN_V);

    // Bounded. An unbounded spin here would outlast the task watchdog and take
    // the whole probe down rather than reporting a stuck peripheral.
    uint32_t tu = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_UPDATE);
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_UPDATE) {
        if (esp_cpu_get_cycle_count() - tu > PROBE_POLL_TIMEOUT_CYCLES) break;
    }

    gpio_set_level(PROBE_PIN_CS, 0);

    // Deliberately the last thing before the write, and sampled with interrupts
    // already armed by the caller: every latency figure is (ISR entry - this).
    *cyc_usr_set = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_USR);
    return ESP_OK;
}

void probe_spi_finish_async(void)
{
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_USR) {
        if (esp_cpu_get_cycle_count() - t0 > PROBE_POLL_TIMEOUT_CYCLES) break;
    }
    gpio_set_level(PROBE_PIN_CS, 1);
}

// ── High-resolution timing ───────────────────────────────────────────────────
//
// See the header for why this exists rather than reading the numbers off a
// trace. Everything here is untraced on purpose: a traced read costs several
// hundred cycles, which is the same order as the interval being measured, so
// recording the measurement would destroy it.

// One transfer, polling a single register. `watch_cmd` selects which:
// true  -> spin on CMD.USR until it clears
// false -> spin on DMA_INT_RAW until TRANS_DONE sets
// Returns cycles from the CMD.USR write to that event, or 0 on timeout.
static uint32_t one_shot(const uint32_t *words, size_t nwords, size_t len,
                         bool watch_cmd, bool *timed_out)
{
    for (size_t i = 0; i < nwords; i++) {
        wq(SPI_W0_REG(PROBE_SPI_IDX) + i * 4, words[i]);
    }
    wq(SPI_MS_DLEN_REG(PROBE_SPI_IDX), (uint32_t)(len * 8 - 1) & SPI_MS_DATA_BITLEN_V);

    uint32_t tu = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_UPDATE);
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_UPDATE) {
        if (esp_cpu_get_cycle_count() - tu > PROBE_POLL_TIMEOUT_CYCLES) break;
    }

    wq(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);

    gpio_set_level(PROBE_PIN_CS, 0);

    uint32_t t0 = esp_cpu_get_cycle_count();
    wq(SPI_CMD_REG(PROBE_SPI_IDX), SPI_USR);

    uint32_t t1 = 0;
    *timed_out = false;
    if (watch_cmd) {
        while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_USR) {
            if (esp_cpu_get_cycle_count() - t0 > PROBE_POLL_TIMEOUT_CYCLES) {
                *timed_out = true; break;
            }
        }
    } else {
        while (!(rq(SPI_DMA_INT_RAW_REG(PROBE_SPI_IDX)) & SPI_TRANS_DONE_INT_RAW)) {
            if (esp_cpu_get_cycle_count() - t0 > PROBE_POLL_TIMEOUT_CYCLES) {
                *timed_out = true; break;
            }
        }
    }
    t1 = esp_cpu_get_cycle_count();

    // Let the transfer finish before dropping CS, even when watching TRANS_DONE.
    while (rq(SPI_CMD_REG(PROBE_SPI_IDX)) & SPI_USR) {
        if (esp_cpu_get_cycle_count() - t0 > PROBE_POLL_TIMEOUT_CYCLES) break;
    }
    gpio_set_level(PROBE_PIN_CS, 1);
    wq(SPI_DMA_INT_CLR_REG(PROBE_SPI_IDX), SPI_TRANS_DONE_INT_CLR);

    return *timed_out ? 0 : (t1 - t0);
}

esp_err_t probe_spi_timing_probe(const uint8_t *tx, size_t len, int dc_level,
                                 uint32_t iterations, probe_spi_timing_stats_t *out)
{
    if (!s_open)                              return ESP_ERR_INVALID_STATE;
    if (tx == NULL || out == NULL)            return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > PROBE_SPI_MAX_XFER) return ESP_ERR_INVALID_ARG;
    if (iterations == 0 || iterations > 1000) return ESP_ERR_INVALID_ARG;

    uint32_t words[PROBE_SPI_MAX_XFER / 4];
    memset(words, 0, sizeof(words));
    memcpy(words, tx, len);
    size_t nwords = (len + 3) / 4;

    memset(out, 0, sizeof(*out));
    out->iterations = iterations;
    out->usr_min = out->td_min = 0xFFFFFFFFu;

    gpio_set_level(PROBE_PIN_DC, dc_level ? 1 : 0);

    uint64_t usr_sum = 0, td_sum = 0;
    uint32_t usr_n = 0, td_n = 0;

    for (uint32_t i = 0; i < iterations; i++) {
        bool to = false;
        uint32_t c = one_shot(words, nwords, len, true, &to);
        if (to) out->usr_fail++;
        else {
            if (c < out->usr_min) out->usr_min = c;
            if (c > out->usr_max) out->usr_max = c;
            usr_sum += c; usr_n++;
        }

        c = one_shot(words, nwords, len, false, &to);
        if (to) out->td_fail++;
        else {
            if (c < out->td_min) out->td_min = c;
            if (c > out->td_max) out->td_max = c;
            td_sum += c; td_n++;
        }
    }

    out->usr_mean = usr_n ? (uint32_t)(usr_sum / usr_n) : 0;
    out->td_mean  = td_n  ? (uint32_t)(td_sum  / td_n)  : 0;
    if (out->usr_min == 0xFFFFFFFFu) out->usr_min = 0;
    if (out->td_min  == 0xFFFFFFFFu) out->td_min  = 0;

    // The trace no longer describes anything after this ran — say so rather
    // than leaving a stale trace that looks like it belongs to this call.
    probe_spi_trace_clear();
    return ESP_OK;
}

// ── Trace readout ────────────────────────────────────────────────────────────

void   probe_spi_trace_clear(void) { s_ev_n = 0; s_ev_overflow = false; }
size_t probe_spi_trace_count(void) { return s_ev_n; }

const probe_spi_ev_t *probe_spi_trace_at(size_t i)
{
    return i < s_ev_n ? &s_ev[i] : NULL;
}

const probe_spi_timing_t *probe_spi_last_timing(void) { return &s_timing; }
