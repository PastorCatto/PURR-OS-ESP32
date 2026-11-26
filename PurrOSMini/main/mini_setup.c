/* mini_setup.c - see mini_setup.h. */
#include "mini_setup.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mini_link.h"
#include "mini_px.h"
#include "mini_ui.h"
#include "mini_wifi.h"
#include "purr_px_pair.h"
#include "purr_px_setup.h"

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* the device's answer to what was just sent: its status, or -1 if it did not answer in time */
static int wait_ack(const uint8_t id[8], int type)
{
    uint32_t t0 = ms_now();
    while (ms_now() - t0 < 8000) {
        uint8_t ch, buf[16];
        int n = mini_px_recv(id, &ch, buf, sizeof(buf));
        int at, st;
        if (n > 0 && ch == PURR_PX_CH_SETUP && purr_pxs_parse_ack(buf, (size_t)n, &at, &st) == 0 && at == type) {
            return st;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return -1;
}

/* sends one message and says what came of it; true if the device took it */
static bool send_step(const uint8_t id[8], const uint8_t *m, int len, int type, const char *what)
{
    if (len < 0 || mini_px_send(id, PURR_PX_CH_SETUP, m, (size_t)len) != 0) {
        mini_ui_status("Remote setup", "Could not send", what);
        vTaskDelay(pdMS_TO_TICKS(1800));
        return false;
    }
    int st = wait_ack(id, type);
    if (st == PURR_PXS_OK) {
        return true;
    }
    mini_ui_status("Remote setup", st < 0 ? "No answer" : (st == PURR_PXS_REFUSED ? "The device refused" : "The device could not do it"), what);
    vTaskDelay(pdMS_TO_TICKS(2200));
    return false;
}

static bool ask_text(const char *title, const char *prompt, char *out, int cap, bool secret)
{
    return mini_ui_text(title, prompt, out, cap, secret);
}

/* the steps, for one device that is paired and connected */
static void wizard(const uint8_t id[8], const char *device)
{
    unsigned need = PURR_PXS_NEED_ACCOUNT | PURR_PXS_NEED_WIFI | PURR_PXS_NEED_NAME;      /* assumed until the device says */
    uint32_t t0 = ms_now();
    while (ms_now() - t0 < 4000) {                                                        /* its own list of what is missing, if it sends one */
        uint8_t ch, buf[40];
        int n = mini_px_recv(id, &ch, buf, sizeof(buf));
        char nm[PURR_PXS_MAX_NAME + 1];
        unsigned f;
        if (n > 0 && ch == PURR_PX_CH_SETUP && purr_pxs_parse_needs(buf, (size_t)n, &f, nm) == 0) {
            need = f;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    uint8_t m[PURR_PXS_MAX_MSG + 8];
    char a[PURR_PXS_MAX_PASS + 1], b[PURR_PXS_MAX_PASS + 1];

    if (need & PURR_PXS_NEED_NAME) {
        snprintf(a, sizeof(a), "%s", device);
        if (!ask_text("Remote setup", "A name for this device", a, PURR_PXS_MAX_NAME + 1, false)) {
            return;
        }
        if (a[0] != '\0' && !send_step(id, m, purr_pxs_make_name(a, m, sizeof(m)), PURR_PXS_SET_NAME, "the name")) {
            return;
        }
    }
    if (need & PURR_PXS_NEED_ACCOUNT) {
        for (;;) {
            a[0] = b[0] = '\0';
            if (!ask_text("Remote setup", "User name for the first account", a, PURR_PXS_MAX_USER + 1, false) || a[0] == '\0') {
                return;
            }
            char p1[PURR_PXS_MAX_PASS + 1], p2[PURR_PXS_MAX_PASS + 1];
            if (!ask_text("Remote setup", "Password", p1, PURR_PXS_MAX_PASS + 1, true) || p1[0] == '\0') {
                return;
            }
            if (!ask_text("Remote setup", "Password again", p2, PURR_PXS_MAX_PASS + 1, true)) {
                return;
            }
            if (strcmp(p1, p2) != 0) {
                mini_ui_status("Remote setup", "The passwords differ", "try again");
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }
            snprintf(b, sizeof(b), "%s", p1);
            memset(p1, 0, sizeof(p1));
            memset(p2, 0, sizeof(p2));
            break;
        }
        bool ok = send_step(id, m, purr_pxs_make_account(a, b, m, sizeof(m)), PURR_PXS_SET_ACCT, "the account");
        memset(b, 0, sizeof(b));
        memset(m, 0, sizeof(m));
        if (!ok) {
            return;
        }
    }
    if (need & PURR_PXS_NEED_WIFI) {
        char saved[33], spass[65];
        bool have = mini_wifi_saved(saved, sizeof(saved), spass, sizeof(spass));
        int use_own = have ? mini_ui_choice("Remote setup", "Wi-Fi for the device", saved, "Use this one", "Choose another") : 0;
        if (use_own) {
            send_step(id, m, purr_pxs_make_wifi(saved, spass, m, sizeof(m)), PURR_PXS_SET_WIFI, "the Wi-Fi network");
            memset(spass, 0, sizeof(spass));
            memset(m, 0, sizeof(m));
        } else {
            mini_ap_t ap;
            if (mini_ui_pick_network(&ap)) {
                char pw[65] = "";
                if (!ap.secure || mini_ui_password(ap.ssid, pw, sizeof(pw))) {
                    send_step(id, m, purr_pxs_make_wifi(ap.ssid, pw, m, sizeof(m)), PURR_PXS_SET_WIFI, "the Wi-Fi network");
                    memset(pw, 0, sizeof(pw));
                    memset(m, 0, sizeof(m));
                }
            }
        }
    }
    if (mini_ui_choice("Remote setup", "Put Meshtastic on it?", "it installs when it has the network", "Yes", "No")) {
        send_step(id, m, purr_pxs_make_install("meshtastic", m, sizeof(m)), PURR_PXS_INSTALL, "Meshtastic");
    }
    if (mini_ui_choice("Remote setup", "Finish and restart the device?", "setup closes afterwards", "Finish", "Not yet")) {
        if (send_step(id, m, purr_pxs_make_done(m, sizeof(m)), PURR_PXS_DONE, "finishing")) {
            mini_ui_status("Remote setup", "Done", "the device is restarting");
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
}

void mini_setup_run(void)
{
    for (;;) {
        purr_link_peer_t nodes[7];
        int n = mini_px_setup_nodes(nodes, 7);
        char names[7][20];
        for (int i = 0; i < n; i++) {
            snprintf(names[i], sizeof(names[i]), "%.19s", nodes[i].name);
        }
        int pick = mini_ui_setup_nodes(names, n, 1200);
        if (pick == -1) {
            return;
        }
        if (pick < 0) {
            continue;
        }
        purr_link_peer_t p = nodes[pick];
        if (!mini_px_is_paired(p.id) && !mini_link_pair(&p)) {
            continue;
        }
        mini_ui_status("Remote setup", "Connecting", names[pick]);
        mini_px_connect(p.id);
        uint32_t t0 = ms_now();
        while (!mini_px_session_up(p.id) && ms_now() - t0 < 8000) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!mini_px_session_up(p.id)) {
            mini_ui_status("Remote setup", "Could not connect", "move closer and try again");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        wizard(p.id, names[pick]);
    }
}
