#include "purr_drvcmds.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "purr_driver.h"
#include "purr_driver_class.h"

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
    purr_cli_printf(cli, "usage: radio config <MHz> [<kHz bandwidth> <sf> <cr> <dBm>] | tx <text> | rx [seconds] | sleep\n"
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
