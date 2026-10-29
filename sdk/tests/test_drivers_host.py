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
