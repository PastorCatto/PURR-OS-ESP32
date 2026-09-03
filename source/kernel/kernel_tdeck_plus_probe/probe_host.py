#!/usr/bin/env python3
"""probe_host.py — host-side driver for the PURR OS hardware probe.

Speaks the line protocol documented in PROTOCOL.md to a T-Deck Plus running the
tdeck_plus_probe build, over its native USB CDC port.

    python probe_host.py --list
    python probe_host.py --port COM7 id
    python probe_host.py --port COM7 capture spi_baseline.json
    python probe_host.py --port COM7            # interactive

The parser drops every line that does not start with '%'. ESP_LOG output shares
this endpoint, and that sentinel is the only thing standing between a driver
warning arriving mid-response and a corrupted capture.

Requires pyserial (`pip install pyserial`).
"""

import argparse
import json
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit(
        "pyserial not installed.\n"
        "  pip install pyserial\n"
        "or use the ESP-IDF venv, which already has it:\n"
        "  C:\\Espressif\\tools\\python\\v5.3.5\\venv\\Scripts\\python.exe probe_host.py ..."
    )


# Reply terminators. Every command produces exactly one of these, which is why
# no command here needs a guessed timeout to know it is finished.
_OK = "%ok"
_ERR = "%err "


class ProbeError(RuntimeError):
    """Raised when the device answers %err. Carries the bare reason token."""

    def __init__(self, reason, command):
        super().__init__(f"{command!r} refused: {reason}")
        self.reason = reason
        self.command = command


class RebootDetected(RuntimeError):
    """The board restarted mid-command — panic, watchdog, or a wedging poke.

    Distinct from ProbeError on purpose: a refusal means the device is healthy
    and said no; this means the device went away and whatever was being measured
    is void. Callers that treat the two the same will record garbage as data.
    """


class Probe:
    def __init__(self, port, baud=115200, timeout=5.0):
        # Baud is irrelevant over CDC but pyserial wants a number.
        self.baud = baud
        self.timeout = timeout
        self.port = port
        self.log = []
        self.reconnects = 0
        self.ser = serial.Serial(port, baud, timeout=0.2)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def reconnect(self, settle=8.0):
        """Wait for the port to come back after the board reboots.

        The firmware reboots on panic and on a watchdog trip, both of which are
        EXPECTED outcomes of poking registers by hand. The USB CDC endpoint is
        provided by the chip itself, so a reboot drops the port and re-enumerates
        it a second or two later. Without this, the first wedge ends the session
        and everything after it is lost — which defeats running unattended.
        """
        self.close()
        deadline = time.time() + settle
        last = None
        while time.time() < deadline:
            time.sleep(0.4)
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
                self.drain(0.6)
                self.reconnects += 1
                return True
            except Exception as exc:      # port not back yet
                last = exc
        raise IOError(f"{self.port} did not come back within {settle}s ({last})")

    def _readline(self):
        raw = self.ser.readline()
        if not raw:
            return None
        try:
            return raw.decode("utf-8", errors="replace").rstrip("\r\n")
        except Exception:
            return None

    def drain(self, seconds=0.4):
        """Swallow the boot banner or anything left over from a prior command."""
        end = time.time() + seconds
        seen = []
        while time.time() < end:
            line = self._readline()
            if line is not None:
                seen.append(line)
        return seen

    def command(self, cmd, timeout=None):
        """Send one command, return its %d payload lines. Raises on %err.

        A `%hello` arriving mid-reply means the board rebooted underneath us —
        surfaced as RebootDetected rather than silently returning a truncated
        reply, because a partial capture that looks complete is worse than a
        loud failure.
        """
        limit = timeout or self.timeout
        try:
            self.ser.reset_input_buffer()
            self.ser.write((cmd + "\n").encode())
            self.ser.flush()
        except serial.SerialException as exc:
            raise RebootDetected(f"port dropped writing {cmd!r}: {exc}")

        data = []
        deadline = time.time() + limit
        while time.time() < deadline:
            try:
                line = self._readline()
            except serial.SerialException as exc:
                raise RebootDetected(f"port dropped reading {cmd!r}: {exc}")
            if line is None:
                continue
            if not line.startswith("%"):
                continue  # ESP_LOG noise — see module docstring
            if line == _OK:
                self.log.append({"cmd": cmd, "data": data, "ok": True})
                return data
            if line.startswith(_ERR):
                reason = line[len(_ERR):].strip()
                self.log.append({"cmd": cmd, "data": data, "ok": False, "err": reason})
                raise ProbeError(reason, cmd)
            if line.startswith("%d "):
                data.append(line[3:])
            elif line.startswith("%hello"):
                raise RebootDetected(f"board rebooted during {cmd!r}")
            # %ready and anything else: ignore, they are not replies.
        raise TimeoutError(f"no terminator for {cmd!r} within {limit}s")


def _kv(lines, key):
    """First payload line starting with `key `, minus the key."""
    for ln in lines:
        if ln.startswith(key + " "):
            return ln[len(key) + 1:]
    return None


def cmd_list(_args):
    ports = list(list_ports.comports())
    if not ports:
        print("no serial ports found")
        return 1
    for p in ports:
        # The S3's native USB is Espressif VID 0x303a. Flagging it saves
        # guessing when several CDC devices are attached.
        hint = "  <-- likely ESP32-S3 native USB" if (p.vid == 0x303A) else ""
        print(f"{p.device:12} {p.description}{hint}")
    return 0


def _preamble(pr):
    """The reproducibility checklist from PROTOCOL.md, as structured data."""
    info = {}

    ident = pr.command("id")
    info["id"] = ident
    info["elf_sha256"] = _kv(ident, "elf_sha256")
    info["cpu_hz"] = _kv(ident, "cpu_hz")

    # Silence ESP_LOG so the capture is clean. Done after `id` so any startup
    # complaint is still visible in the log above.
    try:
        pr.command("log 0")
    except ProbeError:
        pass

    # Refuse to record a capture from a build whose access gate is not intact.
    guard = pr.command("guardtest")
    info["guardtest"] = guard
    if _kv(guard, "all_refused") != "1":
        raise ProbeError("guard-breach", "guardtest")

    info["regions"] = pr.command("regions")
    return info


def cmd_capture(args):
    pr = Probe(args.port, timeout=args.timeout)
    try:
        pr.drain()
        capture = {
            "port": args.port,
            "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "preamble": _preamble(pr),
            "transactions": [],
        }

        pr.command(f"spiopen {args.hz} {args.mode}")

        # Default probe set: an ST7789 RDDID (0x04) read, a NOP, and a
        # zero-fill. Three shapes rather than one — a command with a real
        # reply, a command with none, and an all-zero payload — because an
        # emulator can special-case any single one of them by accident.
        payloads = args.xfer or ["04 00 00 00", "00", "00 00 00 00 00 00 00 00"]
        for p in payloads:
            entry = {"tx": p}
            try:
                lines = pr.command(f"x {p}")
                entry["ok"] = True
            except ProbeError as e:
                lines = []
                entry["ok"] = False
                entry["err"] = e.reason
            entry["miso"] = _kv(lines, "miso")
            entry["events"] = [ln[3:] for ln in lines if ln.startswith("ev ")]
            entry["timing"] = [ln[7:] for ln in lines if ln.startswith("timing ")]
            capture["transactions"].append(entry)
            print(f"  {p}  ->  miso {entry.get('miso')}  "
                  f"({len(entry['events'])} events)")

        with open(args.outfile, "w", encoding="utf-8") as fh:
            json.dump(capture, fh, indent=2)
        print(f"\nwrote {args.outfile}")
        print(f"elf_sha256 {capture['preamble']['elf_sha256']}")
        print(f"cpu_hz     {capture['preamble']['cpu_hz']}")
        return 0
    finally:
        pr.close()


def _run_sequence(pr, commands, echo=True):
    """Run commands in ONE session, so peripheral state persists between them.

    This matters more than it looks: `spiopen` configures the peripheral and
    every later `x` depends on it, but a fresh connection reboots or at minimum
    reconnects the board. Running each command in its own session — the obvious
    shape for a CLI — silently produces `spi-not-open` for everything after the
    first, which is a confusing way to learn this.
    """
    failed = 0
    for cmd in commands:
        cmd = cmd.strip()
        if not cmd or cmd.startswith("#"):
            continue
        if echo:
            print(f"> {cmd}")
        try:
            for line in pr.command(cmd):
                print(f"  {line}")
        except ProbeError as e:
            print(f"  ERR {e.reason}")
            failed += 1
        except RebootDetected as e:
            print(f"  REBOOT {e} — reconnecting")
            pr.reconnect()
            failed += 1
        except TimeoutError as e:
            print(f"  TIMEOUT {e}")
            failed += 1
    return failed


def cmd_one(args):
    # Semicolons split one invocation into several commands sharing a session.
    text = " ".join(args.words)
    commands = [c for c in text.split(";")]
    pr = Probe(args.port, timeout=args.timeout)
    try:
        pr.drain()
        failed = _run_sequence(pr, commands, echo=len(commands) > 1)
        return 1 if failed else 0
    finally:
        pr.close()


def cmd_script(args):
    with open(args.file, encoding="utf-8") as fh:
        commands = fh.read().splitlines()
    pr = Probe(args.port, timeout=args.timeout)
    try:
        pr.drain()
        failed = _run_sequence(pr, commands)
        if pr.reconnects:
            print(f"\n({pr.reconnects} reboot(s) recovered during this run)")
        print(f"\n{failed} command(s) failed")
        return 1 if failed else 0
    finally:
        pr.close()


def cmd_interactive(args):
    pr = Probe(args.port, timeout=args.timeout)
    try:
        banner = pr.drain(0.6)
        for ln in banner:
            if ln.startswith("%hello") or ln == "%ready":
                print(ln)
        print("connected — 'help' for commands, Ctrl-D or 'quit' to exit\n")
        while True:
            try:
                line = input("probe> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if not line:
                continue
            if line in ("quit", "exit"):
                return 0
            try:
                for out in pr.command(line):
                    print("  " + out)
                print("  ok")
            except ProbeError as e:
                print(f"  ERR {e.reason}")
            except RebootDetected as e:
                print(f"  REBOOT {e}")
                try:
                    pr.reconnect()
                    print("  reconnected")
                except IOError as re:
                    print(f"  RECONNECT FAILED {re}")
                    return 1
            except TimeoutError as e:
                print(f"  TIMEOUT {e}")
    finally:
        pr.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyACM0")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")

    # dest="action", not "mode" — `capture` has its own --mode for the SPI
    # clock mode, and the two would silently overwrite each other.
    sub = ap.add_subparsers(dest="action")

    cap = sub.add_parser("capture", help="run the reproducibility checklist, write JSON")
    cap.add_argument("outfile")
    cap.add_argument("--hz", type=int, default=10000000)
    cap.add_argument("--mode", type=int, default=0)
    cap.add_argument("--xfer", action="append",
                     help="hex byte payload; repeatable, replaces the defaults")

    run = sub.add_parser("run", help="run command(s); ';' separates, one session")
    run.add_argument("words", nargs=argparse.REMAINDER)

    scr = sub.add_parser("script", help="run a file of commands, one per line")
    scr.add_argument("file")

    args = ap.parse_args()

    if args.list:
        return cmd_list(args)
    if not args.port:
        print("no --port given; available ports:\n")
        cmd_list(args)
        return 2

    if args.action == "capture":
        return cmd_capture(args)
    if args.action == "run":
        return cmd_one(args)
    if args.action == "script":
        return cmd_script(args)
    return cmd_interactive(args)


if __name__ == "__main__":
    sys.exit(main())
