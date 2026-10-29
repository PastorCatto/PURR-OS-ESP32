"""Runs a real driver on the host, against a bus that is plain Python.

The driver's C source is compiled as a shared library (a driver is freestanding, so it needs nothing else), loaded with ctypes, and handed a bus
table whose functions are Python callables. A test then plays the part of the hardware: it feeds a "GPS" its sentences, answers a "radio" over SPI,
and checks what the driver did with them. This is how the reference drivers are proved before any board is involved.
"""
import ctypes as C
import os
import subprocess

import helpers

c_u8, c_u32, c_i32 = C.c_uint8, C.c_uint32, C.c_int32
PTR = C.c_void_p


def build(source, name):
    """Compile `source` into a shared library and load it."""
    out = os.path.join(helpers.tmpdir(), "lib%s.dylib" % name)
    cmd = [helpers.clang(), "-dynamiclib", "-fPIC", "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fvisibility=default"] + \
        ["-I" + i for i in helpers.INCLUDES] + [source, "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("the driver did not compile:\n" + r.stderr)
    return C.CDLL(out)


# ---- the structs (sizes are checked against the headers by layout_probe)
class KV(C.Structure):
    _fields_ = [("key", C.c_char * 8), ("val", c_i32)]


class KVCfg(C.Structure):
    _fields_ = [("count", c_u8), ("kv", KV * 12)]


class Device(C.Structure):
    _fields_ = [("name", C.c_char_p), ("compatible", C.c_char_p), ("priority", C.c_int), ("extra_pins", C.c_int * 6), ("cfg", PTR)]


PROBE = C.CFUNCTYPE(C.c_int, C.POINTER(Device), PTR, C.POINTER(PTR))
REMOVE = C.CFUNCTYPE(None, PTR)
READ = C.CFUNCTYPE(C.c_int, PTR, PTR, C.c_size_t)


class Driver(C.Structure):
    _fields_ = [("name", C.c_char_p), ("compatible", C.c_char_p), ("probe", PROBE), ("remove", REMOVE), ("read", READ), ("bus_abi", c_u32),
                ("class_id", c_u32), ("ops", PTR)]


class GnssFix(C.Structure):
    _fields_ = [("valid", c_u8), ("quality", c_u8), ("sats_used", c_u8), ("sats_view", c_u8), ("time_valid", c_u8), ("month", c_u8), ("day", c_u8),
                ("hour", c_u8), ("minute", c_u8), ("second", c_u8), ("year", C.c_uint16), ("lat_e7", c_i32), ("lon_e7", c_i32), ("alt_mm", c_i32),
                ("speed_mmps", c_u32), ("course_cdeg", c_i32), ("hdop_x10", C.c_uint16), ("age_ms", c_u32)]


class LoraCfg(C.Structure):
    _fields_ = [("freq_hz", c_u32), ("bw_hz", c_u32), ("sf", c_u8), ("cr", c_u8), ("tx_dbm", C.c_int8), ("crc_on", c_u8), ("implicit_header", c_u8),
                ("reserved", c_u8), ("preamble", C.c_uint16), ("sync_word", C.c_uint16)]


class LoraRx(C.Structure):
    _fields_ = [("rssi_dbm", C.c_int16), ("snr_db", C.c_int8), ("reserved", c_u8)]


class GnssOps(C.Structure):
    _fields_ = [("version", c_u32), ("power", C.CFUNCTYPE(C.c_int, PTR, C.c_int)), ("poll", C.CFUNCTYPE(C.c_int, PTR)),
                ("get_fix", C.CFUNCTYPE(C.c_int, PTR, C.POINTER(GnssFix))), ("set_rate_hz", C.CFUNCTYPE(C.c_int, PTR, C.c_int))]


class LoraOps(C.Structure):
    _fields_ = [("version", c_u32), ("configure", C.CFUNCTYPE(C.c_int, PTR, C.POINTER(LoraCfg))),
                ("send", C.CFUNCTYPE(C.c_int, PTR, PTR, C.c_size_t, c_u32)),
                ("receive", C.CFUNCTYPE(C.c_int, PTR, PTR, C.c_size_t, c_u32, C.POINTER(LoraRx))), ("sleep", C.CFUNCTYPE(C.c_int, PTR))]


class ModemStatus(C.Structure):
    _fields_ = [("registered", c_u8), ("roaming", c_u8), ("tech", c_u8), ("data_up", c_u8), ("rssi_dbm", C.c_int16), ("operator_name", C.c_char * 20),
                ("imei", C.c_char * 18), ("ip", C.c_char * 16)]


class ModemOps(C.Structure):
    _fields_ = [("version", c_u32), ("power", C.CFUNCTYPE(C.c_int, PTR, C.c_int)), ("status", C.CFUNCTYPE(C.c_int, PTR, C.POINTER(ModemStatus))),
                ("connect", C.CFUNCTYPE(C.c_int, PTR, C.c_char_p, C.c_char_p, C.c_char_p)), ("disconnect", C.CFUNCTYPE(C.c_int, PTR)),
                ("at", C.CFUNCTYPE(C.c_int, PTR, C.c_char_p, PTR, C.c_size_t, c_u32)), ("sockets", PTR)]


class WifiAp(C.Structure):
    _fields_ = [("ssid", C.c_char * 33), ("rssi_dbm", C.c_int8), ("channel", c_u8), ("secure", c_u8)]


class WifiStatus(C.Structure):
    _fields_ = [("connected", c_u8), ("rssi_dbm", C.c_int8), ("channel", c_u8), ("reserved", c_u8), ("ssid", C.c_char * 33), ("ip", C.c_char * 16)]


class SockOps(C.Structure):
    _fields_ = [("version", c_u32), ("open", C.CFUNCTYPE(C.c_int, PTR, C.c_char_p, C.c_uint16, C.c_int, c_u32)),
                ("send", C.CFUNCTYPE(C.c_int, PTR, C.c_int, C.c_char_p, C.c_size_t)),
                ("recv", C.CFUNCTYPE(C.c_int, PTR, C.c_int, PTR, C.c_size_t, c_u32)), ("close", C.CFUNCTYPE(C.c_int, PTR, C.c_int))]


class WifiOps(C.Structure):
    _fields_ = [("version", c_u32), ("power", C.CFUNCTYPE(C.c_int, PTR, C.c_int)),
                ("scan", C.CFUNCTYPE(C.c_int, PTR, C.POINTER(WifiAp), C.c_int, c_u32)),
                ("connect", C.CFUNCTYPE(C.c_int, PTR, C.c_char_p, C.c_char_p, c_u32)), ("disconnect", C.CFUNCTYPE(C.c_int, PTR)),
                ("status", C.CFUNCTYPE(C.c_int, PTR, C.POINTER(WifiStatus))), ("sockets", PTR), ("send_frame", PTR), ("recv_frame", PTR)]


class HciOps(C.Structure):
    _fields_ = [("version", c_u32), ("reset", C.CFUNCTYPE(C.c_int, PTR)), ("write", C.CFUNCTYPE(C.c_int, PTR, c_u8, C.c_char_p, C.c_size_t)),
                ("read", C.CFUNCTYPE(C.c_int, PTR, PTR, C.c_size_t, c_u32))]


def kv_cfg(**values):
    cfg = KVCfg()
    cfg.count = len(values)
    for i, (k, v) in enumerate(values.items()):
        cfg.kv[i].key = k.encode()
        cfg.kv[i].val = v
    return cfg


class Bus:
    """The hardware, as Python. Subclass or monkey-patch the methods a test cares about; the rest do nothing sensible-but-harmless."""

    def __init__(self):
        self.t_us = 0
        self.gpio = {}
        self.log = []

    # SPI
    def spi_add_device(self, host, cs, mode, hz): return 1
    def spi_transfer(self, handle, tx, n): return bytes(n)
    # GPIO and time
    def gpio_set_output(self, pin, level): self.gpio[pin] = level
    def gpio_set_input_pullup(self, pin): pass
    def gpio_get(self, pin): return self.gpio.get(pin, 0)
    def gpio_write(self, pin, level): self.gpio[pin] = level
    def delay_ms(self, ms): self.t_us += ms * 1000
    def now_us(self): self.t_us += 50; return self.t_us
    # UART
    def uart_open(self, port, tx, rx, baud, bufsize): return port + 1
    def uart_write(self, handle, data): self.log.append(("uart_write", bytes(data))); return len(data)
    def uart_read(self, handle, cap, timeout_ms): return b""
    def uart_set_baud(self, handle, baud): return 0
    def uart_close(self, handle): pass

    def table(self):
        """The ctypes bus table; keep the returned object alive as long as the driver is used."""
        b = self
        T = C.CFUNCTYPE

        def spi_transfer(h, tx, rx, n):
            data = C.string_at(tx, n) if tx else bytes(n)
            out = b.spi_transfer(h, data, n)
            if rx:
                C.memmove(rx, out, n)
            return 0

        def uart_read(h, buf, cap, timeout):
            data = b.uart_read(h, cap, timeout)[:cap]
            if data:
                C.memmove(buf, data, len(data))
            return len(data)

        def uart_write(h, data, n):
            return b.uart_write(h, C.string_at(data, n))

        funcs = [
            ("spi_add_device", T(c_u32, C.c_int, C.c_int, C.c_int, C.c_int), b.spi_add_device),
            ("spi_transfer", T(C.c_int, c_u32, PTR, PTR, C.c_size_t), spi_transfer),
            ("i2c_add_device", T(c_u32, C.c_int, C.c_int, C.c_int, C.c_uint16, C.c_int), lambda *a: 1),
            ("i2c_write", T(C.c_int, c_u32, PTR, C.c_size_t, C.c_int), lambda *a: 0),
            ("i2c_read", T(C.c_int, c_u32, PTR, C.c_size_t, C.c_int), lambda *a: 0),
            ("i2c_write_read", T(C.c_int, c_u32, PTR, C.c_size_t, PTR, C.c_size_t, C.c_int), lambda *a: 0),
            ("i2c_remove_device", T(None, c_u32), lambda *a: None),
            ("gpio_set_output", T(None, C.c_int, C.c_int), b.gpio_set_output),
            ("gpio_set_input_pullup", T(None, C.c_int), b.gpio_set_input_pullup),
            ("gpio_get", T(C.c_int, C.c_int), b.gpio_get),
            ("gpio_write", T(None, C.c_int, C.c_int), b.gpio_write),
            ("delay_ms", T(None, c_u32), b.delay_ms),
            ("now_us", T(C.c_uint64), b.now_us),
            ("dma_alloc", T(PTR, C.c_size_t), lambda n: None),
            ("backlight_init", T(C.c_int, C.c_int), lambda p: 0),
            ("backlight_set", T(None, c_u8), lambda v: None),
            ("adc_read_mv", T(C.c_int, C.c_int), lambda p: -1),
            ("uart_open", T(c_u32, C.c_int, C.c_int, C.c_int, C.c_int, C.c_size_t), b.uart_open),
            ("uart_write", T(C.c_int, c_u32, PTR, C.c_size_t), uart_write),
            ("uart_read", T(C.c_int, c_u32, PTR, C.c_size_t, c_u32), uart_read),
            ("uart_set_baud", T(C.c_int, c_u32, C.c_int), b.uart_set_baud),
            ("uart_close", T(None, c_u32), b.uart_close),
        ]
        fields = [("abi_version", c_u32)] + [(n, t) for n, t, _ in funcs]

        class Table(C.Structure):
            _fields_ = fields

        tab = Table()
        tab.abi_version = 6
        keep = []
        for n, t, f in funcs:
            cb = t(f)
            keep.append(cb)
            setattr(tab, n, cb)
        tab._keep = keep
        return tab


class Loaded:
    """A driver loaded from its source, bound to a fake bus and one device."""

    def __init__(self, source, entry, bus, cfg, board_spi_host=1, compatible=b"x"):
        self.lib = build(source, entry)
        self.bus = bus
        self.table = bus.table()
        fn = getattr(self.lib, entry)
        fn.restype = C.POINTER(Driver)
        fn.argtypes = [PTR]
        self.drv = fn(C.byref(self.table)).contents
        self.cfg = cfg
        self.dev = Device(b"dev0", compatible, 2, (C.c_int * 6)(*([-1] * 6)), C.cast(C.pointer(cfg), PTR))
        # purr_board_t: the driver only reads spi.host; its offset comes from the header via a probe library
        self.board = C.create_string_buffer(512)
        off = _board_spi_host_offset()
        C.memmove(C.addressof(self.board) + off, C.byref(C.c_int(board_spi_host)), 4)
        self.handle = PTR()

    def probe(self):
        return self.drv.probe(C.byref(self.dev), C.addressof(self.board), C.byref(self.handle))

    def remove(self):
        self.drv.remove(self.handle)

    def ops(self, ops_type):
        return C.cast(self.drv.ops, C.POINTER(ops_type)).contents


_off = {}


def _board_spi_host_offset():
    if "spi" not in _off:
        d = helpers.tmpdir()
        src = os.path.join(d, "probe.c")
        with open(src, "w") as fh:
            fh.write('#include <stddef.h>\n#include "purr_board.h"\nunsigned long off_spi_host(void){return offsetof(purr_board_t, spi.host);}\n'
                     'unsigned long sz_board(void){return sizeof(purr_board_t);}\n'
                     '#include "purr_driver.h"\nunsigned long sz_device(void){return sizeof(purr_device_t);}\n'
                     'unsigned long sz_driver(void){return sizeof(purr_driver_t);}\nunsigned long sz_kv(void){return sizeof(purr_kv_cfg_t);}\n'
                     '#include "purr_driver_class.h"\nunsigned long sz_fix(void){return sizeof(purr_gnss_fix_t);}\nunsigned long sz_lcfg(void){return sizeof(purr_lora_cfg_t);}\n')
        lib = build(src, "probe")
        for fn in ("off_spi_host", "sz_board", "sz_device", "sz_driver", "sz_kv", "sz_fix", "sz_lcfg"):
            getattr(lib, fn).restype = C.c_ulong
        _off["spi"] = lib.off_spi_host()
        assert lib.sz_board() < 512, "purr_board_t grew past the test buffer"
        assert lib.sz_device() == C.sizeof(Device), (lib.sz_device(), C.sizeof(Device))
        assert lib.sz_driver() == C.sizeof(Driver), (lib.sz_driver(), C.sizeof(Driver))
        assert lib.sz_kv() == C.sizeof(KVCfg), (lib.sz_kv(), C.sizeof(KVCfg))
        assert lib.sz_fix() == C.sizeof(GnssFix), (lib.sz_fix(), C.sizeof(GnssFix))
        assert lib.sz_lcfg() == C.sizeof(LoraCfg), (lib.sz_lcfg(), C.sizeof(LoraCfg))
    return _off["spi"]
