#include "purr_drvcmds.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "purr_battery.h"
#include "purr_radio_bars.h"
#include "purr_driver.h"
#include "purr_driver_class.h"
#include "purr_sd.h"
#include "esp_timer.h"

static int no_driver(purr_cli_t *cli, const char *what, const char *compat)
{
    purr_cli_printf(cli, "%s: no driver is loaded for it.\n"
                         "  Put the \"%s\" driver on the device (Settings > Updates > Update drivers, or from your dev server), and make sure a device with\n"
                         "  compatible=%s is in /etc/board.conf (the T-Deck Plus has one for its GPS and its radio).\n", what, compat, compat);
    return 1;
}

static void print_deg(purr_cli_t *cli, int32_t e7, char pos, char neg)
{
    int64_t v = e7;
    char h = v < 0 ? neg : pos;
    if (v < 0) v = -v;
    purr_cli_printf(cli, "%d.%07d %c", (int)(v / 10000000), (int)(v % 10000000), h);
}

/* ---------------------------------------------------------------- gps */

static void print_fix(purr_cli_t *cli, const purr_gnss_fix_t *f)
{
    if (!f->valid) {
        purr_cli_printf(cli, "no fix yet (%d satellites in view). Take it outside, or near a window, and give it a minute or two.\n", f->sats_view);
        if (f->time_valid) {
            purr_cli_printf(cli, "  the satellites' clock says %04d-%02d-%02d %02d:%02d:%02d UTC\n", f->year, f->month, f->day, f->hour, f->minute, f->second);
        }
        return;
    }
    purr_cli_printf(cli, "position  ");
    print_deg(cli, f->lat_e7, 'N', 'S');
    purr_cli_printf(cli, ", ");
    print_deg(cli, f->lon_e7, 'E', 'W');
    purr_cli_printf(cli, "\naltitude  %d.%d m\n", (int)(f->alt_mm / 1000), (int)(abs(f->alt_mm % 1000) / 100));
    purr_cli_printf(cli, "speed     %u.%02u m/s, course %d.%02d deg\n", (unsigned)(f->speed_mmps / 1000), (unsigned)((f->speed_mmps % 1000) / 10),
                    (int)(f->course_cdeg / 100), (int)(abs(f->course_cdeg % 100)));
    purr_cli_printf(cli, "satellites %d used, %d in view, HDOP %d.%d\n", f->sats_used, f->sats_view, f->hdop_x10 / 10, f->hdop_x10 % 10);
    if (f->time_valid) {
        purr_cli_printf(cli, "time      %04d-%02d-%02d %02d:%02d:%02d UTC\n", f->year, f->month, f->day, f->hour, f->minute, f->second);
    }
}

int purr_gps_cmd(purr_cli_t *cli, int argc, char **argv)
{
    purr_class_entry_t e;
    if (purr_class_find(PURR_DRV_CLASS_GNSS, &e, 1) < 1) {
        return no_driver(cli, "gps", "gnss-nmea");
    }
    const purr_gnss_ops_t *ops = e.ops;
    const char *sub = argc >= 2 ? argv[1] : "fix";
    if (strcmp(sub, "power") == 0 && argc >= 3) {
        int on = strcmp(argv[2], "on") == 0;
        purr_cli_printf(cli, ops->power && ops->power(e.handle, on) == 0 ? "gps %s\n" : "this receiver does not switch (or %s failed)\n", on ? "on" : "off");
        return 0;
    }
    if (strcmp(sub, "rate") == 0 && argc >= 3) {
        purr_cli_printf(cli, ops->set_rate_hz && ops->set_rate_hz(e.handle, atoi(argv[2])) == 0 ? "asked for %s fixes a second\n" : "the receiver would not take %s\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "fix") == 0 || strcmp(sub, "watch") == 0) {
        int watch = sub[0] == 'w';
        for (int i = 0; i < (watch ? 15 : 1); i++) {
            purr_gnss_fix_t f;
            if (ops->get_fix(e.handle, &f) < 0) {
                purr_cli_printf(cli, "gps: the receiver did not answer (is it powered, and are TX and RX wired the right way round?)\n");
                return 1;
            }
            print_fix(cli, &f);
            if (watch) {
                purr_cli_printf(cli, "--\n");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        return 0;
    }
    purr_cli_printf(cli, "usage: gps [fix | watch | power on|off | rate <hz>]\n");
    return 1;
}

/* ---------------------------------------------------------------- radio */

static purr_lora_cfg_t s_lora = {.freq_hz = 868000000, .bw_hz = 125000, .sf = 7, .cr = 5, .tx_dbm = 14, .crc_on = 1, .preamble = 8, .sync_word = 0x1424};
static int s_lora_set;

int purr_radio_cmd(purr_cli_t *cli, int argc, char **argv)
{
    purr_class_entry_t e;
    if (purr_class_find(PURR_DRV_CLASS_LORA, &e, 1) < 1) {
        return no_driver(cli, "radio", "lora-sx126x");
    }
    const purr_lora_ops_t *ops = e.ops;
    const char *sub = argc >= 2 ? argv[1] : "";
    if (strcmp(sub, "config") == 0 && argc >= 3) {
        s_lora.freq_hz = (uint32_t)(strtod(argv[2], NULL) * 1e6 + 0.5);
        if (argc >= 4) s_lora.bw_hz = (uint32_t)(strtod(argv[3], NULL) * 1e3 + 0.5);
        if (argc >= 5) s_lora.sf = (uint8_t)atoi(argv[4]);
        if (argc >= 6) s_lora.cr = (uint8_t)atoi(argv[5]);
        if (argc >= 7) s_lora.tx_dbm = (int8_t)atoi(argv[6]);
        if (ops->configure(e.handle, &s_lora) != 0) {
            purr_cli_printf(cli, "radio: the radio would not take those settings (frequency 150-960 MHz, spreading factor 5-12, coding rate 5-8)\n");
            return 1;
        }
        s_lora_set = 1;
        purr_cli_printf(cli, "radio set to %u Hz, %u Hz wide, SF%d, CR 4/%d, %d dBm\n", (unsigned)s_lora.freq_hz, (unsigned)s_lora.bw_hz, s_lora.sf, s_lora.cr, s_lora.tx_dbm);
        return 0;
    }
    if (!s_lora_set && (strcmp(sub, "tx") == 0 || strcmp(sub, "rx") == 0)) {
        if (ops->configure(e.handle, &s_lora) != 0) {
            purr_cli_printf(cli, "radio: the radio did not answer. Check the wiring in /etc/board.conf (cs, busy, dio1, rst) and the TCXO setting (tcxo_mv).\n");
            return 1;
        }
        s_lora_set = 1;
        purr_cli_printf(cli, "(using the default 868 MHz, SF7, 125 kHz; change it with `radio config`)\n");
    }
    if (strcmp(sub, "tx") == 0 && argc >= 3) {
        size_t n = strlen(argv[2]);
        int rc = ops->send(e.handle, (const uint8_t *)argv[2], n, 5000);
        purr_cli_printf(cli, rc == 0 ? "sent %u bytes\n" : "radio: it did not go out (%u bytes)\n", (unsigned)n);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(sub, "rx") == 0) {
        int secs = argc >= 3 ? atoi(argv[2]) : 10;
        if (secs < 1) secs = 1;
        if (secs > 120) secs = 120;
        uint8_t buf[256];
        purr_lora_rxinfo_t info = {0};
        purr_cli_printf(cli, "listening for %d s...\n", secs);
        int n = ops->receive(e.handle, buf, sizeof(buf), (uint32_t)secs * 1000, &info);
        if (n < 0) {
            purr_cli_printf(cli, "radio: receive failed\n");
            return 1;
        }
        if (n == 0) {
            purr_cli_printf(cli, "nothing heard\n");
            return 0;
        }
        purr_cli_printf(cli, "%d bytes, RSSI %d dBm, SNR %d dB: ", n, info.rssi_dbm, info.snr_db);
        for (int i = 0; i < n; i++) purr_cli_printf(cli, "%c", isprint(buf[i]) ? buf[i] : '.');
        purr_cli_printf(cli, "\n");
        return 0;
    }
    if (strcmp(sub, "sleep") == 0) {
        purr_cli_printf(cli, ops->sleep(e.handle) == 0 ? "radio asleep\n" : "radio: could not put it to sleep\n");
        return 0;
    }
    if (strcmp(sub, "status") == 0) {                   /* what the status bar's service icon is built from */
        purr_cli_printf(cli, "radio     driver table version %u\n", (unsigned)ops->version);
        if (ops->version < 2 || ops->status == NULL) {
            purr_cli_printf(cli, "          this driver predates the status call: update the lora-sx126x driver (the service icon stays hidden until then)\n");
            return 0;
        }
        purr_lora_status_t st;
        if (ops->status(e.handle, &st) != 0) {
            purr_cli_printf(cli, "radio: the status call failed\n");
            return 1;
        }
        if (!st.heard) {
            purr_cli_printf(cli, "heard     nothing yet (the radio only listens during `radio rx` or when an app listens)\n");
        } else {
            purr_cli_printf(cli, "heard     %u packet(s); the last %u s ago at %d dBm, SNR %d dB\n", (unsigned)st.rx_count, (unsigned)(st.last_age_ms / 1000),
                            st.last_rssi_dbm, st.last_snr_db);
        }
        purr_cli_printf(cli, "bars      %d of 4\n", purr_radio_bars());
        return 0;
    }
    purr_cli_printf(cli, "usage: radio config <MHz> [<kHz bandwidth> <sf> <cr> <dBm>] | tx <text> | rx [seconds] | sleep | status\n"
                         "  Only use frequencies you are allowed to (868 MHz in Europe, 915 MHz in the Americas).\n");
    return 1;
}

/* ---------------------------------------------------------------- modem */

int purr_modem_cmd(purr_cli_t *cli, int argc, char **argv)
{
    purr_class_entry_t e;
    if (purr_class_find(PURR_DRV_CLASS_MODEM, &e, 1) < 1) {
        return no_driver(cli, "modem", "modem-at");
    }
    const purr_modem_ops_t *ops = e.ops;
    const char *sub = argc >= 2 ? argv[1] : "status";
    if (strcmp(sub, "power") == 0 && argc >= 3) {
        int on = strcmp(argv[2], "on") == 0;
        purr_cli_printf(cli, ops->power(e.handle, on) == 0 ? "modem %s\n" : "modem: it did not answer after power %s\n", on ? "on" : "off");
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        purr_modem_status_t st;
        if (ops->status(e.handle, &st) != 0) {
            purr_cli_printf(cli, "modem: it did not answer. Try `modem power on` first.\n");
            return 1;
        }
        static const char *tech[] = {"none", "GSM", "LTE", "LTE-M", "NB-IoT"};
        purr_cli_printf(cli, "network    %s%s\n", st.registered ? (st.operator_name[0] ? st.operator_name : "registered") : "not registered", st.roaming ? " (roaming)" : "");
        purr_cli_printf(cli, "technology %s\nsignal     %d dBm\nIMEI       %s\nsession    %s\n", st.tech < 5 ? tech[st.tech] : "?", st.rssi_dbm, st.imei[0] ? st.imei : "?",
                        st.data_up ? st.ip : "down");
        return 0;
    }
    if (strcmp(sub, "connect") == 0 && argc >= 3) {
        int rc = ops->connect(e.handle, argv[2], argc >= 4 ? argv[3] : NULL, argc >= 5 ? argv[4] : NULL);
        purr_cli_printf(cli, rc == 0 ? "data session up\n" : "modem: the network would not open a session (check the APN and that it is registered)\n");
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(sub, "disconnect") == 0) {
        purr_cli_printf(cli, ops->disconnect(e.handle) == 0 ? "session closed\n" : "modem: that did not work\n");
        return 0;
    }
    if (strcmp(sub, "at") == 0 && argc >= 3) {
        char resp[256];
        int n = ops->at(e.handle, argv[2], resp, sizeof(resp), 5000);
        purr_cli_printf(cli, n >= 0 ? "%s\n" : "(no answer)\n", resp);
        return n >= 0 ? 0 : 1;
    }
    purr_cli_printf(cli, "usage: modem [status | power on|off | connect <apn> [user pass] | disconnect | at <command>]\n");
    return 1;
}

/* ---------------------------------------------------------------- sd */

static void sd_ls_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    purr_cli_t *cli = ctx;
    if (is_dir) {
        purr_cli_printf(cli, "  %s/\n", name);
    } else {
        purr_cli_printf(cli, "  %-32s %u\n", name, (unsigned)size);
    }
}

static const char *card_name(uint8_t card)
{
    return card == PURR_BLOCK_CARD_SDHC ? "SDHC/SDXC (block addressed)" : card == PURR_BLOCK_CARD_SDSC ? "SD (byte addressed)" : "unknown";
}

typedef struct {
    uint32_t pos;
    int bad;
} sd_verify_t;

static int sd_verify_cb(void *ctx, const void *data, uint32_t len)
{
    sd_verify_t *v = ctx;
    const uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++, v->pos++) {
        if (p[i] != (uint8_t)(v->pos * 31u + (v->pos >> 8))) {
            v->bad++;
        }
    }
    return 0;
}

int purr_sd_cmd(purr_cli_t *cli, int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    char why[100];
    if (strcmp(sub, "status") == 0) {
        purr_sd_status_t st;
        purr_sd_status(&st);
        purr_cli_printf(cli, "driver   %s\n", st.driver ? "loaded (sd-spi)" : "not loaded");
        if (!st.mounted) {
            purr_cli_printf(cli, "card     not mounted: %s\n", st.why[0] ? st.why : "no reason given");
            return 1;
        }
        purr_cli_printf(cli, "card     %s, %u sectors (%u MB)\n", card_name(st.card), (unsigned)st.sectors, (unsigned)(st.sectors / 2048));
        purr_cli_printf(cli, "mounted  /sd, %u MB of %u MB free\n", (unsigned)(st.free_bytes >> 20), (unsigned)(st.total_bytes >> 20));
        return 0;
    }
    if (strcmp(sub, "mount") == 0) {
        if (purr_sd_mount(why, sizeof(why)) != 0) {
            purr_cli_printf(cli, "sd: %s\n", why);
            return 1;
        }
        purr_cli_printf(cli, "mounted at /sd\n");
        return 0;
    }
    if (strcmp(sub, "unmount") == 0 || strcmp(sub, "eject") == 0) {
        purr_sd_unmount();
        purr_cli_printf(cli, "unmounted; it is safe to take the card out\n");
        return 0;
    }
    if (strcmp(sub, "ls") == 0) {
        const char *path = argc >= 3 ? argv[2] : "/sd";
        if (!purr_sd_is_path(path) || purr_sd_list(path, sd_ls_cb, cli) != 0) {
            purr_cli_printf(cli, "sd ls: cannot list %s (is the card mounted? try: sd)\n", path);
            return 1;
        }
        return 0;
    }
    if (strcmp(sub, "test") == 0) {
        /* writes 64 KB to /sd/purr-test.bin, reads it back and compares it, and removes it: the quickest proof that the driver, the bus and the file system agree */
        enum { TOTAL = 65536, PIECE = 4096 };
        static uint8_t buf[PIECE];
        if (!purr_sd_present()) {
            purr_sd_status_t st;
            purr_sd_status(&st);
            purr_cli_printf(cli, "sd test: %s\n", st.why[0] ? st.why : "no card");
            return 1;
        }
        const char *path = "/sd/purr-test.bin";
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            purr_cli_printf(cli, "sd test: could not create %s\n", path);
            return 1;
        }
        int64_t t0 = esp_timer_get_time();
        int ok = 1;
        for (uint32_t pos = 0; pos < TOTAL && ok; pos += PIECE) {
            for (uint32_t i = 0; i < PIECE; i++) {
                buf[i] = (uint8_t)((pos + i) * 31u + ((pos + i) >> 8));
            }
            ok = fwrite(buf, 1, PIECE, f) == PIECE;
        }
        ok = (fclose(f) == 0) && ok;
        int64_t t1 = esp_timer_get_time();
        if (!ok) {
            purr_cli_printf(cli, "sd test: the write failed\n");
            purr_sd_remove(path);
            return 1;
        }
        sd_verify_t v = {0, 0};
        int rc = purr_sd_read(path, sd_verify_cb, &v);
        int64_t t2 = esp_timer_get_time();
        purr_sd_remove(path);
        if (rc != 0 || v.pos != TOTAL || v.bad != 0) {
            purr_cli_printf(cli, "sd test: FAILED (read rc=%d, %u bytes back, %d wrong)\n", rc, (unsigned)v.pos, v.bad);
            return 1;
        }
        purr_cli_printf(cli, "sd test: ok. wrote 64 KB in %u ms, read it back in %u ms, all bytes right\n", (unsigned)((t1 - t0) / 1000), (unsigned)((t2 - t1) / 1000));
        return 0;
    }
    purr_cli_printf(cli, "usage: sd [status | mount | unmount | ls [path] | test]\n");
    return 1;
}

/* `battery`: the gauge's raw four bytes and what the OS makes of them. Run it plugged in and unplugged to see what the cell line does on USB power
 * (the first gauge treated anything over 4.4 V as "no battery"). */
int purr_battery_cmd(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t b[4] = {0};
    int n = purr_device_read("battery", b, sizeof(b));
    if (n != 4) {
        purr_cli_printf(cli, "battery: no reading from the driver (%d); is the battery-adc driver loaded?\n", n);
        return 1;
    }
    purr_cli_printf(cli, "raw       %u%% at %u mV, flags 0x%02x (cell %s, %s)\n", b[0], (unsigned)(b[1] | (b[2] << 8)), b[3], (b[3] & 1) ? "present" : "not seen",
                    (b[3] & 2) ? "line held high: charging or USB power" : "line at rest level");
    int pct = 0, mv = 0;
    if (purr_battery_get(&pct, &mv)) {
        purr_cli_printf(cli, "the OS     %d%% at %d mV%s\n", pct, mv, purr_battery_charging() ? ", charging" : "");
    } else {
        purr_cli_printf(cli, "the OS     no battery reading\n");
    }
    return 0;
}
