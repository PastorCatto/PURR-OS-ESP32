// purr_probe_cmd.c — host-facing command loop. See PROTOCOL.md in this
// directory for the wire format and the full command reference.
//
// Every protocol line starts with '%'. That single character is what makes the
// channel usable: ESP_LOG output shares this USB endpoint, and a host parser
// that drops any line without the sentinel cannot be derailed by a driver
// warning arriving mid-response. Each command emits zero or more '%d' data
// lines and then exactly one '%ok' or '%err <reason>' terminator, so a host
// always knows when a reply is complete without guessing at timeouts.

#include "purr_probe_cmd.h"
#include "purr_probe_guard.h"
#include "purr_probe_spi.h"
#include "purr_probe_int.h"
#include "purr_probe_dma.h"
#include "purr_kernel.h"
#include "soc/spi_reg.h"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "soc/soc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   // strcasecmp, for `snap <window>` name matching
#include <ctype.h>

static const char *TAG = "probe_cmd";

#define CMD_LINE_MAX   256
#define CMD_ARGV_MAX   16
#define PROBE_PROTO_VERSION "1"

// ── Output helpers ───────────────────────────────────────────────────────────

static void p_ok(void)                  { printf("%%ok\n");  fflush(stdout); }
static void p_err(const char *reason)   { printf("%%err %s\n", reason); fflush(stdout); }

static void p_data(const char *fmt, ...)
{
    va_list ap;
    printf("%%d ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

// ── Parsing ──────────────────────────────────────────────────────────────────

static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || (end && *end != '\0')) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_dec(const char *s, long *out)
{
    if (s == NULL || *s == '\0') return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) return false;
    *out = v;
    return true;
}

// ── Commands ─────────────────────────────────────────────────────────────────

static void cmd_id(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const esp_app_desc_t *app = esp_app_get_description();

    p_data("proto %s", PROBE_PROTO_VERSION);
    p_data("kernel purros-probe %s", PURR_KERNEL_VERSION);
    p_data("chip model=%d cores=%d rev=%d features=0x%x",
           (int)ci.model, (int)ci.cores, (int)ci.revision, (unsigned)ci.features);
    p_data("idf %s", IDF_VER);
    if (app) {
        p_data("app %s %s %s %s", app->project_name, app->version, app->date, app->time);
        // The ELF hash pins a trace to the exact binary that produced it.
        char hash[65];
        for (int i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", app->app_elf_sha256[i]);
        hash[64] = '\0';
        p_data("elf_sha256 %s", hash);
    }
    // Needed to convert every cycle count in a trace into wall time.
    p_data("cpu_hz %u", (unsigned)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u));
    p_data("heap_internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    p_data("heap_spiram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    p_data("reset_reason %d", (int)esp_reset_reason());
    p_ok();
}

static void cmd_regions(void)
{
    p_data("armed %d", probe_guard_is_armed() ? 1 : 0);
    for (size_t i = 0; i < probe_guard_deny_count(); i++) {
        const probe_window_t *w = probe_guard_deny_at(i);
        p_data("deny %08x %08x %s %s", (unsigned)w->base,
               (unsigned)(w->base + w->len - 1), w->name, w->note ? w->note : "");
    }
    for (size_t i = 0; i < probe_guard_allow_count(); i++) {
        const probe_window_t *w = probe_guard_allow_at(i);
        p_data("allow %08x %08x %s %s", (unsigned)w->base,
               (unsigned)(w->base + w->len - 1), w->name, w->note ? w->note : "");
    }
    p_ok();
}

// Live proof that the eFuse block is unreachable, rather than a claim in a
// comment. Runs the real classifier against real eFuse addresses; the bus is
// never touched because every one of them is refused.
static void cmd_guardtest(void)
{
    const uint32_t efuse_lo = DR_REG_EFUSE_BASE;
    const uint32_t efuse_hi = DR_REG_EFUSE_BASE + 0x1000 - 4;
    const uint32_t efuse_mid = DR_REG_EFUSE_BASE + 0x800;
    const uint32_t probes[] = { efuse_lo, efuse_mid, efuse_hi };
    bool all_refused = true;

    for (int i = 0; i < 3; i++) {
        uint32_t sink = 0;
        probe_access_t rr = probe_guard_read32(probes[i], &sink);
        probe_access_t wr = probe_guard_check_write(probes[i]);
        if (rr == PROBE_ACCESS_OK || wr == PROBE_ACCESS_OK) all_refused = false;
        p_data("efuse %08x read=%s write=%s", (unsigned)probes[i],
               probe_guard_reason(rr), probe_guard_reason(wr));
    }

    // And the same for the other irreversible/security blocks.
    for (size_t i = 0; i < probe_guard_deny_count(); i++) {
        const probe_window_t *w = probe_guard_deny_at(i);
        probe_access_t wr = probe_guard_check_write(w->base);
        if (wr == PROBE_ACCESS_OK) all_refused = false;
        p_data("denywin %s %08x write=%s", w->name, (unsigned)w->base,
               probe_guard_reason(wr));
    }

    p_data("all_refused %d", all_refused ? 1 : 0);
    if (all_refused) p_ok();
    else             p_err("guard-breach");
}

static void cmd_read(int argc, char **argv)
{
    uint32_t addr;
    if (argc < 2 || !parse_u32(argv[1], &addr)) { p_err("bad-addr"); return; }

    uint32_t count = 1;
    if (argc >= 3 && !parse_u32(argv[2], &count)) { p_err("bad-count"); return; }

    probe_access_t chk = probe_guard_check_read(addr, count);
    if (chk != PROBE_ACCESS_OK) { p_err(probe_guard_reason(chk)); return; }

    // Eight words per line keeps a line under the console's comfortable width
    // and makes a hand-read dump line up with a typical register map.
    for (uint32_t i = 0; i < count; i += 8) {
        char line[128];
        int n = snprintf(line, sizeof(line), "%08x", (unsigned)(addr + i * 4));
        for (uint32_t j = 0; j < 8 && (i + j) < count; j++) {
            uint32_t v = 0;
            probe_access_t r = probe_guard_read32(addr + (i + j) * 4, &v);
            if (r != PROBE_ACCESS_OK) { p_err(probe_guard_reason(r)); return; }
            n += snprintf(line + n, sizeof(line) - n, " %08x", (unsigned)v);
        }
        p_data("%s", line);
    }
    p_ok();
}

static void cmd_write(int argc, char **argv)
{
    uint32_t addr, val;
    if (argc < 3)                    { p_err("usage w <addr> <val>"); return; }
    if (!parse_u32(argv[1], &addr))  { p_err("bad-addr");  return; }
    if (!parse_u32(argv[2], &val))   { p_err("bad-value"); return; }

    probe_access_t r = probe_guard_write32(addr, val);
    if (r != PROBE_ACCESS_OK) {
        // Named explicitly in the log as well as the reply: a refused write to
        // a forbidden region is worth seeing in the console history, because it
        // means a host script tried something it should not have.
        ESP_LOGW(TAG, "refused write 0x%08x = 0x%08x (%s)",
                 (unsigned)addr, (unsigned)val, probe_guard_reason(r));
        p_err(probe_guard_reason(r));
        return;
    }
    // Read back so the host sees what actually stuck — write-only and
    // partially-writable bits are exactly the kind of thing an emulator gets
    // wrong, and this makes each one visible for free.
    uint32_t rb = 0;
    if (probe_guard_read32(addr, &rb) == PROBE_ACCESS_OK) {
        p_data("%08x wrote=%08x readback=%08x", (unsigned)addr, (unsigned)val, (unsigned)rb);
    }
    p_ok();
}

static void cmd_gpio_read(int argc, char **argv)
{
    long pin;
    if (argc < 2 || !parse_dec(argv[1], &pin) || pin < 0 || pin >= GPIO_NUM_MAX) {
        p_err("bad-pin"); return;
    }
    p_data("gpio %ld level=%d", pin, gpio_get_level((gpio_num_t)pin));
    p_ok();
}

static void cmd_gpio_write(int argc, char **argv)
{
    long pin, level;
    if (argc < 3) { p_err("usage gw <pin> <0|1>"); return; }
    if (!parse_dec(argv[1], &pin) || pin < 0 || pin >= GPIO_NUM_MAX) { p_err("bad-pin"); return; }
    if (!parse_dec(argv[2], &level) || (level != 0 && level != 1))   { p_err("bad-level"); return; }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) { p_err("gpio-config"); return; }
    gpio_set_level((gpio_num_t)pin, (uint32_t)level);
    p_ok();
}

static void cmd_spiopen(int argc, char **argv)
{
    uint32_t hz = 10000000u;
    long mode = 0;
    if (argc >= 2) {
        long h;
        if (!parse_dec(argv[1], &h) || h <= 0) { p_err("bad-hz"); return; }
        hz = (uint32_t)h;
    }
    if (argc >= 3 && (!parse_dec(argv[2], &mode) || mode < 0 || mode > 3)) {
        p_err("bad-mode"); return;
    }
    esp_err_t e = probe_spi_open(hz, (uint8_t)mode);
    if (e != ESP_OK) { p_err(esp_err_to_name(e)); return; }
    p_ok();
}

static void dump_trace(void)
{
    size_t n = probe_spi_trace_count();
    const probe_spi_ev_t *first = probe_spi_trace_at(0);
    uint32_t base_cycle = first ? first->cycle : 0;

    p_data("trace_events %u", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const probe_spi_ev_t *e = probe_spi_trace_at(i);
        const char *nm = probe_spi_reg_name(e->addr);
        // Cycles relative to the first event, not absolute: the raw counter
        // differs on every run, and two otherwise identical traces would
        // refuse to diff if it were printed as-is.
        p_data("ev %u %s %08x %s %08x +%u", (unsigned)i,
               e->write ? "W" : "R",
               (unsigned)e->addr,
               nm ? nm : "?",
               (unsigned)e->value,
               (unsigned)(e->cycle - base_cycle));
    }

    const probe_spi_timing_t *t = probe_spi_last_timing();
    p_data("timing usr_set=%u usr_clear=%u trans_done=%u",
           (unsigned)t->cyc_usr_set, (unsigned)t->cyc_usr_clear,
           (unsigned)t->cyc_trans_done);
    p_data("timing cycles_to_usr_clear=%d cycles_to_trans_done=%d polls=%u",
           t->usr_cleared     ? (int)(t->cyc_usr_clear  - t->cyc_usr_set) : -1,
           t->trans_done_seen ? (int)(t->cyc_trans_done - t->cyc_usr_set) : -1,
           (unsigned)t->polls_until_clear);
    p_data("timing usr_cleared=%d trans_done_seen=%d",
           t->usr_cleared ? 1 : 0, t->trans_done_seen ? 1 : 0);
}

static void cmd_spixfer(int argc, char **argv)
{
    if (!probe_spi_is_open()) { p_err("spi-not-open"); return; }
    if (argc < 2)             { p_err("usage x <hexbyte>... [dc=0|1]"); return; }

    uint8_t tx[PROBE_SPI_MAX_XFER];
    uint8_t rx[PROBE_SPI_MAX_XFER];
    size_t len = 0;
    int dc = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "dc=", 3) == 0) {
            dc = (argv[i][3] == '1') ? 1 : 0;
            continue;
        }
        uint32_t b;
        if (!parse_u32(argv[i], &b) || b > 0xFF) { p_err("bad-byte"); return; }
        if (len >= PROBE_SPI_MAX_XFER)           { p_err("too-long");  return; }
        tx[len++] = (uint8_t)b;
    }
    if (len == 0) { p_err("no-bytes"); return; }

    esp_err_t e = probe_spi_xfer(tx, rx, len, dc);

    char line[3 * PROBE_SPI_MAX_XFER + 8];
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%s%02x", i ? " " : "", rx[i]);
    }
    p_data("miso %s", line);

    dump_trace();

    if (e != ESP_OK) { p_err(e == ESP_ERR_TIMEOUT ? "usr-stuck" : esp_err_to_name(e)); return; }
    p_ok();
}

static void cmd_trace(void)
{
    dump_trace();
    p_ok();
}

// Dump one whole allow window. This is the emulator's register-file
// initializer: the values every register in a peripheral holds right now,
// which at boot (before anything has touched it) is its reset state.
//
// One window at a time, named, rather than a single "dump everything" command.
// Reading a peripheral whose clock is gated off does not reliably return zero
// on this chip — it can stall the bus — so which peripherals get touched stays
// a deliberate choice by the operator rather than something this command
// decides on their behalf by sweeping the whole map.
static void cmd_snap(int argc, char **argv)
{
    if (argc < 2) { p_err("usage snap <window> (see regions)"); return; }

    const probe_window_t *w = NULL;
    for (size_t i = 0; i < probe_guard_allow_count(); i++) {
        const probe_window_t *c = probe_guard_allow_at(i);
        if (strcasecmp(c->name, argv[1]) == 0) { w = c; break; }
    }
    if (w == NULL) { p_err("no-such-window"); return; }

    p_data("snap %s %08x %08x", w->name, (unsigned)w->base,
           (unsigned)(w->base + w->len - 1));

    uint32_t words = w->len / 4;
    for (uint32_t i = 0; i < words; i += 8) {
        char line[128];
        int n = snprintf(line, sizeof(line), "%08x", (unsigned)(w->base + i * 4));
        for (uint32_t j = 0; j < 8 && (i + j) < words; j++) {
            uint32_t v = 0;
            probe_access_t r = probe_guard_read32(w->base + (i + j) * 4, &v);
            if (r != PROBE_ACCESS_OK) { p_err(probe_guard_reason(r)); return; }
            n += snprintf(line + n, sizeof(line) - n, " %08x", (unsigned)v);
        }
        p_data("%s", line);
        // A 4 KB window is 128 lines over USB CDC. Well inside the watchdog's
        // 10 s, but fed anyway so the margin does not depend on host read speed.
        esp_task_wdt_reset();
    }
    p_ok();
}

static void cmd_spitiming(int argc, char **argv)
{
    if (!probe_spi_is_open()) { p_err("spi-not-open"); return; }
    if (argc < 2) { p_err("usage spitiming <hexbyte>... [n=<iters>] [dc=0|1]"); return; }

    uint8_t tx[PROBE_SPI_MAX_XFER];
    size_t len = 0;
    int dc = 0;
    uint32_t iters = 50;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "dc=", 3) == 0) { dc = (argv[i][3] == '1'); continue; }
        if (strncmp(argv[i], "n=", 2) == 0) {
            long v;
            if (!parse_dec(argv[i] + 2, &v) || v < 1 || v > 1000) { p_err("bad-iters"); return; }
            iters = (uint32_t)v;
            continue;
        }
        uint32_t b;
        if (!parse_u32(argv[i], &b) || b > 0xFF) { p_err("bad-byte"); return; }
        if (len >= PROBE_SPI_MAX_XFER)           { p_err("too-long");  return; }
        tx[len++] = (uint8_t)b;
    }
    if (len == 0) { p_err("no-bytes"); return; }

    probe_spi_timing_stats_t st;
    esp_err_t e = probe_spi_timing_probe(tx, len, dc, iters, &st);
    esp_task_wdt_reset();
    if (e != ESP_OK) { p_err(esp_err_to_name(e)); return; }

    p_data("timing_iters %u bytes %u", (unsigned)st.iterations, (unsigned)len);
    p_data("usr_clear min=%u mean=%u max=%u fail=%u",
           (unsigned)st.usr_min, (unsigned)st.usr_mean,
           (unsigned)st.usr_max, (unsigned)st.usr_fail);
    p_data("trans_done min=%u mean=%u max=%u fail=%u",
           (unsigned)st.td_min, (unsigned)st.td_mean,
           (unsigned)st.td_max, (unsigned)st.td_fail);

    // State the conclusion rather than leaving the reader to subtract two
    // numbers and guess whether the difference clears the noise. Each figure
    // is a minimum over independent runs of a tight single-register loop, so
    // the comparison is meaningful only when the gap exceeds the jitter.
    if (st.usr_fail == 0 && st.td_fail == 0) {
        int32_t gap = (int32_t)st.td_min - (int32_t)st.usr_min;
        uint32_t jitter = (st.usr_max - st.usr_min) + (st.td_max - st.td_min);
        const char *verdict =
            ((uint32_t)(gap < 0 ? -gap : gap) <= jitter) ? "indistinguishable"
            : (gap < 0 ? "trans_done-first" : "usr-clear-first");
        p_data("order gap=%d jitter=%u verdict=%s",
               (int)gap, (unsigned)jitter, verdict);
    }
    p_ok();
}

static void cmd_intmatrix(void)
{
    if (!probe_spi_is_open()) { p_err("spi-not-open"); return; }

    probe_int_row_t rows[PROBE_INT_MAX_ROWS];
    size_t n = 0;
    esp_err_t e = probe_int_matrix(rows, PROBE_INT_MAX_ROWS, &n);
    if (e != ESP_OK) { p_err(esp_err_to_name(e)); return; }

    for (size_t i = 0; i < n; i++) {
        p_data("row %u ena=%08x raw_before=%08x st_before=%08x "
               "raw_after=%08x st_after=%08x raw_preset=%d",
               (unsigned)i, (unsigned)rows[i].ena_written,
               (unsigned)rows[i].raw_before, (unsigned)rows[i].st_before,
               (unsigned)rows[i].raw_after,  (unsigned)rows[i].st_after,
               rows[i].raw_preset ? 1 : 0);
    }

    // Row 2 arms ENA on an ALREADY-SET raw bit. Whether ST rises there decides
    // if a late-arming reader can still see the event, or has lost it forever.
    if (n > 2) {
        bool late_ok = (rows[2].st_after & SPI_TRANS_DONE_INT_RAW) != 0;
        p_data("late_arm_sees_event %d", late_ok ? 1 : 0);
    }
    if (n > 3) {
        bool st_follows = (rows[3].st_after & SPI_TRANS_DONE_INT_RAW) == 0;
        p_data("clearing_raw_drops_st %d", st_follows ? 1 : 0);
    }
    p_ok();
}

static void cmd_intfire(int argc, char **argv)
{
    if (!probe_spi_is_open()) { p_err("spi-not-open"); return; }

    uint8_t tx[PROBE_SPI_MAX_XFER];
    size_t len = 0;
    int dc = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "dc=", 3) == 0) { dc = (argv[i][3] == '1'); continue; }
        uint32_t b;
        if (!parse_u32(argv[i], &b) || b > 0xFF) { p_err("bad-byte"); return; }
        if (len >= PROBE_SPI_MAX_XFER)           { p_err("too-long");  return; }
        tx[len++] = (uint8_t)b;
    }
    if (len == 0) { tx[0] = 0x00; len = 1; }

    probe_int_result_t r;
    esp_err_t e = probe_int_fire(tx, len, dc, &r);
    esp_task_wdt_reset();
    if (e != ESP_OK) { p_err(esp_err_to_name(e)); return; }

    p_data("fired %d count %u core %u", r.fired ? 1 : 0,
           (unsigned)r.count, (unsigned)r.core);
    if (r.fired) {
        p_data("latency_cycles %u", (unsigned)(r.cyc_isr - r.cyc_usr_set));
    }
    p_data("in_isr st=%08x raw=%08x",
           (unsigned)r.st_at_entry, (unsigned)r.raw_at_entry);
    // The ISR masked ENA rather than clearing RAW, so this shows whether RAW
    // is latched independently of the enable bit.
    p_data("after_mask raw=%08x st=%08x",
           (unsigned)r.raw_after, (unsigned)r.st_after);
    p_data("reentered %d", (r.count > 1) ? 1 : 0);
    p_ok();
}

// Deliberately hang so the watchdog has something to catch. The point is to
// turn "recovery is configured" into "recovery was observed" — a safety net
// nobody has ever seen fire is a safety net nobody should be relying on,
// especially with the board running unattended.
static void cmd_wedge(void)
{
    p_data("wedging deliberately; watchdog should panic and reboot in ~10s");
    p_data("the host will see the port drop, then re-enumerate");
    // No p_ok(). Nothing returns from here — that is the whole point.
    for (;;) {
        // Pointedly NOT calling esp_task_wdt_reset().
    }
}

static void cmd_dmaopen(void)
{
    if (!probe_spi_is_open()) { p_err("spi-not-open"); return; }
    esp_err_t e = probe_dma_open();
    if (e != ESP_OK) { p_err(esp_err_to_name(e)); return; }
    p_ok();
}

static void cmd_dmax(int argc, char **argv)
{
    if (!probe_dma_is_open()) { p_err("dma-not-open"); return; }

    long len = 256, pat = 0xA5, dc = 1;
    if (argc >= 2 && (!parse_dec(argv[1], &len) || len < 1 || len > PROBE_DMA_MAX_XFER)) {
        p_err("bad-len"); return;
    }
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "pat=", 4) == 0) {
            uint32_t v;
            if (!parse_u32(argv[i] + 4, &v) || v > 0xFF) { p_err("bad-pattern"); return; }
            pat = (long)v;
        } else if (strncmp(argv[i], "dc=", 3) == 0) {
            dc = (argv[i][3] == '1');
        }
    }

    probe_dma_result_t r;
    esp_err_t e = probe_dma_xfer((uint8_t)pat, (size_t)len, (int)dc, &r);
    esp_task_wdt_reset();
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) { p_err(esp_err_to_name(e)); return; }

    p_data("dma bytes=%u pattern=%02x", (unsigned)r.bytes, (unsigned)pat);
    p_data("desc_before dw0=%08x buf=%08x next=%08x size=%u length=%u eof=%u owner=%u",
           (unsigned)r.before.dw0, (unsigned)r.before.buffer, (unsigned)r.before.next,
           (unsigned)r.before.size, (unsigned)r.before.length,
           (unsigned)r.before.suc_eof, (unsigned)r.before.owner);
    p_data("desc_after  dw0=%08x buf=%08x next=%08x size=%u length=%u eof=%u owner=%u",
           (unsigned)r.after.dw0, (unsigned)r.after.buffer, (unsigned)r.after.next,
           (unsigned)r.after.size, (unsigned)r.after.length,
           (unsigned)r.after.suc_eof, (unsigned)r.after.owner);
    // The headline: whether firmware can poll `owner` to learn the buffer is
    // free again, or must rely on the interrupt instead.
    p_data("owner_cleared %d length_updated %d",
           r.owner_cleared ? 1 : 0, r.length_updated ? 1 : 0);
    p_data("dma_conf_after %08x int_raw_after %08x trans_done %d",
           (unsigned)r.spi_dma_conf_after, (unsigned)r.spi_dma_int_raw_after,
           r.trans_done_seen ? 1 : 0);
    p_data("cycles_to_done %u timed_out %d",
           (unsigned)r.cycles_to_done, r.timed_out ? 1 : 0);

    if (r.timed_out) { p_err("dma-stuck"); return; }
    p_ok();
}

static void cmd_dmarx(int argc, char **argv)
{
    if (!probe_dma_is_open()) { p_err("dma-not-open"); return; }

    long len = 64, pat = 0x00, dc = 1;
    if (argc >= 2 && (!parse_dec(argv[1], &len) || len < 1 || len > PROBE_DMA_MAX_XFER)) {
        p_err("bad-len"); return;
    }
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "pat=", 4) == 0) {
            uint32_t v;
            if (!parse_u32(argv[i] + 4, &v) || v > 0xFF) { p_err("bad-pattern"); return; }
            pat = (long)v;
        } else if (strncmp(argv[i], "dc=", 3) == 0) {
            dc = (argv[i][3] == '1');
        }
    }

    probe_dma_result_t r;
    probe_dma_rx_t rx;
    esp_err_t e = probe_dma_xfer_duplex((uint8_t)pat, (size_t)len, (int)dc, &r, &rx);
    esp_task_wdt_reset();
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) { p_err(esp_err_to_name(e)); return; }

    p_data("duplex bytes=%u pattern=%02x", (unsigned)r.bytes, (unsigned)pat);
    p_data("tx_desc_after owner=%u length=%u owner_cleared=%d",
           (unsigned)r.after.owner, (unsigned)r.after.length, r.owner_cleared ? 1 : 0);
    p_data("rx_desc_before dw0=%08x size=%u length=%u owner=%u",
           (unsigned)rx.rx_before.dw0, (unsigned)rx.rx_before.size,
           (unsigned)rx.rx_before.length, (unsigned)rx.rx_before.owner);
    p_data("rx_desc_after  dw0=%08x size=%u length=%u owner=%u eof=%u",
           (unsigned)rx.rx_after.dw0, (unsigned)rx.rx_after.size,
           (unsigned)rx.rx_after.length, (unsigned)rx.rx_after.owner,
           (unsigned)rx.rx_after.suc_eof);
    // The question this command exists for: does RX hand the buffer back, and
    // does it report how much arrived? TX does neither.
    p_data("rx_owner_cleared %d rx_length_updated %d rx_length %u",
           rx.rx_owner_cleared ? 1 : 0, rx.rx_length_updated ? 1 : 0,
           (unsigned)rx.rx_length);
    // Buffer pre-filled with 0xEE, so "unchanged" is distinguishable from a
    // genuine all-zero read — and MISO really does read 0x00 on this board.
    p_data("rx_buffer_changed %d first %02x %02x %02x %02x %02x %02x %02x %02x",
           rx.rx_buffer_changed ? 1 : 0,
           rx.rx_first[0], rx.rx_first[1], rx.rx_first[2], rx.rx_first[3],
           rx.rx_first[4], rx.rx_first[5], rx.rx_first[6], rx.rx_first[7]);
    p_data("dma_conf_after %08x int_raw_after %08x trans_done %d",
           (unsigned)r.spi_dma_conf_after, (unsigned)r.spi_dma_int_raw_after,
           r.trans_done_seen ? 1 : 0);
    p_data("cycles_to_done %u timed_out %d",
           (unsigned)r.cycles_to_done, r.timed_out ? 1 : 0);

    if (r.timed_out) { p_err("dma-stuck"); return; }
    p_ok();
}

// Decoded per-channel GDMA state for all five pairs.
//
// A raw `snap GDMA` gives the same bytes, but the question being asked is
// structural — which channel is bound to SPI2, did the engine ever receive an
// out_link write, and what does the hardware actually store when handed a
// descriptor pointer. That needs the register file split per channel and the
// key fields named, next to what IDF believes it allocated.
//
// PERI_SEL is the field that settles a false match. It is 6 bits with a RESET
// VALUE OF 0x3F, while SPI2's trigger id is 0 — so "unbound" and "bound to
// SPI2" are 0x3F and 0x00 respectively, not 0 and non-zero. Anything that
// defaults this field to 0 reports every unbound channel as SPI2.
static void cmd_gdma(void)
{
    if (!probe_dma_is_open()) { p_err("dma-not-open"); return; }

    int tx_id = -1, rx_id = -1;
    uint32_t tx_desc = 0, rx_desc = 0;
    probe_dma_channel_info(&tx_id, &rx_id, &tx_desc, &rx_desc);

    p_data("idf_alloc tx_ch=%d rx_ch=%d", tx_id, rx_id);
    p_data("desc_addr tx=%08x rx=%08x", (unsigned)tx_desc, (unsigned)rx_desc);
    p_data("spi2_trigger_id 0  peri_sel_reset 3f");

    // Channel pair n is CH0's layout plus n * 0xC0.
    const uint32_t base   = DR_REG_GDMA_BASE;
    const uint32_t stride = 0xC0;
    struct { uint32_t off; const char *name; } regs[] = {
        { 0x00, "IN_CONF0"    }, { 0x20, "IN_LINK"   }, { 0x24, "IN_STATE"  },
        { 0x28, "IN_EOF_DES"  }, { 0x30, "IN_DSCR"   }, { 0x48, "IN_PERI"   },
        { 0x60, "OUT_CONF0"   }, { 0x80, "OUT_LINK"  }, { 0x84, "OUT_STATE" },
        { 0x88, "OUT_EOF_DES" }, { 0x90, "OUT_DSCR"  }, { 0xA8, "OUT_PERI"  },
    };

    for (int ch = 0; ch < 5; ch++) {
        uint32_t chb = base + (uint32_t)ch * stride;
        char line[220];
        int n = snprintf(line, sizeof(line), "ch%d", ch);
        uint32_t in_peri = 0, out_peri = 0, out_link = 0, in_link = 0;
        for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
            uint32_t v = 0;
            probe_access_t r = probe_guard_read32(chb + regs[i].off, &v);
            if (r != PROBE_ACCESS_OK) { p_err(probe_guard_reason(r)); return; }
            n += snprintf(line + n, sizeof(line) - n, " %s=%08x", regs[i].name, (unsigned)v);
            if (regs[i].off == 0x48) in_peri  = v;
            if (regs[i].off == 0xA8) out_peri = v;
            if (regs[i].off == 0x80) out_link = v;
            if (regs[i].off == 0x20) in_link  = v;
        }
        p_data("%s", line);
        // Decoded verdict per channel, so the answer does not depend on the
        // reader remembering that 0x3F means "nothing attached".
        p_data("ch%d out_peri=%02x(%s) in_peri=%02x(%s) outlink_addr=%05x inlink_addr=%05x",
               ch,
               (unsigned)(out_peri & 0x3F),
               ((out_peri & 0x3F) == 0x3F) ? "unbound" : (((out_peri & 0x3F) == 0) ? "SPI2" : "other"),
               (unsigned)(in_peri & 0x3F),
               ((in_peri & 0x3F) == 0x3F) ? "unbound" : (((in_peri & 0x3F) == 0) ? "SPI2" : "other"),
               (unsigned)(out_link & 0xFFFFF),
               (unsigned)(in_link & 0xFFFFF));
    }
    p_ok();
}

static void cmd_log(int argc, char **argv)
{
    long lvl;
    if (argc < 2 || !parse_dec(argv[1], &lvl) || lvl < 0 || lvl > 5) {
        p_err("usage log <0..5>"); return;
    }
    esp_log_level_set("*", (esp_log_level_t)lvl);
    p_ok();
}

static void cmd_help(void)
{
    static const char *lines[] = {
        "ping                       liveness check",
        "id                         chip, IDF, app build, ELF sha256, heap",
        "regions                    allow/deny MMIO windows this build enforces",
        "guardtest                  live proof eFuse and friends are unreachable",
        "r <addr> [count]           read count 32-bit words (hex addr, max 256)",
        "w <addr> <value>           write one 32-bit word, reports readback",
        "g <pin>                    read GPIO level",
        "gw <pin> <0|1>             drive GPIO output",
        "spiopen [hz] [mode]        bring up SPI2 at register level",
        "x <hexbyte>... [dc=0|1]    SPI2 transaction, full register trace",
        "spitiming <hex>... [n=50]  tight single-register timing, resolves ordering",
        "trace                      re-dump the last SPI trace",
        "snap <window>              dump a whole peripheral window (see regions)",
        "intmatrix                  INT_RAW/ENA/ST truth table, no CPU interrupt",
        "intfire <hex>... [dc=]     real ISR: latency, core, re-entry, masking",
        "dmaopen                    connect a GDMA TX channel to SPI2",
        "dmax [len] [pat=] [dc=]    DMA TX transfer, reports descriptor writeback",
        "dmarx [len] [pat=] [dc=]   full-duplex DMA, reports RX descriptor writeback",
        "gdma                       decoded GDMA channel registers, all 5 pairs",
        "wedge                      hang on purpose to prove watchdog recovery",
        "log <0..5>                 set ESP_LOG level (0=none)",
        "reset                      restart the chip",
        NULL,
    };
    for (int i = 0; lines[i]; i++) p_data("%s", lines[i]);
    p_data("eFuse and flash-encryption registers are permanently unreachable");
    p_ok();
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

static void dispatch(int argc, char **argv)
{
    if (argc == 0) return;
    const char *c = argv[0];

    if      (!strcmp(c, "ping"))      p_ok();
    else if (!strcmp(c, "id"))        cmd_id();
    else if (!strcmp(c, "regions"))   cmd_regions();
    else if (!strcmp(c, "guardtest")) cmd_guardtest();
    else if (!strcmp(c, "r"))         cmd_read(argc, argv);
    else if (!strcmp(c, "w"))         cmd_write(argc, argv);
    else if (!strcmp(c, "g"))         cmd_gpio_read(argc, argv);
    else if (!strcmp(c, "gw"))        cmd_gpio_write(argc, argv);
    else if (!strcmp(c, "spiopen"))   cmd_spiopen(argc, argv);
    else if (!strcmp(c, "x"))         cmd_spixfer(argc, argv);
    else if (!strcmp(c, "trace"))     cmd_trace();
    else if (!strcmp(c, "snap"))      cmd_snap(argc, argv);
    else if (!strcmp(c, "spitiming")) cmd_spitiming(argc, argv);
    else if (!strcmp(c, "intmatrix")) cmd_intmatrix();
    else if (!strcmp(c, "intfire"))   cmd_intfire(argc, argv);
    else if (!strcmp(c, "wedge"))     cmd_wedge();
    else if (!strcmp(c, "dmaopen"))   cmd_dmaopen();
    else if (!strcmp(c, "dmax"))      cmd_dmax(argc, argv);
    else if (!strcmp(c, "dmarx"))     cmd_dmarx(argc, argv);
    else if (!strcmp(c, "gdma"))      cmd_gdma();
    else if (!strcmp(c, "log"))       cmd_log(argc, argv);
    else if (!strcmp(c, "help"))      cmd_help();
    else if (!strcmp(c, "reset"))     { p_ok(); vTaskDelay(pdMS_TO_TICKS(50)); esp_restart(); }
    else                              p_err("unknown-command");
}

static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

// ── The loop ─────────────────────────────────────────────────────────────────

void probe_cmd_loop(void)
{
    // Fail closed. If the gate never armed, the probe answers commands with a
    // refusal forever rather than falling back to unrestricted access. There is
    // no path from here to a permissive mode.
    if (!probe_guard_is_armed()) {
        ESP_LOGE(TAG, "access gate not armed — refusing to serve commands");
        while (1) {
            printf("%%err gate-not-armed\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    // Which console this build talks over is decided at compile time by
    // sdkconfig, and the two need different drivers. Gated on the Kconfig
    // symbol rather than a probe-specific flag so the two can never disagree.
    //
    // The UART variant exists for emulation. Under QEMU the whole console —
    // every log line and every protocol line — goes out one peripheral, so a
    // gap in the emulator's USB-Serial-JTAG model presents as firmware that
    // boots to total silence, indistinguishable from firmware that hung. UART0
    // is modelled well by QEMU and by every other tool, which turns "it stopped
    // somewhere" back into readable output.
    bool have_driver = false;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 1024;
    ucfg.tx_buffer_size = 2048;
    esp_err_t uerr = usb_serial_jtag_driver_install(&ucfg);
    if (uerr == ESP_OK || uerr == ESP_ERR_INVALID_STATE) {
        have_driver = true;
        usb_serial_jtag_vfs_use_driver();
        // CR and LF both end a line, and neither is echoed back — a host script
        // sending "\r\n" must not produce a spurious empty command.
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    } else {
        ESP_LOGW(TAG, "usb_serial_jtag driver install: %s — falling back to plain stdio",
                 esp_err_to_name(uerr));
    }
#else
    // UART0. The driver is installed for the same reason as above: a bounded
    // read, so the loop always reaches its watchdog feed while idle.
    esp_err_t uerr = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    if (uerr == ESP_OK || uerr == ESP_ERR_INVALID_STATE) {
        have_driver = true;
    } else {
        ESP_LOGW(TAG, "uart driver install: %s — falling back to plain stdio",
                 esp_err_to_name(uerr));
    }
#endif
    setvbuf(stdin,  NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Watch this task. A panic already reboots (PANIC_PRINT_REBOOT); this
    // catches the quieter failure — a write that leaves a peripheral spinning
    // or a bus stalled, where nothing faults but the loop stops answering.
    // Without it, a board driven remotely by a script goes silent and stays
    // that way until someone walks over to it, which is precisely the
    // situation this build is meant to avoid.
    esp_err_t werr = esp_task_wdt_add(NULL);
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog not attached: %s", esp_err_to_name(werr));
    }

    printf("%%hello purros-probe proto=%s kernel=%s\n", PROBE_PROTO_VERSION, PURR_KERNEL_VERSION);
    printf("%%hello efuse=blocked flash-enc=blocked secure-boot=blocked\n");
    printf("%%ready\n");
    fflush(stdout);

    char line[CMD_LINE_MAX];
    char *argv[CMD_ARGV_MAX];

    while (1) {
        size_t len = 0;
        // Hand-rolled line reader rather than fgets: fgets on a VFS that can
        // return early gives short reads that look like complete lines, which
        // silently truncates a long command into two bogus ones.
        while (1) {
            // A BOUNDED read, not fgetc.
            //
            // fgetc on this VFS blocks indefinitely when the host is silent, so
            // the watchdog feed below was unreachable while idle and the board
            // rebooted every 10 s on its own — confirmed live, reset_reason 6,
            // and invisible in testing because a command was always arriving
            // within the timeout. An idle probe waiting for its operator is the
            // NORMAL state of this tool; it has to survive it.
            //
            // 100 ms keeps the loop responsive while feeding the watchdog ~100x
            // per timeout period.
            int ch = -1;
            if (have_driver) {
                uint8_t b;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
                int n = usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(100));
#else
                int n = uart_read_bytes(UART_NUM_0, &b, 1, pdMS_TO_TICKS(100));
#endif
                if (n > 0) ch = b;
            } else {
                ch = fgetc(stdin);
            }
            esp_task_wdt_reset();
            if (ch < 0) { if (!have_driver) vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            if (ch == '\r') continue;
            if (ch == '\n') break;
            if (len < sizeof(line) - 1) line[len++] = (char)ch;
        }
        line[len] = '\0';
        if (len == 0) continue;

        int argc = tokenize(line, argv, CMD_ARGV_MAX);
        dispatch(argc, argv);
    }
}
