# PURR OS hardware probe — wire protocol

A T-Deck Plus firmware build that boots the PURR kernel, starts nothing else,
and hands the chip to a host over the native USB port. Built for the ESP32-S3
emulator work: it exists to supply the **read side** of hardware behaviour,
which an instrumented driver cannot capture.

Build: `python purrstrap/purrstrap.py build tdeck_plus_probe`
Kernel: [`kernel_tdp_probe_boot.c`](kernel_tdp_probe_boot.c)

---

## Safety: what this build cannot do

The probe takes arbitrary addresses from a host script, so the access gate is
the first thing to understand. It lives in one file,
[`purr_probe_guard.c`](purr_probe_guard.c), and nothing else in the probe
dereferences a host-supplied address.

**eFuse, flash encryption and secure boot registers are unreachable — reads and
writes both.** eFuse bits burn one way. A stray word into the programming
registers can permanently enable flash encryption or secure boot, or blow the
JTAG and download-mode disable bits, leaving a board that can never be
reflashed or debugged again. That is not a risk worth carrying in a tool whose
whole job is to poke registers from a script, so:

| Layer | What it does |
|---|---|
| **Allowlist-first** | An address outside every enumerated allow window is refused. Denial is the default for the whole 4 GB space, including peripherals nobody thought to name. |
| **Independent deny list** | Checked *before* the allowlist, wins unconditionally, covers eFuse, SENSITIVE, HMAC, DS, both flash controllers (SPI0/SPI1, which hold the XTS-AES flash-encryption block) and RTC_CNTL. |
| **Boot-time proof** | `probe_guard_selftest()` verifies no allow window intersects any deny window, and behaviourally probes each deny window at its base, midpoint and last word. |
| **Fail closed** | The gate stays sealed unless that proof passes. Until armed, *every* access returns `gate-not-armed`, and the command loop refuses to serve anything. |
| **No bypass** | There is no `#if`, Kconfig symbol, or runtime flag that unseals the deny list. A gate with a documented bypass is a gate that gets bypassed at 2am. |

Verify it on the running device with `guardtest`, which runs the real
classifier against real eFuse addresses and reports `all_refused 1`.

Two further consequences of the same reasoning: spans may not straddle two
adjacent allow windows (adjacency is a fact about the chip's memory map, not an
intention of the list), and a read that *starts* in permitted space but runs
into a deny window is refused whole rather than truncated.

If eFuse *values* are ever needed for emulator fidelity, get them from the host
with `espefuse.py summary` — which cannot burn without an explicit burn
subcommand — rather than by adding a switch here.

---

## Two builds: pick by where you are running it

Same firmware, one copy of the source. They differ in exactly one sdkconfig
choice — which peripheral the console uses — and
`kernel_tdeck_plus_probe_uart/` is six one-line wrappers around
`kernel_tdeck_plus_probe/`'s sources, so the two cannot drift.

| device | console | use for |
|---|---|---|
| `tdeck_plus_probe` | USB-Serial-JTAG | **real hardware** — the T-Deck's USB-C is the only cable |
| `tdeck_plus_probe_uart` | UART0, 115200 | **emulation** — reach for this first |

```sh
python purrstrap/purrstrap.py build tdeck_plus_probe_uart
# -> cattobaked/tdeck_plus_probe_uart/PURR_OS_tdeck_plus_probe_uart.bin
qemu-system-xtensa ... -serial stdio
```

**Why the UART build exists.** The entire console — every `ESP_LOG` line *and*
every `%` protocol line — leaves through one peripheral. On the USB build that
is USB-Serial-JTAG, which Espressif's QEMU fork ships as a stub: reads return
zero, writes are dropped. Firmware then boots to **complete silence**, which
looks exactly like firmware that hung in startup. Raising the log level does not
help, because nothing was reaching the console in the first place.

So when a probe build goes quiet under emulation, run the UART variant before
debugging the firmware. It separates *the firmware stopped* from *the console
model dropped it*, and only one of those is a firmware problem.

On real hardware the UART build needs UART0's pins (TX GPIO43 / RX GPIO44 on the
T-Deck, shared with the GPS header); its USB-C port stays silent.

---

## Transport

The T-Deck has no USB-serial bridge chip; its USB-C goes straight to the S3's
own controller, so the board enumerates as `/dev/ttyACM0` (`COMx` on Windows).
Baud rate is irrelevant — it's CDC, not a real UART.

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` for this device, so the console *is* that
port. Log output and command replies therefore share one endpoint.

**Every protocol line begins with `%`.** That single character is what makes the
channel reliable: a host parser that drops any line without the sentinel cannot
be derailed by a driver warning arriving mid-response. `log 0` silences ESP_LOG
entirely if you want a clean capture.

### Framing

Each command produces zero or more data lines and then **exactly one**
terminator, so a host always knows when a reply is complete without guessing at
timeouts:

```
%d <payload>      zero or more data lines
%ok               success terminator
%err <reason>     failure terminator, machine-readable token
```

On boot:

```
%hello purros-probe proto=1 kernel=1.0.0-dp9
%hello efuse=blocked flash-enc=blocked secure-boot=blocked
%ready
```

Refusal reasons are stable tokens: `forbidden-region`, `not-allowed`,
`unaligned`, `bad-span`, `gate-not-armed`, `unknown-command`, `bad-addr`,
`bad-value`, `bad-pin`, `spi-not-open`, `usr-stuck`.

---

## Commands

Addresses and values are hex (`0x` optional). Pin numbers and log levels are
decimal.

| Command | Meaning |
|---|---|
| `ping` | Liveness. Replies `%ok`. |
| `id` | Chip model/cores/revision, IDF version, app build, **ELF sha256**, CPU Hz, heap, reset reason. |
| `regions` | Every allow and deny window this build enforces, with names and rationale. |
| `guardtest` | Live proof the deny list holds. Ends `%ok` only if `all_refused 1`. |
| `r <addr> [count]` | Read `count` 32-bit words (max 256), eight per line. |
| `w <addr> <value>` | Write one word, then report the **readback** — write-only and partially-writable bits are exactly what an emulator gets wrong, and this makes each one visible for free. |
| `g <pin>` | Read GPIO level. |
| `gw <pin> <0\|1>` | Drive a GPIO output. |
| `spiopen [hz] [mode]` | Bring up SPI2 at register level. Defaults 10 MHz, mode 0. |
| `x <hexbyte>... [dc=0\|1]` | One SPI2 transaction. Emits MISO bytes **and the full register trace**. |
| `spitiming <hex>... [n=50] [dc=]` | Tight single-register timing over N iterations. Use this, not the trace, whenever the timing or the ordering of two events is the actual question. |
| `trace` | Re-dump the last SPI trace without re-running the transfer. |
| `snap <window>` | Dump a whole peripheral window (name from `regions`). **Only meaningful once the peripheral is clocked** — see "an unclocked peripheral reads as all-zeros" below. |
| `intmatrix` | `INT_RAW`/`INT_ENA`/`INT_ST` truth table. Enables no CPU interrupt, so it cannot wedge anything. |
| `intfire <hex>... [dc=]` | Install a real ISR, run a transfer: latency, core, re-entry count, and what masking does. |
| `wedge` | Hang on purpose, to prove the watchdog recovers. Verified: reboots in ~10 s with `reset_reason 6` (`ESP_RST_TASK_WDT`), and `probe_host.py` reconnects by itself. |
| `log <0..5>` | ESP_LOG level, 0 = silent. |
| `reset` | Restart the chip. |

`id`'s `elf_sha256` pins a captured trace to the exact binary that produced it.
Record it alongside any capture you intend to diff against an emulator.

---

## The SPI trace — what it is for

`spiopen` + `x` is the reason this build exists. The IDF driver brings up the
bus and pin matrix (that part is well understood, and a hand-rolled version
would only risk a broken baseline), but **the transaction itself runs through
raw register accesses**, every one of them recorded.

```
> spiopen 10000000 0
%ok
> x 04 00 00 00
%d miso 00 85 85 52
%d trace_events 23
%d ev 0 W 60024098 W0 00000004 +0
%d ev 1 W 6002401c MS_DLEN 0000001f +48
%d ev 2 W 60024000 CMD 00800000 +96
%d ev 3 R 60024000 CMD 00000000 +144
...
%d ev 9 R 60024000 CMD 00040000 +892
%d ev 10 R 6002403c DMA_INT_RAW 00000000 +940
%d ev 11 R 60024000 CMD 00000000 +988
%d ev 12 R 6002403c DMA_INT_RAW 00001000 +1036
%d timing usr_set=... usr_clear=... trans_done=...
%d timing cycles_to_usr_clear=904 cycles_to_trans_done=1012 polls=4
%d timing usr_cleared=1 trans_done_seen=1
%ok
```

Each event is `ev <index> <R|W> <addr> <regname> <value> +<cycles since first
event>`. Cycles are relative, not absolute — the raw counter differs every run,
and two otherwise identical traces would refuse to diff if it were printed
as-is. `id`'s `cpu_hz` converts them to wall time.

The specific unknowns this answers:

- **Does `CMD.USR` self-clear, or latch until acknowledged?** The poll loop
  records every `CMD_REG` read with its value, so you see the transition.
- **What does `DMA_INT_RAW` read mid-transfer?** Polled alongside `CMD_REG` on
  every iteration, deliberately — an emulator that clears `USR` and sets
  `TRANS_DONE` in the same instant passes a test that watches only one of them
  and hangs firmware that waits on the other. The trace shows which moves first
  and how far apart.
- **How long from `CMD.USR` to `TRANS_DONE`?** `cycles_to_trans_done`.
- **Does `TRANS_DONE` stay set until written-to-clear?** There's an extra read
  after `USR` clears; and the write-1-to-clear is followed by a readback rather
  than assumed.
- **Does `CMD.UPDATE` self-clear?** `spi_apply_conf()` polls it traced rather
  than blind-delaying.
- **What does MISO give with nothing driving it?** The `miso` line, read back
  out of the `W` registers.

`-1` for `cycles_to_usr_clear` or `cycles_to_trans_done` means it never
happened within the ~100 ms timeout — a finding, not a tuning problem.

Transfers are CPU-driven through `W0..W15`, so **64 bytes max**, no DMA. That's
deliberate: DMA descriptor chasing is a second unknown, and it belongs in its
own characterisation pass rather than tangled into this one.

---

## Confirmed on hardware

First run against a real T-Deck Plus (ESP32-S3 rev v0.2, 160 MHz, 8 MB PSRAM).
These are measurements, not predictions — reproduce them before trusting them
in an emulator, and record the `elf_sha256` of the build you measured with.

**`SPI_CLK_GATE_REG` (SPI2 + 0xE8) must be non-zero or nothing runs.** After
`spi_bus_initialize()` alone it reads `0x00000000`. IDF sets `CLK_EN`,
`MST_CLK_ACTIVE` and `MST_CLK_SEL` when a *device* is added, not when the bus
is brought up — so with no device attached the peripheral has no master clock,
`CMD.USR` latches high and never clears, and every transfer times out. The
first version of `probe_spi_open()` had exactly this bug. **This is the single
highest-value thing here for an emulator**: model that gate, or firmware that
configures SPI2 correctly in every other respect will hang forever on a bit
that looks like a clock-domain detail. `spiopen` now writes `0x7` and the
register is in the trace under the name `CLK_GATE`.

**`CMD.UPDATE` self-clears immediately.** Written `0x00800000`, it read back
`0x00000000` on the very first poll (~490 cycles later). No latch, no
acknowledge needed.

**`DMA_CONF` does not read back what you write.** Written `0x00000000`, reads
`0x00000003`. The low bits are reset controls that re-assert. An emulator
returning the written value will look correct and diverge from silicon.

**`CMD.USR` self-clears on completion**, and `TRANS_DONE_INT_RAW` (bit 12 of
`DMA_INT_RAW`) latches until written-to-clear via `DMA_INT_CLR`.

**`TRANS_DONE` and `USR` clear are simultaneous.** Measured with `spitiming`,
100 iterations per figure, tight single-register polling (10 MHz, mode 0):

| transfer | `USR` clear (min/mean/max) | `TRANS_DONE` (min/mean/max) | gap |
|---|---|---|---|
| 4 bytes | 1344 / 1363 / 2926 | 1343 / 1343 / 1393 | **−1 cycle** |
| 8 bytes | 1804 / 1814 / 2822 | 1803 / 1826 / 3385 | **−1 cycle** |

A one-cycle gap against jitter of 1600–2600 cycles is no gap at all. An
emulator may clear `USR` and set `TRANS_DONE` in the same instant. Take the
**min** as the real figure — the maxima are FreeRTOS tick interrupts landing
inside the measured window, not peripheral behaviour.

This **corrects an earlier reading of this same hardware.** The coarse `x`
trace put `TRANS_DONE` ~1400 cycles ahead of `USR` clear on the 8-byte
transfer, which looked like a real ordering. It was entirely a sampling
artifact: that loop reads `CMD_REG` ~480 cycles before `DMA_INT_RAW` within an
iteration and iterates every ~1400 cycles, so it cannot resolve two events one
cycle apart. The trace's own timing lines are fine for "did it complete and
roughly how fast"; **use `spitiming` for anything where the answer matters**,
and note it also shows the true transfer cost (~1344 cycles for 4 bytes, ~8.4 µs)
without the several-hundred-cycle-per-read tracing overhead the `x` figures
carry.

**An unclocked peripheral reads as all-zeros — it does not stall, and it does
not return its reset values.** From `captures/baseline-20260804-132148.txt`:
SPI2 snapshotted before `spiopen` was **0 of 1024 words non-zero**; after
`spiopen`, 112. The proof is `SPI_DATE_REG` at +0x0F0, a hardwired constant that
read `0x00000000` before and `0x02101190` after — a value that cannot be zero on
a readable peripheral. Compare GPIO (522/1024 non-zero) and IO_MUX (1024/1024),
which are clocked from boot.

Two consequences. For **captures**: enable a peripheral's clock before `snap`,
or you are recording zeros and calling them reset values. For the **emulator**:
decide deliberately what an access to a clock-gated peripheral returns, because
on real silicon it is 0 rather than a bus fault — firmware that probes for a
peripheral by reading its DATE/ID register will see 0 and can conclude the
peripheral is absent.

**SPI2's register file is 0x100 bytes, mirrored 16× across its 4 KB window.**
Verified exactly: every offset in `0x000..0xFFF` equals the offset at
`offset & 0xFF`, and the 112 changed words are precisely 7 distinct registers ×
16 mirrors. An emulator must mask SPI2 addresses to 8 bits; one that decodes the
full 12 bits will return 0 where hardware returns a live value.

### Interrupts

Measured with `intmatrix` (no CPU interrupt involved) and `intfire` (a real
handler). `ETS_SPI2_INTR_SOURCE` is documented level-triggered, and behaves so.

**`INT_ST == INT_RAW & INT_ENA`, purely combinational.** Not edge-latched, not
sampled at the moment of the event. The full truth table:

| | `ENA` | `RAW` before | `ST` before | action | `RAW` after | `ST` after |
|---|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | baseline | 0 | 0 |
| 1 | 0 | `0x1000` | 0 | transfer ran while masked | `0x1000` | 0 |
| 2 | `0x1000` | `0x1000` | 0 | **arm ENA late** | `0x1000` | **`0x1000`** |
| 3 | `0x1000` | `0x1000` | `0x1000` | write `INT_CLR` | 0 | 0 |

Consequences, each of which an emulator can get wrong independently:

- **RAW sets regardless of the mask** (row 1). Masking suppresses delivery, not
  recording.
- **No lost wakeup** (row 2). Arming `ENA` on an *already-set* `RAW` raises `ST`
  immediately, so firmware that arms after the hardware already finished still
  sees the event. An emulator that only raises `ST` on the RAW 0→1 edge will
  hang exactly that firmware, and only sometimes — whenever the arm loses the
  race.
- **Clearing `RAW` drops `ST`** (row 3), and masking `ENA` drops `ST` while
  leaving `RAW` set (`intfire`'s `after_mask raw=00001000 st=00000000`).

**The ISR re-enters exactly twice.** Deterministic — `count 2` on every run,
across transfer sizes — even though the handler masks `ENA` as its first
side-effecting act. The write to `ENA` does not deassert the line before the
CPU has already taken the interrupt a second time.

> **This is the first thing to check against an emulator's interrupt path.**
> A model that deasserts the line synchronously on the `ENA` write yields
> `count == 1`. Real hardware yields 2. Handlers written against silicon are
> re-entrant (or idempotent) by necessity; one written against a synchronous
> model may not be, and the divergence shows up as a doubled completion, a
> double-decremented semaphore, or a transaction dequeued twice.

**Latency ~440 cycles from the hardware event to ISR entry**, on **core 0**.
`intfire` reports latency from the `CMD.USR` write: 1782 cycles for 4 bytes,
1144 for 1 byte. Subtracting `spitiming`'s `TRANS_DONE` figure (1343 for
4 bytes) leaves ~439 cycles of CPU dispatch overhead. Run-to-run this moves
(2078 and 848 on a later capture) — the interrupt lands wherever the CPU
happens to be, so treat it as a few hundred cycles, not a constant.

#### Implementing this in an emulator

In dependency order, each item independently able to hang firmware:

1. `INT_ST` is **combinational**: recompute as `RAW & ENA` on every write to
   either register and on every event. Do not latch it, and do not compute it
   only on the RAW 0→1 edge.
2. `RAW` sets **regardless of `ENA`**. Masking suppresses delivery only.
3. `INT_CLR` writes clear `RAW`; `ST` must drop as a consequence of rule 1, not
   as a separate action.
4. Raise the CPU interrupt **whenever `ST != 0`**, level-style — not on the
   transition. This is what makes late arming work.
5. Keep the line asserted for a short window after `ENA` is cleared, or model
   the re-entry some other way. Real hardware enters the handler **twice**;
   a synchronous deassert gives one entry and silently diverges.
6. Dispatch latency of a few hundred cycles, not zero. Firmware that races the
   ISR against a polled flag behaves differently at zero latency.

### DMA

Measured with `dmaopen` + `dmax` at 64, 512 and 2048 bytes. A GDMA TX channel is
allocated and connected via IDF (`GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2)`);
the descriptor and the SPI-side DMA registers are driven raw.

**The TX descriptor is NOT written back.** `owner` stays 1 and `length` is
unchanged after completion, at every size:

```
desc_before dw0=c0800800 size=2048 length=2048 eof=1 owner=1
desc_after  dw0=c0800800 size=2048 length=2048 eof=1 owner=1
owner_cleared 0  length_updated 0
```

> **The trap.** RX descriptors on this family *do* get `owner` cleared, and it
> is easy to model TX the same way "for symmetry". Do not. Firmware cannot learn
> a TX buffer is reusable by polling `owner` — it must use `TRANS_DONE` or the
> interrupt. An emulator that clears TX `owner` will happily run firmware that
> polls it, and that firmware will hang on real hardware. The inverse is worse:
> firmware written against silicon, run on an emulator that never sets
> `TRANS_DONE`, waits forever with a descriptor that looks busy.

**RX descriptors ARE written back — TX and RX behave differently in the same
transfer.** Measured with `dmarx`, which arms both channels for one full-duplex
transaction, so this is not an artefact of comparing two separate runs:

```
tx_desc_after   owner=1 length=64            owner_cleared=0
rx_desc_before  dw0=80000040 size=64 length=0   owner=1
rx_desc_after   dw0=40040040 size=64 length=64  owner=0 eof=1
rx_owner_cleared 1  rx_length_updated 1  rx_length 64
```

On the RX side the engine clears `owner`, fills `length` with what actually
arrived, **and sets `suc_eof` itself** (it was written as 0). On the TX side, in
that same transfer, nothing changes.

> **This is the one to check against a display read that times out.** Because RX
> writeback works, polling the RX descriptor's `owner` *is* a valid way for
> firmware to learn a read completed — the opposite of TX. An emulator that
> models descriptor writeback once, symmetrically, gets exactly one of the two
> directions right. If it never clears RX `owner`, never fills `length`, or
> never sets `suc_eof`, a driver waiting on that descriptor waits forever, and
> the symptom is a read that hangs while writes look perfectly healthy.

`rx_buffer_changed 1` with `first 00 00 00 00 …`: the buffer is pre-filled with
`0xEE` before each run precisely so a genuine all-zero read is distinguishable
from the engine never writing at all — necessary here, because MISO really does
read `0x00` on this board.

`DMA_CONF` reads back `0x18200003` for full duplex — `DMA_TX_ENA` (bit 28),
`DMA_RX_ENA` (bit 27), `RX_EOF_EN` (bit 21), plus the usual self-asserting low
`0x3`. Full duplex costs no extra time: 64 bytes in 9,292 cycles against 9,442
for TX alone.

#### GDMA channel state (`gdma`)

Decoded per-channel registers for all five pairs, alongside what IDF believes it
allocated. Five channel pairs, `DR_REG_GDMA_BASE + n * 0xC0`.

**`PERI_SEL` resets to `0x3F`, and SPI2's trigger id is `0`.** Measured right
after `dmaopen`:

```
idf_alloc tx_ch=0 rx_ch=0
ch0  out_peri=00(SPI2)    in_peri=00(SPI2)
ch1  out_peri=3f(unbound) in_peri=3f(unbound)     ... and ch2, ch3, ch4 likewise
```

> **A model that zero-initialises `PERI_SEL` reports every unbound channel as
> bound to SPI2**, because "unbound" is `0x3F` here and not `0`. That produces a
> confident match on a channel nothing ever programmed — `found=1` with a null
> descriptor, which is exactly what a false positive looks like. `0x3F` is the
> reset value to model; `0` is a real binding.

`tx_ch=0` *and* `rx_ch=0` is also a genuine allocation rather than a default:
GDMA channels are **pairs**, each with an independent in (RX) and out (TX) half,
so both halves of pair 0 legitimately report id 0.

**Link registers hold only the low 20 bits; the EOF registers hold the full
address.** After a transfer with descriptors at `tx=3fca8ce4`, `rx=3fca9534`:

| register | value | holds |
|---|---|---|
| `OUT_LINK` | `008a8ce4` | low 20 bits (`a8ce4`) + control flags in 20–23 |
| `IN_LINK` | `011a9534` | low 20 bits (`a9534`) + control flags |
| `OUT_EOF_DES` | `3fca8ce4` | **full 32-bit descriptor address** |
| `IN_EOF_DES` | `3fca9534` | **full 32-bit descriptor address** |
| `OUT_STATE` / `IN_STATE` | `00028ce4` / `00029534` | low bits of the current descriptor |
| `OUT_DSCR` / `IN_DSCR` | `00000000` | zero once complete |

So comparing a full descriptor pointer against `OUT_LINK` never matches — the
upper 12 bits are not stored there at all. Compare against `OUT_EOF_DES_ADDR`,
or mask to 20 bits.

**`gdma_connect` programs `PERI_SEL`; `gdma_start` programs the link address.**
Before any transfer the bound channel already reads `out_peri=00` while
`outlink_addr=00000`. That is what separates *the driver never programmed the
engine* (link address still zero on a channel whose `PERI_SEL` is bound) from
*reading the wrong channel* (`PERI_SEL` is `0x3F` on the channel being watched).

**`TRANS_DONE` marks DMA completion too** — same bit 12 of `DMA_INT_RAW`, no
separate DMA-completion signal on the SPI side.

**`DMA_CONF` reads back `0x10000003`** after writing `SPI_DMA_TX_ENA` (bit 28):
the enable sticks, and the low `0x3` are the same self-asserting reset bits
noted above.

**DMA saturates the bus — no per-descriptor stall.**

| bytes | cycles | cycles/byte |
|---|---|---|
| 64 | 9,442 | 147.5 |
| 512 | 66,257 | 129.4 |
| 2048 | 262,900 | 128.4 |

At 10 MHz one byte is 8 bits ÷ 10 MHz = 0.8 µs = **exactly 128 cycles at
160 MHz**. The measured steady-state rate is 128.4, so the engine keeps the
shift register fed continuously. Model it as a fixed setup cost of ~750–1250
cycles plus the pure SPI bit time; there is no extra per-transfer stall to
account for.

### SAR ADC (SENS)

Reachable since the RTC deny window was narrowed from 0x1000 to 0x800 — see the
guard section. `SENS_SAR_MEAS1_CTRL2_REG` is `0x6000880C`: bit 18
`MEAS1_START_FORCE`, bit 17 `MEAS1_START_SAR`, bit 16 `MEAS1_DONE_SAR`, bits
15:0 the sample.

**Conversion completes within one CPU read.** Writing `0x60000`
(`START_FORCE | START_SAR`) reads back `0x000709dd` on the very next access —
done already set, sample already loaded. Modelling it as instantaneous is
correct at any granularity firmware can observe.

**Clearing start does NOT clear done, and the sample goes stale rather than
invalid.** This is the one to get right:

```
w 6000880c 60000   ->  readback 000709e5   (force+start+done, sample 0x09e5)
w 6000880c 0       ->  readback 000109e5   (start cleared, DONE STILL SET)
r 6000880c         ->           000109e5   (stable; stale sample retained)
```

> A driver that re-arms by writing zero and then polls `done` sees a completion
> that belongs to the *previous* conversion, and reads the previous sample. Real
> hardware does not protect against that race. An emulator that clears `done`
> when start is cleared is **safer than the silicon**, which sounds harmless and
> is not: firmware carrying that race passes on the emulator and is flaky on the
> board, which is the worst direction for a divergence to point.

Live samples on this T-Deck read ~2525–2533 counts (`0x09dd`–`0x09e5`) on the
GPIO4 battery divider, if a plausible default is wanted.

**MISO reads `0x00` on this board** with the panel selected. The T-Deck's ST7789
wiring shares MISO (GPIO38) with SD and LoRa and the panel does not drive it,
so all-zeros is the correct expected value here, not a failed read.

---

## What the probe deliberately does not start

- **The module loader** — nothing to load; no UI in this build.
- **SPIFFS and SD** — both would put the flash controller and the SD card on
  the very SPI bus under study, and a background filesystem flush landing
  mid-trace is exactly what makes a capture unreproducible.
- **`purr_crash_guard` strikes** — a debug tool that disables itself after a few
  crashes is worse than useless; the point is to survive poking at things until
  they break.
- **WiFi / BT / LoRa** — the radios contend for SPI2 and generate background
  interrupt activity.

---

## Running it unattended

The board recovers on its own, because wedging it is a normal outcome of this
tool rather than an exception:

- **Panic → print backtrace, then reboot** (`PANIC_PRINT_REBOOT`). Not the GDB
  stub: that halts the CPU and waits for a human, so from a host script the port
  just goes quiet forever.
- **Task watchdog, 10 s, panics (and so reboots)** — catches the quieter case
  where nothing faults but a stalled bus stops the loop answering. The loop
  feeds it on every input poll, so idling at the prompt never trips it.
- **`probe_host.py` reconnects.** A `%hello` arriving mid-reply raises
  `RebootDetected` (deliberately *not* `ProbeError` — a refusal means the device
  is healthy and said no; a reboot means whatever was being measured is void),
  and the port is reopened when it re-enumerates.
- **`id`'s `reset_reason`** tells a script afterwards whether it rebooted, and why.

Sessions matter: peripheral state lives in the device, so `spiopen` and a later
`x` must share one connection.

```
probe_host.py --port COM3 run "spiopen 10000000 0 ; spitiming 04 00 00 00 n=100"
probe_host.py --port COM3 script baseline.txt      # one command per line, '#' comments
```

Running each command as its own invocation gives `spi-not-open` for everything
after the first.

### Editing sdkconfig_tdeck_plus_probe.overrides

`SDKCONFIG_DEFAULTS` only **seeds** the config. Once
`CoreOS/build_tdeck_plus_probe/sdkconfig` exists, edits to the overrides file
are silently ignored for any symbol already materialized there. Delete that
file to force regeneration:

```
rm CoreOS/build_tdeck_plus_probe/sdkconfig
```

Nothing warns you about this — the build succeeds and quietly keeps the old
value.

---

## Reproducibility checklist

When handing a capture to someone modelling the peripheral:

1. `id` first — record `elf_sha256` and `cpu_hz`.
2. `log 0` to silence ESP_LOG for the capture.
3. `guardtest` — confirms the build's gate is intact.
4. `regions` — tells the reader exactly what this build could and could not
   reach, so absent registers aren't mistaken for absent behaviour.
5. Then the actual `spiopen` / `x` sequence.

[`probe_host.py`](probe_host.py) does all five and writes a JSON capture.
