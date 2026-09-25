// purr_probe_guard.c — MMIO access gate. See purr_probe_guard.h for the model.
//
// There is deliberately NO compile-time escape hatch anywhere in this file.
// No #if, no Kconfig symbol, no "unsafe mode" flag unseals the deny list. If a
// future need for eFuse *reads* appears, get them from the host with
// `espefuse.py summary` (which cannot burn without an explicit burn subcommand)
// or from ordinary firmware via esp_efuse_read_field_blob() — not by adding a
// switch here. A gate with a documented bypass is a gate that gets bypassed at
// 2am by whoever is chasing a bug.

#include "purr_probe_guard.h"

#include "soc/soc.h"
#include "esp_log.h"

static const char *TAG = "probe_guard";

// ── Deny list ────────────────────────────────────────────────────────────────
//
// Checked first, wins unconditionally, applies to reads AND writes.
//
// Reads are denied too, not just writes. Not because a read can burn anything
// — it cannot — but because the cheapest correct rule to state, audit and trust
// is "the probe cannot address this block at all." A gate that permits reads
// has to get the read/write split right on every future edit; this one does
// not have a split to get wrong.

static const probe_window_t s_deny[] = {
    { DR_REG_EFUSE_BASE, 0x1000, "EFUSE",
      "one-way burns: flash encryption, secure boot, JTAG/download disable" },

    { DR_REG_SENSITIVE_BASE, 0x1000, "SENSITIVE",
      "secure boot and flash-encryption permission control" },

    { DR_REG_HMAC_BASE, 0x1000, "HMAC",
      "derives keys from eFuse key blocks" },

    { DR_REG_DIGITAL_SIGNATURE_BASE, 0x1000, "DS",
      "digital signature peripheral, consumes eFuse keys" },

    // SPI0/SPI1 are the FLASH controller, not general-purpose SPI. The
    // XTS-AES flash-encryption block lives inside this address space, and a
    // stray write here can corrupt the flash the probe is running from.
    // SPI2 (the display/SD/LoRa bus this tool exists to trace) is a different
    // peripheral at a different base and is allowed below.
    { DR_REG_SPI0_BASE, 0x1000, "SPI0_MEM",
      "flash controller + XTS-AES flash encryption" },

    { DR_REG_SPI1_BASE, 0x1000, "SPI1_MEM",
      "flash controller" },

    // RTC_CNTL carries brownout, the RTC watchdogs, power-domain control and
    // — historically on this family — the eFuse programming voltage path.
    // Nothing the probe needs, everything a typo could ruin. RTCIO sits in the
    // next 0x400 slot and is denied with it: RTC-domain pad control is not
    // something to poke at by accident either.
    //
    // 0x800, NOT the 0x1000 this used to be. The old window swallowed the whole
    // 4 KB span, and the SAR ADC (SENS, 0x60008800) lives in it — a peripheral
    // an emulator has to model and that was therefore impossible to measure.
    // Each of these peripherals is a 0x100-byte register file in its own 0x400
    // slot (verified against rtc_cntl_reg.h / rtc_io_reg.h / sens_reg.h), so
    // 0x800 covers RTC_CNTL and RTCIO exactly and stops short of SENS.
    //
    // Narrowing this exposes NOTHING by itself. The allowlist is the gate; deny
    // is belt-and-braces over it. Everything in 0x60008800..0x60008FFF stayed
    // unreachable after this edit until SENS was deliberately added to the
    // allow table below — and 0x60008C00..0x60008FFF is unreachable still.
    { DR_REG_RTCCNTL_BASE, 0x800, "RTC_CNTL+RTCIO",
      "power domains, brownout, RTC watchdogs, eFuse programming voltage, RTC pads" },
};

// ── Allow list ───────────────────────────────────────────────────────────────
//
// Everything not named here is refused. Keep this list to peripherals the
// emulator actually has to model.

static const probe_window_t s_allow[] = {
    { DR_REG_SPI2_BASE,  0x1000, "SPI2",     "display / SD / LoRa bus (GPSPI2)" },
    { DR_REG_SPI3_BASE,  0x1000, "SPI3",     "GPSPI3" },
    { DR_REG_GPIO_BASE,  0x1000, "GPIO",     "pin level, direction, matrix" },
    { DR_REG_IO_MUX_BASE, 0x1000, "IO_MUX",  "pad function and drive strength" },
    { DR_REG_I2C_EXT_BASE, 0x1000, "I2C0",   "GT911 touch, BBQ20 keyboard" },
    { DR_REG_I2C1_EXT_BASE, 0x1000, "I2C1",  "" },
    { DR_REG_LEDC_BASE,  0x1000, "LEDC",     "display backlight PWM" },
    { DR_REG_TIMERGROUP0_BASE, 0x1000, "TIMG0", "" },
    { DR_REG_TIMERGROUP1_BASE, 0x1000, "TIMG1", "" },
    { DR_REG_SYSTIMER_BASE, 0x1000, "SYSTIMER", "" },
    { DR_REG_UART_BASE,  0x1000, "UART0",    "console" },
    { DR_REG_UART1_BASE, 0x1000, "UART1",    "GPS NMEA" },
    { DR_REG_UART2_BASE, 0x1000, "UART2",    "" },
    { DR_REG_SDMMC_BASE, 0x1000, "SDMMC",    "" },
    // Peripheral clock gating and resets. A bad write here can wedge the chip
    // until the next reset — recoverable by power cycle, unlike anything on the
    // deny list, and the emulator has to model these to boot anything at all.
    { DR_REG_SYSTEM_BASE, 0x1000, "SYSTEM",  "peripheral clock enable / reset" },
    // GDMA — the DMA engine SPI2 bulk transfers run through. Sits at
    // 0x6003F000, directly above HMAC (0x6003E000) and DS (0x6003D000) on the
    // deny list; adjacent but non-overlapping, and probe_guard_selftest()
    // re-proves that at every boot rather than trusting this comment.
    { DR_REG_GDMA_BASE,  0x1000, "GDMA",     "SPI2 bulk transfer DMA engine" },
    // SENS — the SAR ADC. Battery sense on GPIO4 for this board, and the
    // peripheral whose completion-polling behaviour an emulator has to get
    // right or firmware spins forever waiting on MEAS1_DONE.
    //
    // Its own 0x400 slot, which holds nothing else. The register file is only
    // 0x100 bytes; the rest of the slot is allowed so the probe can answer
    // whether SENS mirrors within its slot the way SPI2 does across its 4 KB
    // window — exactly the kind of question this tool exists to settle rather
    // than guess. Trim to 0x100 if the wider view is not wanted; nothing in
    // this slot is irreversible either way.
    { DR_REG_SENS_BASE,  0x400,  "SENS",     "SAR ADC (battery sense, GPIO4)" },
};

#define ALLOW_N (sizeof(s_allow) / sizeof(s_allow[0]))
#define DENY_N  (sizeof(s_deny)  / sizeof(s_deny[0]))

// Reads are capped so a fat-fingered count cannot stall the command loop for
// minutes holding a bus other tasks may want.
#define PROBE_MAX_READ_WORDS 256u

static bool s_armed = false;

// ── Window arithmetic ────────────────────────────────────────────────────────
//
// All of it in uint64_t. A window ending at 0xFFFFFFFF, or a caller-supplied
// addr + length that wraps past the top of the address space, must not be able
// to alias into a permitted range through 32-bit truncation.

static bool span_in_window(const probe_window_t *w, uint64_t lo, uint64_t hi_excl)
{
    uint64_t wlo = (uint64_t)w->base;
    uint64_t whi = wlo + (uint64_t)w->len;
    return lo >= wlo && hi_excl <= whi;
}

static bool span_hits_window(const probe_window_t *w, uint64_t lo, uint64_t hi_excl)
{
    uint64_t wlo = (uint64_t)w->base;
    uint64_t whi = wlo + (uint64_t)w->len;
    return lo < whi && wlo < hi_excl;   // non-empty intersection
}

// Pure classifier — no armed check, so probe_guard_selftest() can use it to
// prove properties before the gate is armed.
static probe_access_t classify(uint32_t addr, uint32_t words)
{
    if (words == 0 || words > PROBE_MAX_READ_WORDS) return PROBE_DENY_SPAN;
    if (addr & 0x3u)                                return PROBE_DENY_UNALIGNED;

    uint64_t lo      = (uint64_t)addr;
    uint64_t hi_excl = lo + (uint64_t)words * 4u;
    if (hi_excl > 0x100000000ull) return PROBE_DENY_SPAN;

    // Deny first, and on ANY overlap — not just full containment. A read that
    // starts in permitted space and runs into eFuse is refused whole.
    for (size_t i = 0; i < DENY_N; i++) {
        if (span_hits_window(&s_deny[i], lo, hi_excl)) return PROBE_DENY_FORBIDDEN;
    }

    // Then require the whole span to sit inside one single allow window.
    // Not "each word is in some window" — a span may not straddle two
    // adjacent windows, because adjacency is a fact about this chip's map
    // rather than an intention of this list.
    for (size_t i = 0; i < ALLOW_N; i++) {
        if (span_in_window(&s_allow[i], lo, hi_excl)) return PROBE_ACCESS_OK;
    }

    return PROBE_DENY_NOT_ALLOWED;
}

// ── Public checks ────────────────────────────────────────────────────────────

probe_access_t probe_guard_check_read(uint32_t addr, uint32_t words)
{
    if (!s_armed) return PROBE_DENY_NOT_ARMED;
    return classify(addr, words);
}

probe_access_t probe_guard_check_write(uint32_t addr)
{
    if (!s_armed) return PROBE_DENY_NOT_ARMED;
    return classify(addr, 1);
}

// ── The only sanctioned bus accessors ────────────────────────────────────────
//
// The volatile dereferences below are the only ones in the probe kernel that
// take a host-supplied address. Each re-validates immediately before the
// access; no caller is trusted to have checked.

probe_access_t probe_guard_read32(uint32_t addr, uint32_t *out)
{
    if (out == NULL) return PROBE_DENY_SPAN;
    probe_access_t r = probe_guard_check_read(addr, 1);
    if (r != PROBE_ACCESS_OK) return r;
    *out = *(volatile uint32_t *)(uintptr_t)addr;
    return PROBE_ACCESS_OK;
}

probe_access_t probe_guard_write32(uint32_t addr, uint32_t value)
{
    probe_access_t r = probe_guard_check_write(addr);
    if (r != PROBE_ACCESS_OK) return r;
    *(volatile uint32_t *)(uintptr_t)addr = value;
    return PROBE_ACCESS_OK;
}

// ── Introspection ────────────────────────────────────────────────────────────

size_t                probe_guard_allow_count(void)      { return ALLOW_N; }
const probe_window_t *probe_guard_allow_at(size_t i)     { return i < ALLOW_N ? &s_allow[i] : NULL; }
size_t                probe_guard_deny_count(void)       { return DENY_N; }
const probe_window_t *probe_guard_deny_at(size_t i)      { return i < DENY_N ? &s_deny[i] : NULL; }
bool                  probe_guard_is_armed(void)         { return s_armed; }

const char *probe_guard_reason(probe_access_t r)
{
    switch (r) {
        case PROBE_ACCESS_OK:        return "ok";
        case PROBE_DENY_FORBIDDEN:   return "forbidden-region";
        case PROBE_DENY_NOT_ALLOWED: return "not-allowed";
        case PROBE_DENY_UNALIGNED:   return "unaligned";
        case PROBE_DENY_SPAN:        return "bad-span";
        case PROBE_DENY_NOT_ARMED:   return "gate-not-armed";
        default:                     return "unknown";
    }
}

// ── Boot-time proof ──────────────────────────────────────────────────────────
//
// This is what makes the deny list load-bearing rather than decorative. It runs
// before the command loop accepts a single byte, and the gate stays sealed
// unless every assertion holds.

bool probe_guard_selftest(void)
{
    bool ok = true;

    // 1. No window is degenerate or wraps.
    for (size_t i = 0; i < DENY_N; i++) {
        if (s_deny[i].len == 0 ||
            (uint64_t)s_deny[i].base + s_deny[i].len > 0x100000000ull) {
            ESP_LOGE(TAG, "deny window %s is degenerate or wraps", s_deny[i].name);
            ok = false;
        }
    }
    for (size_t i = 0; i < ALLOW_N; i++) {
        if (s_allow[i].len == 0 ||
            (uint64_t)s_allow[i].base + s_allow[i].len > 0x100000000ull) {
            ESP_LOGE(TAG, "allow window %s is degenerate or wraps", s_allow[i].name);
            ok = false;
        }
    }

    // 2. THE load-bearing one: no allow window may touch any deny window.
    //    This is what stops a future careless bulk allow window from quietly
    //    re-exposing eFuse. classify() would still refuse such an access
    //    (deny is checked first), but an allowlist that overlaps a deny range
    //    means someone's mental model has already diverged from the code, and
    //    that is the moment to stop rather than the next edit.
    for (size_t a = 0; a < ALLOW_N; a++) {
        uint64_t lo = (uint64_t)s_allow[a].base;
        uint64_t hi = lo + s_allow[a].len;
        for (size_t d = 0; d < DENY_N; d++) {
            if (span_hits_window(&s_deny[d], lo, hi)) {
                ESP_LOGE(TAG, "allow window %s overlaps deny window %s — refusing to arm",
                         s_allow[a].name, s_deny[d].name);
                ok = false;
            }
        }
    }

    // 3. Behavioural check on the deny list, at three points per window:
    //    first word, last word, and midpoint. Proves the classifier actually
    //    refuses them rather than merely that the table says it should.
    for (size_t d = 0; d < DENY_N; d++) {
        const uint32_t probes[3] = {
            s_deny[d].base,
            s_deny[d].base + s_deny[d].len - 4u,
            (s_deny[d].base + (s_deny[d].len / 2u)) & ~3u,
        };
        for (int p = 0; p < 3; p++) {
            if (classify(probes[p], 1) != PROBE_DENY_FORBIDDEN) {
                ESP_LOGE(TAG, "deny window %s not refused at 0x%08x — refusing to arm",
                         s_deny[d].name, (unsigned)probes[p]);
                ok = false;
            }
        }
        // A span that starts just below the window and runs into it must also
        // be refused, not truncated.
        if (classify((s_deny[d].base - 16u) & ~3u, 8) != PROBE_DENY_FORBIDDEN) {
            ESP_LOGE(TAG, "span running into deny window %s not refused — refusing to arm",
                     s_deny[d].name);
            ok = false;
        }
    }

    if (ok) {
        s_armed = true;
        ESP_LOGI(TAG, "gate armed: %u allow windows, %u deny windows",
                 (unsigned)ALLOW_N, (unsigned)DENY_N);
        ESP_LOGI(TAG, "eFuse 0x%08x..0x%08x is unreachable for read and write",
                 (unsigned)DR_REG_EFUSE_BASE,
                 (unsigned)(DR_REG_EFUSE_BASE + 0x1000 - 1));
    } else {
        s_armed = false;
        ESP_LOGE(TAG, "SELFTEST FAILED — gate sealed, probe will accept no accesses");
    }
    return ok;
}
