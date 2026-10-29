"""The reference drivers, run for real against a pretend bus (see driverhost.py)."""
import ctypes as C
import os
import unittest

import driverhost as dh
import helpers

DRIVERS = os.path.join(helpers.ROOT, "Modules", "drivers")


def nmea(body):
    cs = 0
    for ch in body.encode():
        cs ^= ch
    return ("$%s*%02X\r\n" % (body, cs)).encode()


class FakeGps(dh.Bus):
    def __init__(self):
        super().__init__()
        self.rx = bytearray()

    def feed(self, data):
        self.rx += data

    def uart_read(self, handle, cap, timeout_ms):
        out, self.rx = bytes(self.rx[:cap]), self.rx[cap:]
        return out


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class Gnss(unittest.TestCase):
    def load(self):
        bus = FakeGps()
        ld = dh.Loaded(os.path.join(DRIVERS, "gnss_nmea_module.c"), "gnss_nmea_entry", bus, dh.kv_cfg(port=1, tx=43, rx=44, baud=38400))
        self.assertEqual(ld.probe(), 0)
        self.addCleanup(ld.remove)
        return ld, bus, ld.ops(dh.GnssOps)

    def test_a_position_from_gga_and_rmc(self):
        ld, bus, ops = self.load()
        bus.feed(nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"))
        bus.feed(nmea("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W"))
        fix = dh.GnssFix()
        self.assertEqual(ops.get_fix(ld.handle, C.byref(fix)), 1)
        self.assertEqual(fix.valid, 1)
        self.assertEqual(fix.sats_used, 8)
        self.assertAlmostEqual(fix.lat_e7 / 1e7, 48.1173, places=4)
        self.assertAlmostEqual(fix.lon_e7 / 1e7, 11.516667, places=4)
        self.assertAlmostEqual(fix.alt_mm / 1000.0, 545.4, places=1)
        self.assertAlmostEqual(fix.hdop_x10 / 10.0, 0.9, places=1)
        self.assertEqual((fix.hour, fix.minute, fix.second), (12, 35, 19))
        self.assertEqual((fix.year, fix.month, fix.day), (1994, 3, 23))
        self.assertAlmostEqual(fix.speed_mmps / 1000.0, 22.4 * 0.514444, delta=0.05)
        self.assertAlmostEqual(fix.course_cdeg / 100.0, 84.4, places=1)

    def test_south_and_west_are_negative(self):
        ld, bus, ops = self.load()
        bus.feed(nmea("GPGGA,010203,3352.000,S,15112.000,W,1,07,1.1,12.0,M,0.0,M,,"))
        fix = dh.GnssFix()
        ops.get_fix(ld.handle, C.byref(fix))
        self.assertLess(fix.lat_e7, 0)
        self.assertLess(fix.lon_e7, 0)
        self.assertAlmostEqual(fix.lat_e7 / 1e7, -33.866667, places=4)

    def test_a_bad_checksum_is_ignored(self):
        ld, bus, ops = self.load()
        bad = nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,").replace(b"*", b"*0")[:-3] + b"\r\n"
        bad = bad.replace(b"*0", b"*FF") if b"*0" in bad else bad
        bus.feed(b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n")
        fix = dh.GnssFix()
        self.assertEqual(ops.get_fix(ld.handle, C.byref(fix)), 0)
        self.assertEqual(fix.valid, 0)

    def test_no_fix_when_the_receiver_says_v(self):
        ld, bus, ops = self.load()
        bus.feed(nmea("GPRMC,123519,V,,,,,,,230394,,"))
        fix = dh.GnssFix()
        self.assertEqual(ops.get_fix(ld.handle, C.byref(fix)), 0)

    def test_sentences_split_across_reads(self):
        ld, bus, ops = self.load()
        data = nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,")
        fix = dh.GnssFix()
        bus.feed(data[:20])
        self.assertEqual(ops.get_fix(ld.handle, C.byref(fix)), 0)
        bus.feed(data[20:])
        self.assertEqual(ops.get_fix(ld.handle, C.byref(fix)), 1)

    def test_probe_needs_a_receive_pin(self):
        bus = FakeGps()
        ld = dh.Loaded(os.path.join(DRIVERS, "gnss_nmea_module.c"), "gnss_nmea_entry", bus, dh.kv_cfg(port=1, tx=43, rx=-1))
        self.assertEqual(ld.probe(), -1)

    def test_rate_command_is_a_ubx_message(self):
        ld, bus, ops = self.load()
        self.assertEqual(ops.set_rate_hz(ld.handle, 5), 0)
        sent = [d for k, d in bus.log if k == "uart_write"][-1]
        self.assertEqual(sent[:4], bytes([0xB5, 0x62, 0x06, 0x08]))
        self.assertEqual(int.from_bytes(sent[6:8], "little"), 200)         # 5 Hz is a 200 ms measurement rate


class FakeSx1262(dh.Bus):
    """Enough of an SX1262 to be configured and to send and receive: it keeps what it was told, and answers status and IRQ reads."""

    def __init__(self, present=True):
        super().__init__()
        self.present = present
        self.cmds = []
        self.regs = {}
        self.buf = bytearray(256)
        self.irq = 0
        self.mode = 2                                   # STBY_RC
        self.pending_rx = None
        self.busy_pin = 13

    def gpio_get(self, pin):
        if pin == self.busy_pin:
            return 0 if self.present else 1             # no chip: BUSY floats high
        return 0

    def spi_transfer(self, handle, tx, n):
        if not self.present:
            return b"\xff" * n
        op = tx[0]
        self.cmds.append((op, tx[1:]))
        out = bytearray(n)
        out[0] = (self.mode << 4) | 0x04
        if op == 0xC0:
            out[1] = (self.mode << 4) | 0x04
        elif op == 0x80:
            self.mode = 2
        elif op == 0x0D:
            addr = (tx[1] << 8) | tx[2]
            for i, b in enumerate(tx[3:]):
                self.regs[addr + i] = b
        elif op == 0x0E:
            for i, b in enumerate(tx[2:]):
                self.buf[tx[1] + i] = b
        elif op == 0x83:                                # SetTx: it transmits at once
            self.irq |= 0x0001
        elif op == 0x82:                                # SetRx: a packet is waiting
            if self.pending_rx is not None:
                self.irq |= 0x0002
        elif op == 0x12:
            out[2], out[3] = self.irq >> 8, self.irq & 255
        elif op == 0x02:
            self.irq &= ~((tx[1] << 8) | tx[2])
        elif op == 0x13:
            out[2], out[3] = len(self.pending_rx or b""), 0
        elif op == 0x14:
            out[2], out[3], out[4] = 2 * 87, (4 * 7) & 255, 2 * 85       # rssi -87.0 dBm, snr +7 dB
        elif op == 0x1E:
            data = self.pending_rx or b""
            for i, b in enumerate(data):
                out[3 + i] = b
        return bytes(out)

    def last(self, op):
        return [a for o, a in self.cmds if o == op][-1]

    def seen(self, op):
        return any(o == op for o, _ in self.cmds)


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class Lora(unittest.TestCase):
    CFG = dict(cs=9, busy=13, dio1=45, rst=-1, tcxo_mv=1800, dio2sw=1)

    def load(self, present=True, **over):
        bus = FakeSx1262(present)
        ld = dh.Loaded(os.path.join(DRIVERS, "lora_sx126x_module.c"), "lora_sx126x_entry", bus, dh.kv_cfg(**dict(self.CFG, **over)))
        return ld, bus

    def cfg(self, **kw):
        c = dh.LoraCfg()
        c.freq_hz, c.bw_hz, c.sf, c.cr, c.tx_dbm, c.crc_on, c.preamble, c.sync_word = 868000000, 125000, 7, 5, 14, 1, 8, 0x1424
        for k, v in kw.items():
            setattr(c, k, v)
        return c

    def test_probe_sets_up_the_chip(self):
        ld, bus = self.load()
        self.assertEqual(ld.probe(), 0)
        self.assertEqual(bus.last(0x97)[0], 2)                          # DIO3 as TCXO at 1.8 V is code 2 (1.6, 1.7, 1.8, 2.2 ...)
        self.assertEqual(bus.last(0x9D), bytes([1]))                    # DIO2 as the RF switch
        self.assertEqual(bus.last(0x8A), bytes([1]))                    # packet type LoRa
        self.assertEqual(bus.last(0x96), bytes([1]))                    # DC-DC regulator
        ld.remove()

    def test_probe_fails_when_nothing_answers(self):
        ld, bus = self.load(present=False)
        self.assertEqual(ld.probe(), -1)

    def test_configure_868_mhz_sf7(self):
        ld, bus = self.load()
        ld.probe()
        ops = ld.ops(dh.LoraOps)
        self.assertEqual(ops.configure(ld.handle, C.byref(self.cfg())), 0)
        self.assertEqual(int.from_bytes(bus.last(0x86), "big"), 0x36400000)         # 868 MHz x 2^25 / 32 MHz
        self.assertEqual(bus.last(0x98), bytes([0xD7, 0xDB]))                       # image calibration for 863-870 MHz
        self.assertEqual(bus.last(0x8B)[:4], bytes([7, 0x04, 1, 0]))                # SF7, 125 kHz, 4/5, no low-data-rate optimisation
        self.assertEqual(bus.last(0x8E)[0], 14)
        self.assertEqual((bus.regs[0x0740], bus.regs[0x0741]), (0x14, 0x24))        # the private sync word
        ld.remove()

    def test_sf12_turns_on_low_data_rate_optimisation(self):
        ld, bus = self.load()
        ld.probe()
        ld.ops(dh.LoraOps).configure(ld.handle, C.byref(self.cfg(sf=12)))
        self.assertEqual(bus.last(0x8B)[3], 1)

    def test_bad_settings_are_refused(self):
        ld, bus = self.load()
        ld.probe()
        ops = ld.ops(dh.LoraOps)
        self.assertEqual(ops.configure(ld.handle, C.byref(self.cfg(sf=4))), -1)
        self.assertEqual(ops.configure(ld.handle, C.byref(self.cfg(freq_hz=100000000))), -1)
        self.assertEqual(ops.send(ld.handle, b"x", 1, 100), -1)                     # not configured yet

    def test_send_writes_the_buffer_and_waits_for_tx_done(self):
        ld, bus = self.load()
        ld.probe()
        ops = ld.ops(dh.LoraOps)
        ops.configure(ld.handle, C.byref(self.cfg()))
        msg = b"hello purr"
        self.assertEqual(ops.send(ld.handle, msg, len(msg), 1000), 0)
        self.assertEqual(bytes(bus.buf[:len(msg)]), msg)
        self.assertEqual(bus.last(0x8C)[3], len(msg))                               # the packet length reached the chip
        self.assertTrue(bus.seen(0x83))

    def test_receive_a_packet_with_its_signal_info(self):
        ld, bus = self.load()
        ld.probe()
        ops = ld.ops(dh.LoraOps)
        ops.configure(ld.handle, C.byref(self.cfg()))
        bus.pending_rx = b"from afar"
        buf = C.create_string_buffer(64)
        info = dh.LoraRx()
        n = ops.receive(ld.handle, buf, 64, 500, C.byref(info))
        self.assertEqual(n, 9)
        self.assertEqual(buf.raw[:9], b"from afar")
        self.assertEqual(info.rssi_dbm, -87)
        self.assertEqual(info.snr_db, 7)

    def test_receive_times_out_quietly(self):
        ld, bus = self.load()
        ld.probe()
        ops = ld.ops(dh.LoraOps)
        ops.configure(ld.handle, C.byref(self.cfg()))
        buf = C.create_string_buffer(8)
        self.assertEqual(ops.receive(ld.handle, buf, 8, 50, None), 0)


if __name__ == "__main__":
    unittest.main()


class FakeAtDevice(dh.Bus):
    """A modem or Wi-Fi module that answers AT commands. `handlers` maps a command (or its prefix) to the reply text; each reply is sent with a trailing OK
    unless it already holds a final line."""

    def __init__(self, handlers=None, awake=True, wake_pin=None):
        super().__init__()
        self.handlers = handlers or {}
        self.awake = awake
        self.wake_pin = wake_pin
        self.outq = bytearray()
        self.inbuf = bytearray()
        self.commands = []

    def gpio_set_output(self, pin, level):
        super().gpio_set_output(pin, level)
        if pin == self.wake_pin and level:
            self.awake = True

    def uart_write(self, handle, data):
        self.log.append(("uart_write", bytes(data)))
        self.inbuf += data
        while b"\r" in self.inbuf:
            line, _, rest = bytes(self.inbuf).partition(b"\r")
            self.inbuf = bytearray(rest)
            self.command(line.decode())
        return len(data)

    def command(self, line):
        line = line.strip()
        self.commands.append(line)
        if not self.awake:
            return
        reply = None
        for key in sorted(self.handlers, key=len, reverse=True):
            if line.startswith(key):
                reply = self.handlers[key]
                break
        if reply is None:
            reply = ""
        if callable(reply):
            reply = reply(line)
        final = reply.rstrip().splitlines()[-1] if reply.strip() else ""
        text = "\r\n" + reply.replace("\n", "\r\n") + ("\r\n" if reply and not reply.endswith("\n") else "")
        if not (final.startswith("OK") or final.startswith("ERROR") or final.startswith("+CME ERROR") or final == "FAIL"):
            text += "\r\nOK\r\n"
        self.outq += text.encode()

    def uart_read(self, handle, cap, timeout_ms):
        out, self.outq = bytes(self.outq[:cap]), self.outq[cap:]
        if not out:
            self.t_us += max(timeout_ms, 1) * 1000
        return out

    def sent(self, prefix):
        return [c for c in self.commands if c.startswith(prefix)]


MODEM_OK = {
    "AT+CEREG?": "+CEREG: 0,1", "AT+CSQ": "+CSQ: 18,99", 'AT+COPS?': '+COPS: 0,0,"Test Net",7', "AT+CGSN": "353456789012345",
    "AT+CGPADDR=1": '+CGPADDR: 1,"10.20.30.40"', "AT+CGDCONT": "", "AT+CGACT": "", "AT+CGAUTH": "", "AT+CFUN": "", "ATE0": "", "AT+CMEE": "", "AT": "",
}


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class Modem(unittest.TestCase):
    def load(self, handlers=None, **kw):
        bus = FakeAtDevice(dict(MODEM_OK, **(handlers or {})), **kw)
        cfg = dict(port=2, tx=17, rx=18, baud=115200)
        cfg.update(kw.pop("cfg", {}) if "cfg" in kw else {})
        ld = dh.Loaded(os.path.join(DRIVERS, "modem_at_module.c"), "modem_at_entry", bus, dh.kv_cfg(**self.CFG))
        self.assertEqual(ld.probe(), 0)
        self.addCleanup(ld.remove)
        return ld, bus, ld.ops(dh.ModemOps)

    CFG = dict(port=2, tx=17, rx=18, baud=115200, pwr=40)

    def test_status_on_a_good_lte_network(self):
        ld, bus, ops = self.load()
        st = dh.ModemStatus()
        self.assertEqual(ops.status(ld.handle, C.byref(st)), 0)
        self.assertEqual((st.registered, st.roaming, st.tech, st.data_up), (1, 0, 2, 1))
        self.assertEqual(st.rssi_dbm, -77)
        self.assertEqual(st.operator_name.decode(), "Test Net")
        self.assertEqual(st.imei.decode(), "353456789012345")
        self.assertEqual(st.ip.decode(), "10.20.30.40")

    def test_roaming_and_not_registered(self):
        ld, bus, ops = self.load({"AT+CEREG?": "+CEREG: 0,5"})
        st = dh.ModemStatus()
        ops.status(ld.handle, C.byref(st))
        self.assertEqual((st.registered, st.roaming), (1, 1))
        ld2, bus2, ops2 = self.load({"AT+CEREG?": "+CEREG: 0,2", "AT+CREG?": "+CREG: 0,0"})
        st2 = dh.ModemStatus()
        ops2.status(ld2.handle, C.byref(st2))
        self.assertEqual(st2.registered, 0)
        self.assertEqual(st2.operator_name, b"")

    def test_a_gsm_fallback_registers_through_creg(self):
        ld, bus, ops = self.load({"AT+CEREG?": "+CEREG: 0,0", "AT+CREG?": "+CREG: 0,1", "AT+COPS?": '+COPS: 0,0,"Old Net",0'})
        st = dh.ModemStatus()
        ops.status(ld.handle, C.byref(st))
        self.assertEqual((st.registered, st.tech), (1, 1))

    def test_no_session_address_means_data_is_down(self):
        ld, bus, ops = self.load({"AT+CGPADDR=1": '+CGPADDR: 1,"0.0.0.0"'})
        st = dh.ModemStatus()
        ops.status(ld.handle, C.byref(st))
        self.assertEqual((st.data_up, st.ip), (0, b""))

    def test_connect_sets_the_apn_and_brings_the_session_up(self):
        ld, bus, ops = self.load()
        self.assertEqual(ops.connect(ld.handle, b"internet", None, None), 0)
        self.assertEqual(bus.sent("AT+CGDCONT"), ['AT+CGDCONT=1,"IP","internet"'])
        self.assertEqual(bus.sent("AT+CGACT"), ["AT+CGACT=1,1"])
        self.assertEqual(bus.sent("AT+CGAUTH"), [])

    def test_connect_with_a_login(self):
        ld, bus, ops = self.load()
        self.assertEqual(ops.connect(ld.handle, b"corp", b"bob", b"s3cret"), 0)
        self.assertEqual(bus.sent("AT+CGAUTH"), ['AT+CGAUTH=1,1,"s3cret","bob"'])

    def test_connect_fails_when_the_network_refuses(self):
        ld, bus, ops = self.load({"AT+CGACT=1": "+CME ERROR: 30"})
        self.assertEqual(ops.connect(ld.handle, b"internet", None, None), -1)

    def test_a_silent_modem_times_out_instead_of_hanging(self):
        ld, bus, ops = self.load(awake=False)
        st = dh.ModemStatus()
        self.assertEqual(ops.status(ld.handle, C.byref(st)), -1)

    def test_power_up_pulses_the_power_key_then_waits_for_an_answer(self):
        ld, bus, ops = self.load(awake=False, wake_pin=40)
        self.assertEqual(ops.power(ld.handle, 1), 0)
        self.assertEqual(bus.gpio.get(40), 0)                                # the key is released again
        self.assertIn("ATE0", bus.commands)
        self.assertIn("AT+CMEE=1", bus.commands)

    def test_a_dead_modem_fails_power_up(self):
        ld, bus, ops = self.load(awake=False)
        self.assertEqual(ops.power(ld.handle, 1), -1)

    def test_raw_commands(self):
        ld, bus, ops = self.load({"ATI": "Quectel\nBG95"})
        buf = C.create_string_buffer(128)
        n = ops.at(ld.handle, b"ATI", buf, 128, 1000)
        self.assertGreater(n, 0)
        self.assertIn(b"BG95", buf.value)
        self.assertIn(b"OK", buf.value)

    def test_probe_needs_both_uart_pins(self):
        bus = FakeAtDevice()
        ld = dh.Loaded(os.path.join(DRIVERS, "modem_at_module.c"), "modem_at_entry", bus, dh.kv_cfg(port=2, tx=-1, rx=18))
        self.assertEqual(ld.probe(), -1)


class FakeEsp(FakeAtDevice):
    """ESP-AT: also takes raw bytes after AT+CIPSEND, and serves passive-mode receive data."""

    def __init__(self, handlers=None, **kw):
        super().__init__(handlers, **kw)
        self.raw_need = 0
        self.raw_got = bytearray()
        self.sent_data = b""
        self.rx_data = {}                                # socket -> bytes waiting in the module

    def uart_write(self, handle, data):
        if self.raw_need:
            take = bytes(data)[:self.raw_need]
            self.raw_got += take
            self.raw_need -= len(take)
            if not self.raw_need:
                self.sent_data = bytes(self.raw_got)
                self.outq += b"\r\nRecv %d bytes\r\n\r\nSEND OK\r\n" % len(self.sent_data)
            return len(data)
        return super().uart_write(handle, data)

    def command(self, line):
        line = line.strip()
        if line.startswith("AT+CIPSEND="):
            self.commands.append(line)
            self.raw_need = int(line.split(",")[1])
            self.raw_got = bytearray()
            self.outq += b"\r\nOK\r\n>"
            return
        if line.startswith("AT+CIPRECVLEN?"):
            self.commands.append(line)
            lens = ",".join(str(len(self.rx_data.get(i, b""))) for i in range(5))
            extra = "0,CLOSED\r\n" if getattr(self, "closed0", False) else ""
            self.outq += (extra + "+CIPRECVLEN:%s\r\n\r\nOK\r\n" % lens).encode()
            return
        if line.startswith("AT+CIPRECVDATA="):
            self.commands.append(line)
            sock, want = [int(x) for x in line.split("=")[1].split(",")]
            data = self.rx_data.get(sock, b"")[:want]
            self.rx_data[sock] = self.rx_data.get(sock, b"")[len(data):]
            self.outq += b"+CIPRECVDATA:%d," % len(data) + data + b"\r\n\r\nOK\r\n"
            return
        super().command(line)


ESP_OK = {"ATE0": "", "AT+CWMODE": "", "AT+CIPMUX": "", "AT+CIPRECVMODE": "", "AT+CWQAP": "", "AT": "",
          'AT+CWJAP?': '+CWJAP:"MyNet","aa:bb:cc:dd:ee:ff",6,-57,0,3,0,0,1', "AT+CIPSTA?": '+CIPSTA:ip:"192.168.1.20"\n+CIPSTA:gateway:"192.168.1.1"',
          'AT+CWJAP=': "WIFI CONNECTED\nWIFI GOT IP"}


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class WifiAt(unittest.TestCase):
    def load(self, handlers=None, **kw):
        bus = FakeEsp(dict(ESP_OK, **(handlers or {})), **kw)
        ld = dh.Loaded(os.path.join(DRIVERS, "wifi_esp_at_module.c"), "wifi_esp_at_entry", bus, dh.kv_cfg(port=2, tx=17, rx=18, baud=115200, en=21))
        self.assertEqual(ld.probe(), 0)
        self.addCleanup(ld.remove)
        return ld, bus, ld.ops(dh.WifiOps)

    def test_power_sets_station_mode_and_passive_receive(self):
        ld, bus, ops = self.load(awake=False, wake_pin=21)
        self.assertEqual(ops.power(ld.handle, 1), 0)
        for cmd in ("ATE0", "AT+CWMODE=1", "AT+CIPMUX=1", "AT+CIPRECVMODE=1"):
            self.assertIn(cmd, bus.commands)

    def test_scan_lists_networks(self):
        scan = ('+CWLAP:(3,"Home WiFi",-48,"aa:bb:cc:00:00:01",6,-3,0)\n+CWLAP:(0,"Cafe, Free",-71,"aa:bb:cc:00:00:02",11,-3,0)\n'
                '+CWLAP:(4,"Quote\\"d",-80,"aa:bb:cc:00:00:03",1,-3,0)')
        ld, bus, ops = self.load({"AT+CWLAP": scan})
        aps = (dh.WifiAp * 8)()
        n = ops.scan(ld.handle, aps, 8, 5000)
        self.assertEqual(n, 3)
        self.assertEqual((aps[0].ssid, aps[0].rssi_dbm, aps[0].channel, aps[0].secure), (b"Home WiFi", -48, 6, 1))
        self.assertEqual((aps[1].ssid, aps[1].secure), (b"Cafe, Free", 0))
        self.assertEqual(aps[2].ssid, b'Quote"d')
        self.assertEqual(ops.scan(ld.handle, aps, 2, 5000), 2)                     # never more than asked for

    def test_connect_escapes_the_name_and_password(self):
        ld, bus, ops = self.load()
        self.assertEqual(ops.connect(ld.handle, b"My,Net", b'p"w', 10000), 0)
        self.assertEqual(bus.sent("AT+CWJAP="), ['AT+CWJAP="My\\,Net","p\\"w"'])

    def test_a_wrong_password_is_an_error(self):
        ld, bus, ops = self.load({"AT+CWJAP=": "+CWJAP:2\nFAIL"})
        self.assertEqual(ops.connect(ld.handle, b"Home", b"nope", 5000), -1)

    def test_status(self):
        ld, bus, ops = self.load()
        st = dh.WifiStatus()
        self.assertEqual(ops.status(ld.handle, C.byref(st)), 0)
        self.assertEqual((st.connected, st.ssid, st.ip, st.channel, st.rssi_dbm), (1, b"MyNet", b"192.168.1.20", 6, -57))
        ld2, bus2, ops2 = self.load({"AT+CWJAP?": "No AP"})
        st2 = dh.WifiStatus()
        ops2.status(ld2.handle, C.byref(st2))
        self.assertEqual((st2.connected, st2.ssid), (0, b""))

    def test_a_tcp_conversation(self):
        ld, bus, ops = self.load({"AT+CIPSTART=": "0,CONNECT", "AT+CIPCLOSE": "0,CLOSED"})
        so = C.cast(ops.sockets, C.POINTER(dh.SockOps)).contents
        sock = so.open(ld.handle, b"example.com", 80, 1, 5000)
        self.assertEqual(sock, 0)
        self.assertEqual(bus.sent("AT+CIPSTART="), ['AT+CIPSTART=0,"TCP","example.com",80'])
        req = b"GET / HTTP/1.0\r\n\r\n"
        self.assertEqual(so.send(ld.handle, sock, req, len(req)), len(req))
        self.assertEqual(bus.sent_data, req)
        bus.rx_data[0] = b"HTTP/1.0 200\r\nOK\r\n"                                     # carriage returns inside the data must not confuse the reader
        buf = C.create_string_buffer(64)
        n = so.recv(ld.handle, sock, buf, 64, 1000)
        self.assertEqual(buf.raw[:n], b"HTTP/1.0 200\r\nOK\r\n")
        self.assertEqual(so.recv(ld.handle, sock, buf, 64, 300), 0)                  # nothing more in time
        self.assertEqual(so.close(ld.handle, sock), 0)

    def test_a_closed_connection_is_reported(self):
        ld, bus, ops = self.load({"AT+CIPSTART=": "0,CONNECT"})
        so = C.cast(ops.sockets, C.POINTER(dh.SockOps)).contents
        sock = so.open(ld.handle, b"h", 1, 1, 1000)
        bus.closed0 = True
        buf = C.create_string_buffer(8)
        self.assertEqual(so.recv(ld.handle, sock, buf, 8, 500), -1)

    def test_udp_is_refused(self):
        ld, bus, ops = self.load()
        so = C.cast(ops.sockets, C.POINTER(dh.SockOps)).contents
        self.assertEqual(so.open(ld.handle, b"h", 1, 0, 1000), -1)


class FakeController(dh.Bus):
    """A Bluetooth controller: remembers what it was sent, answers HCI_Reset, and can be fed packets."""

    def __init__(self, answer_reset=True, reset_status=0):
        super().__init__()
        self.inq = bytearray()
        self.outq = bytearray()
        self.answer_reset = answer_reset
        self.reset_status = reset_status

    def uart_write(self, handle, data):
        self.log.append(("uart_write", bytes(data)))
        self.inq += data
        if bytes(self.inq[:4]) == b"\x01\x03\x0c\x00":
            self.inq = self.inq[4:]
            if self.answer_reset:
                self.outq += bytes([0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, self.reset_status])
        return len(data)

    def uart_read(self, handle, cap, timeout_ms):
        out, self.outq = bytes(self.outq[:cap]), self.outq[cap:]
        if not out:
            self.t_us += max(timeout_ms, 1) * 1000
        return out


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class HciH4(unittest.TestCase):
    def load(self, **kw):
        bus = FakeController(**kw)
        ld = dh.Loaded(os.path.join(DRIVERS, "hci_h4_module.c"), "hci_h4_entry", bus, dh.kv_cfg(port=2, tx=17, rx=18, baud=921600))
        self.assertEqual(ld.probe(), 0)
        self.addCleanup(ld.remove)
        return ld, bus, ld.ops(dh.HciOps)

    def test_reset_waits_for_command_complete(self):
        ld, bus, ops = self.load()
        self.assertEqual(ops.reset(ld.handle), 0)
        self.assertEqual([d for k, d in bus.log if k == "uart_write"][0], b"\x01\x03\x0c\x00")

    def test_reset_fails_when_the_controller_is_silent_or_refuses(self):
        ld, bus, ops = self.load(answer_reset=False)
        self.assertEqual(ops.reset(ld.handle), -1)
        ld2, bus2, ops2 = self.load(reset_status=0x0C)
        self.assertEqual(ops2.reset(ld2.handle), -1)

    def test_write_adds_the_type_byte(self):
        ld, bus, ops = self.load()
        self.assertEqual(ops.write(ld.handle, 2, b"\x01\x20\x03\x00abc", 7), 0)
        self.assertEqual(bus.log[-1][1], b"\x02\x01\x20\x03\x00abc")
        self.assertEqual(ops.write(ld.handle, 9, b"x", 1), -1)                         # not a packet type

    def test_read_sizes_each_packet_from_its_header(self):
        ld, bus, ops = self.load()
        event = bytes([4, 0x13, 5, 1, 2, 3, 4, 5])                                     # Number of Completed Packets, 5 parameter bytes
        acl = bytes([2, 0x01, 0x20, 0x03, 0x00]) + b"abc"                              # handle 0x2001, 3 bytes
        sco = bytes([3, 0x01, 0x00, 2]) + b"zz"
        bus.outq += event + acl + sco
        buf = C.create_string_buffer(64)
        for want in (event, acl, sco):
            n = ops.read(ld.handle, buf, 64, 100)
            self.assertEqual(buf.raw[:n], want)
        self.assertEqual(ops.read(ld.handle, buf, 64, 50), 0)                         # nothing more

    def test_a_packet_too_big_is_skipped_cleanly(self):
        ld, bus, ops = self.load()
        big = bytes([2, 0x01, 0x20, 100, 0]) + bytes(range(100))
        small = bytes([4, 0x05, 1, 0x16])
        bus.outq += big + small
        buf = C.create_string_buffer(32)
        self.assertEqual(ops.read(ld.handle, buf, 32, 100), -1)
        n = ops.read(ld.handle, buf, 32, 100)
        self.assertEqual(buf.raw[:n], small)

    def test_garbage_and_truncation_are_reported(self):
        ld, bus, ops = self.load()
        buf = C.create_string_buffer(32)
        bus.outq += bytes([0x7F])
        self.assertEqual(ops.read(ld.handle, buf, 32, 100), -2)
        bus.outq += bytes([4, 0x13, 5, 1, 2])                                          # promises 5 bytes, delivers 2
        self.assertEqual(ops.read(ld.handle, buf, 32, 100), -2)


class FakeBall(dh.Bus):
    def __init__(self):
        super().__init__()
        self.level = {3: 1, 15: 1, 1: 1, 2: 1, 0: 1}

    def gpio_get(self, pin):
        return self.level.get(pin, 1)


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class Trackball(unittest.TestCase):
    def load(self):
        bus = FakeBall()
        ld = dh.Loaded(os.path.join(DRIVERS, "trackball_gpio_module.c"), "trackball_gpio_entry", bus, dh.kv_cfg(up=3, down=15, left=1, right=2, click=0))
        self.assertEqual(ld.probe(), 0)
        self.addCleanup(ld.remove)
        return ld, bus

    def read(self, ld):
        buf = C.create_string_buffer(4)
        self.assertEqual(ld.drv.read(ld.handle, buf, 4), 4)
        return tuple(b if b < 128 else b - 256 for b in buf.raw[:3])

    def test_steps_and_click(self):
        ld, bus = self.load()
        self.assertEqual(self.read(ld), (0, 0, 0))                 # the first read only learns where the lines are
        bus.level[2] = 0                                          # one step right
        self.assertEqual(self.read(ld), (1, 0, 0))
        self.assertEqual(self.read(ld), (0, 0, 0))                 # no change, no step
        bus.level[2] = 1
        bus.level[3] = 0                                          # a step up
        self.assertEqual(self.read(ld), (1, -1, 0))                # the right line went back (a step), and up is negative
        bus.level[0] = 0
        self.assertEqual(self.read(ld)[2], 1)                      # the click is active low

    def test_probe_needs_at_least_one_pin(self):
        bus = FakeBall()
        ld = dh.Loaded(os.path.join(DRIVERS, "trackball_gpio_module.c"), "trackball_gpio_entry", bus, dh.kv_cfg())
        self.assertEqual(ld.probe(), -1)


class FakeSdCard(dh.Bus):
    """An SD card in SPI mode, byte by byte: the wire protocol of the SD Physical Layer spec (section 7), enough of it for a driver to start the card and move blocks.

    CS is the GPIO the driver drives itself; a card whose CS is high does not hear the bus and does not drive MISO (the bus reads 0xFF)."""
    CS = 39

    def __init__(self, sectors=4096, high=True, v2=True, present=True, acmd41_tries=3):
        super().__init__()
        self.sectors, self.high, self.v2, self.present = sectors, high, v2, present
        self.mem = bytearray(i % 251 for i in range(sectors * 512))
        self.devs = {}                       # handle -> (cs, hz)
        self.idle = False
        self.inited = False
        self.acmd41_left = acmd41_tries
        self.app = False                     # the next command is an application command (after CMD55)
        self.frame = bytearray()
        self.outq = []
        self.mode = "cmd"                    # cmd, wtoken, data
        self.multi_write = False
        self.stream = None
        self.gap = 0
        self.wlba = 0
        self.wdata = bytearray()
        self.cmds = []
        self.bad = []                        # protocol violations the test checks for
        self.bus_hz_seen = set()
        self.spi_cs_args = []
        self.written_tokens = []

    def spi_add_device(self, host, cs, mode, hz):
        h = len(self.devs) + 1
        self.devs[h] = (cs, hz)
        self.spi_cs_args.append(cs)
        return h

    def gpio_write(self, pin, level):
        if pin == self.CS and level == 1 and self.gpio.get(pin, 1) == 0:   # deselected mid-conversation: whatever it was doing is dropped
            self.frame = bytearray()
            self.stream = None
            if self.mode != "cmd":
                self.mode = "cmd"
        self.gpio[pin] = level

    def spi_transfer(self, handle, tx, n):
        hz = self.devs[handle][1]
        out = bytearray()
        for b in tx:
            out.append(self.byte(b, hz))
        return bytes(out)

    def addr(self, arg):
        return arg if self.high else arg // 512

    def byte(self, b, hz):
        if not self.present or self.gpio.get(self.CS, 1) != 0:
            return 0xFF
        self.bus_hz_seen.add(hz)
        if self.outq:
            o = self.outq.pop(0)
        elif self.stream is not None:
            if self.gap < 2:
                self.gap += 1
                o = 0xFF
            else:
                self.gap = 0
                blk = bytes(self.mem[self.stream * 512:(self.stream + 1) * 512])
                self.outq = [0xFE] + list(blk) + [0x12, 0x34]
                self.stream += 1
                o = self.outq.pop(0)
        else:
            o = 0xFF
        self.feed(b, hz)
        return o

    def feed(self, b, hz):
        if self.mode == "cmd":
            if not self.frame:
                if b & 0xC0 == 0x40:
                    self.frame.append(b)
                    if b & 0x3F == 12:       # a stop cancels the stream the moment its first byte arrives
                        self.stream = None
                        self.outq = []
                return
            self.frame.append(b)
            if len(self.frame) == 6:
                f, self.frame = bytes(self.frame), bytearray()
                self.run(f[0] & 0x3F, int.from_bytes(f[1:5], "big"), f[5], hz)
        elif self.mode == "wtoken":
            if b == (0xFC if self.multi_write else 0xFE):
                self.mode, self.wdata = "data", bytearray()
            elif b == 0xFD and self.multi_write:
                self.mode = "cmd"
                self.outq = [0xFF] + [0x00] * 3
            elif b != 0xFF:
                self.bad.append("unexpected byte 0x%02X waiting for a data token" % b)
        elif self.mode == "data":
            self.wdata.append(b)
            if len(self.wdata) == 514:
                if self.wlba >= self.sectors:
                    self.outq = [0x0D]
                else:
                    self.mem[self.wlba * 512:(self.wlba + 1) * 512] = self.wdata[:512]
                    self.outq = [0x05] + [0x00] * 4
                self.wlba += 1
                self.mode = "wtoken" if self.multi_write else "cmd"

    def run(self, cmd, arg, crc, hz):
        self.cmds.append((("A" if self.app else "") + str(cmd), arg))
        app, self.app = self.app, False
        if not self.inited and hz > 400000 and cmd != 0:
            self.bad.append("command %d at %d Hz before the card was started" % (cmd, hz))
        if cmd == 0:
            if crc != 0x95:
                self.bad.append("CMD0 with CRC 0x%02X" % crc)
            self.idle, self.inited = True, False
            self.outq = [0xFF, 0x01]
        elif cmd == 8:
            if not self.v2:
                self.outq = [0xFF, 0x05]
            else:
                if crc != 0x87:
                    self.bad.append("CMD8 with CRC 0x%02X" % crc)
                self.outq = [0xFF, 0x01, 0x00, 0x00, arg >> 8 & 0xF, arg & 0xFF]
        elif cmd == 55:
            self.app = True
            self.outq = [0xFF, 0x01 if self.idle else 0x00]
        elif cmd == 41 and app:
            if self.v2 and self.high and not arg & 0x40000000:
                self.bad.append("ACMD41 without the HCS bit on a version 2 card")
            self.acmd41_left -= 1
            if self.acmd41_left <= 0:
                self.idle, self.inited = False, True
                self.outq = [0xFF, 0x00]
            else:
                self.outq = [0xFF, 0x01]
        elif cmd == 58:
            self.outq = [0xFF, 0x00, 0xC0 if self.high else 0x80, 0xFF, 0x80, 0x00]
        elif cmd == 16:
            self.outq = [0xFF, 0x00 if arg == 512 else 0x40]
        elif cmd == 9:
            self.outq = [0xFF, 0x00, 0xFF, 0xFE] + list(self.csd()) + [0xAB, 0xCD]
        elif cmd in (17, 18):
            a = self.addr(arg)
            if not self.inited:
                self.outq = [0xFF, 0x05]
            elif a >= self.sectors:
                self.outq = [0xFF, 0x40]
            elif cmd == 17:
                self.outq = [0xFF, 0x00, 0xFF, 0xFE] + list(self.mem[a * 512:(a + 1) * 512]) + [0x12, 0x34]
            else:
                self.outq = [0xFF, 0x00]
                self.stream, self.gap = a, 0
        elif cmd == 12:
            self.outq = [0xFF, 0x00, 0x00, 0x00]
        elif cmd in (24, 25):
            a = self.addr(arg)
            if not self.inited or a >= self.sectors:
                self.outq = [0xFF, 0x40]
            else:
                self.outq = [0xFF, 0x00]
                self.mode, self.wlba, self.multi_write = "wtoken", a, cmd == 25
        else:
            self.outq = [0xFF, 0x05]

    def csd(self):
        c = bytearray(16)
        if self.high:                        # CSD version 2: C_SIZE = sectors / 1024 - 1
            cs = self.sectors // 1024 - 1
            c[0] = 0x40
            c[7], c[8], c[9] = cs >> 16 & 0x3F, cs >> 8 & 0xFF, cs & 0xFF
        else:                                # version 1: 512-byte blocks, READ_BL_LEN 9, C_SIZE_MULT 7 (x512), C_SIZE = sectors / 512 - 1
            cs = self.sectors // 512 - 1
            c[5] = 9
            c[6], c[7], c[8] = cs >> 10 & 3, cs >> 2 & 0xFF, (cs & 3) << 6
            c[9], c[10] = 3, 0x80
        return bytes(c)


@unittest.skipUnless(helpers.clang(), "needs a C compiler")
class SdSpi(unittest.TestCase):
    def load(self, **card):
        bus = FakeSdCard(**card)
        ld = dh.Loaded(os.path.join(DRIVERS, "sd_spi_module.c"), "sd_spi_entry", bus, dh.kv_cfg(cs=39, hz=20000000), compatible=b"sd-spi")
        self.assertEqual(ld.probe(), 0)
        self.addCleanup(ld.remove)
        return ld, bus, ld.ops(dh.BlockOps)

    def info(self, ld, ops):
        out = dh.BlockInfo()
        rc = ops.info(ld.handle, C.byref(out))
        return rc, out

    def read(self, ld, ops, lba, count):
        buf = C.create_string_buffer(512 * count)
        rc = ops.read(ld.handle, lba, C.addressof(buf), count)
        return rc, buf.raw

    def write(self, ld, ops, lba, data):
        buf = C.create_string_buffer(data, len(data))
        return ops.write(ld.handle, lba, C.addressof(buf), len(data) // 512)

    def test_it_is_a_block_driver_for_the_current_bus(self):
        ld, bus, ops = self.load()
        self.assertEqual(ld.drv.class_id, 6)
        self.assertEqual(ld.drv.bus_abi, 7)
        self.assertEqual(ld.drv.name, b"sd-spi")

    def test_the_slot_is_not_touched_until_someone_wants_the_card(self):
        ld, bus, ops = self.load()
        self.assertEqual(bus.cmds, [])
        self.assertEqual(bus.gpio[39], 1)                          # deselected from the start: the card must not hear the display
        self.assertEqual(set(bus.spi_cs_args), {-1})               # the driver runs its own chip select

    def test_an_sdhc_card_starts_and_reports_its_size(self):
        ld, bus, ops = self.load(sectors=8192)
        rc, i = self.info(ld, ops)
        self.assertEqual(rc, 0)
        self.assertEqual((i.sectors, i.sector_size, i.card, i.removable, i.writable), (8192, 512, 2, 1, 1))
        self.assertEqual([c for c, _ in bus.cmds][:5], ["0", "8", "55", "A41", "55"][:5])
        self.assertEqual(bus.bad, [])

    def test_a_card_is_started_slowly_then_run_fast(self):
        ld, bus, ops = self.load()
        self.info(ld, ops)
        self.read(ld, ops, 0, 1)
        self.assertEqual(bus.bad, [])
        self.assertIn(400000, bus.bus_hz_seen)
        self.assertIn(20000000, bus.bus_hz_seen)

    def test_an_old_sdsc_card_is_byte_addressed(self):
        ld, bus, ops = self.load(sectors=2048 * 8, high=False, v2=False)
        rc, i = self.info(ld, ops)
        self.assertEqual(rc, 0)
        self.assertEqual((i.sectors, i.card), (2048 * 8, 1))
        self.assertIn(("16", 512), bus.cmds)
        rc, data = self.read(ld, ops, 3, 1)
        self.assertEqual(rc, 0)
        self.assertEqual(data, bytes(bus.mem[3 * 512:4 * 512]))
        self.assertIn(("17", 3 * 512), bus.cmds)                   # a byte address, not a block number

    def test_single_and_multiple_block_reads(self):
        ld, bus, ops = self.load()
        rc, data = self.read(ld, ops, 7, 1)
        self.assertEqual((rc, data), (0, bytes(bus.mem[7 * 512:8 * 512])))
        rc, data = self.read(ld, ops, 100, 5)
        self.assertEqual((rc, data), (0, bytes(bus.mem[100 * 512:105 * 512])))
        self.assertIn(("18", 100), bus.cmds)
        self.assertIn(("12", 0), bus.cmds)
        self.assertEqual(bus.bad, [])
        rc, data = self.read(ld, ops, 4090, 6)                     # the last sectors
        self.assertEqual((rc, data), (0, bytes(bus.mem[4090 * 512:4096 * 512])))

    def test_reading_past_the_end_is_refused(self):
        ld, bus, ops = self.load()
        self.assertEqual(self.read(ld, ops, 4096, 1)[0], -1)
        self.assertEqual(self.read(ld, ops, 4095, 2)[0], -1)
        self.assertEqual(self.read(ld, ops, 0, 0)[0], -1)

    def test_single_and_multiple_block_writes(self):
        ld, bus, ops = self.load()
        one = bytes((i * 7) & 0xFF for i in range(512))
        self.assertEqual(self.write(ld, ops, 20, one), 0)
        self.assertEqual(bytes(bus.mem[20 * 512:21 * 512]), one)
        many = bytes((i * 13 + 5) & 0xFF for i in range(512 * 4))
        self.assertEqual(self.write(ld, ops, 50, many), 0)
        self.assertEqual(bytes(bus.mem[50 * 512:54 * 512]), many)
        self.assertEqual(bytes(bus.mem[54 * 512:55 * 512]), bytes(i % 251 for i in range(54 * 512, 55 * 512)))   # the next sector is untouched
        self.assertEqual(self.read(ld, ops, 50, 4), (0, many))
        self.assertEqual(bus.bad, [])
        self.assertEqual(ops.sync(ld.handle), 0)

    def test_writing_past_the_end_is_refused(self):
        ld, bus, ops = self.load()
        self.assertEqual(self.write(ld, ops, 4095, bytes(1024)), -1)

    def test_no_card_is_an_error_not_a_hang(self):
        ld, bus, ops = self.load(present=False)
        self.assertEqual(self.info(ld, ops)[0], -1)
        self.assertEqual(self.read(ld, ops, 0, 1)[0], -1)
        self.assertEqual(bus.gpio[39], 1)

    def test_a_card_put_in_later_is_found(self):
        ld, bus, ops = self.load(present=False)
        self.assertEqual(self.info(ld, ops)[0], -1)
        bus.present = True
        self.assertEqual(self.info(ld, ops)[0], 0)

    def test_a_card_taken_out_and_put_back_is_started_again(self):
        ld, bus, ops = self.load()
        self.assertEqual(self.read(ld, ops, 1, 1)[0], 0)
        bus.present = False
        self.assertEqual(self.read(ld, ops, 1, 1)[0], -1)
        bus.present = True
        bus.inited = False
        bus.acmd41_left = 2
        self.assertEqual(self.read(ld, ops, 1, 1)[0], 0)           # the failed call dropped the card; the next one restarts it

    def test_a_card_that_does_not_take_3v3_is_refused(self):
        bus = FakeSdCard()
        orig = bus.run

        def run(cmd, arg, crc, hz):
            orig(cmd, arg, crc, hz)
            if cmd == 8:
                bus.outq = [0xFF, 0x01, 0x00, 0x00, 0x00, 0x00]
        bus.run = run
        ld = dh.Loaded(os.path.join(DRIVERS, "sd_spi_module.c"), "sd_spi_entry", bus, dh.kv_cfg(cs=39), compatible=b"sd-spi")
        self.assertEqual(ld.probe(), 0)
        self.assertEqual(self.info(ld, ld.ops(dh.BlockOps))[0], -1)

    def test_the_bus_is_locked_for_every_conversation_and_always_released(self):
        ld, bus, ops = self.load()
        self.info(ld, ops)
        self.assertEqual(bus.lock_depth, 0)
        self.read(ld, ops, 0, 3)
        self.write(ld, ops, 9, bytes(512))
        self.read(ld, ops, 99999, 1)
        self.assertEqual(bus.lock_depth, 0)
        self.assertGreaterEqual(bus.lock_max, 1)
        bus.present = False
        self.read(ld, ops, 0, 1)
        self.assertEqual(bus.lock_depth, 0)

    def test_the_card_is_deselected_after_every_call(self):
        ld, bus, ops = self.load()
        for call in (lambda: self.info(ld, ops), lambda: self.read(ld, ops, 0, 2), lambda: self.write(ld, ops, 1, bytes(512)), lambda: ops.sync(ld.handle)):
            call()
            self.assertEqual(bus.gpio[39], 1)

    def test_probe_needs_a_chip_select(self):
        bus = FakeSdCard()
        ld = dh.Loaded(os.path.join(DRIVERS, "sd_spi_module.c"), "sd_spi_entry", bus, dh.kv_cfg(), compatible=b"sd-spi")
        self.assertEqual(ld.probe(), -1)
