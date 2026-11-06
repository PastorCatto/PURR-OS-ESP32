/* oobe_firstboot.c - see oobe_firstboot.h. */
#include "oobe_firstboot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "commands.h"
#include "login.h"
#include "oobe_job.h"
#include "oobe_ui.h"
#include "purr_cli.h"
#include "purr_console.h"
#include "purr_fetch.h"
#include "purr_net.h"
#include "purr_netrec.h"
#include "purr_oobe.h"
#include "purr_recovery_src.h"
#include "purr_w10m_theme.h"

#if CONFIG_PURR_PROFILE_RECOVERY && (CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS)

static const char *TAG = "oobe-first";

static oobe_job_t *s_acct_job, *s_stage2_job, *s_net_job;
static int s_stage2_started;
static volatile int s_need_wifi;                       /* stage 2 could not get online: the wizard asks for a network (op_need_wifi) */
static int s_retry_after_connect;
static int s_defer;                                    /* the Froyo first run: the install waits for the Installing page, after the legal page has tested the connection */
static int s_install_wanted;                           /* a recovery install is waiting to be started */
static char s_name[40], s_pass[72];
static char *s_legal_text;                             /* fetched by the stage 2 job while it has the network; the legal page shows it */
#define LEGAL_MAX (16 * 1024)

/* ---- the account (hashing the password takes a moment, so it is a job) */

static int job_account(oobe_job_t *job, void *arg)
{
    (void)arg;
    purr_acct_result_t r = purr_login_account_add_first(s_name, s_pass);
    memset(s_pass, 0, sizeof(s_pass));
    if (r != PURR_ACCT_OK) {
        oobe_job_report(job, purr_login_account_error(r), -1);
        return 1;
    }
    return 0;
}

/* ---- stage 2: the rest of the system, in the background */

static void quiet_put(void *ctx, char c) { (void)ctx; (void)c; }       /* the wizard shows its own words, not the installer's */

static int s_step, s_downloads;

static void stage2_progress(size_t got, size_t total)
{
    if (total > 0 && got == total) {
        s_downloads++;                                  /* one more download finished (the modules are fetched one by one) */
    }
    int per;
    if (s_step == 2) {
        per = 960;                                                       /* the bootloader: a few KB, no bar of its own */
    } else if (s_step == 0) {
        per = total > 0 ? (int)((uint64_t)got * 450 / total) : 0;       /* the kernel: the first half */
    } else {
        per = 500 + (s_downloads < 8 ? s_downloads * 60 : 480);          /* the modules: the second half, a step per module */
    }
    oobe_job_report(s_stage2_job, NULL, per);
}

static int fetch_legal(void)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    free(s_legal_text);                                 /* a second run (after the Wi-Fi page) replaces the first */
    s_legal_text = NULL;
    if (purr_fetch_alloc(purr_recovery_legal_url(), &buf, &len, LEGAL_MAX) != ESP_OK) {
        ESP_LOGW(TAG, "legal text not loaded");         /* the page says so and points to Settings; the install carries on */
        return 0;
    }
    char *t = malloc(len + 1);
    if (t != NULL) {
        memcpy(t, buf, len);
        t[len] = '\0';
        s_legal_text = t;
    }
    free(buf);
    return t != NULL;
}

/* The legal page is the connection test (Froyo first run): is the network up, and does the server answer? The text is what it fetches, so a page that shows means a
 * connection that works, and one that does not says so before the install is started on it. */
static int job_legal_test(oobe_job_t *job, void *arg)
{
    (void)arg;
    oobe_job_report(job, "Checking the connection...", -1);
    purr_net_status_t st;
    purr_net_status(&st);
    if (purr_net_wired() ? 0 : (!st.connected || strcmp(st.ip, "0.0.0.0") == 0)) {
        char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
        if (purr_netrec_load(ssid, pass) != 0 || purr_net_connect(ssid, pass, 15000) != ESP_OK) {
            oobe_job_report(job, "Not connected to Wi-Fi.", -1);
            return 1;
        }
    }
    if (!fetch_legal()) {
        char why[96];
        snprintf(why, sizeof(why), "The server did not answer%s%.40s%s.", purr_fetch_last_error()[0] ? " (" : "", purr_fetch_last_error(), purr_fetch_last_error()[0] ? ")" : "");
        oobe_job_report(job, why, -1);
        return 1;
    }
    return 0;
}

static int job_stage2(oobe_job_t *job, void *arg)
{
    (void)arg;
    oobe_job_report(job, "Getting back online...", 0);
    if (purr_net_wired()) {
        if (purr_net_connect("", "", 30000) != ESP_OK) {
            s_need_wifi = 1;
            oobe_job_abort(job, "No network, so the rest of the system wasn't installed.");
        }
    } else {
        char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
        if (purr_netrec_load(ssid, pass) != 0 || purr_net_connect(ssid, pass, 15000) != ESP_OK) {
            s_need_wifi = 1;                            /* the wizard puts its Wi-Fi page next; a network that connects starts this job again */
            oobe_job_abort(job, "Couldn't get back on Wi-Fi, so the rest of the system wasn't installed.");
        }
    }
    s_need_wifi = 0;
    if (s_legal_text == NULL) {                         /* the legal page may have fetched it already */
        fetch_legal();
    }
    static purr_cli_t cli;
    purr_cli_init(&cli, NULL, 0, quiet_put, NULL, "");
    purr_fetch_set_progress(stage2_progress);
    s_step = 0;
    s_downloads = 0;
    if (!purr_kernel_installed()) {                     /* the loader normally put it in; only an older loader leaves it to us */
        oobe_job_report(job, "Installing the kernel...", 0);
        if (purr_stage2_step(&cli, 0) != 0) {
            purr_fetch_set_progress(NULL);
            oobe_job_abort(job, "The kernel didn't install. Power it off and on to try again.");
        }
    }
    s_step = 1;
    oobe_job_report(job, "Installing the modules...", 500);
    if (purr_stage2_step(&cli, 1) != 0) {
        static char why[200];
        snprintf(why, sizeof(why), "Some modules didn't install%s%.60s%s. Retry, or add them later from Settings.", purr_fetch_last_error()[0] ? " (" : "",
                 purr_fetch_last_error(), purr_fetch_last_error()[0] ? ")" : "");
        purr_fetch_set_progress(NULL);
        oobe_job_abort(job, why);
    }
    oobe_job_report(job, "Installing the pictures...", 800);
    purr_stage2_step(&cli, 3);                          /* the shell's picture pack, if the release carries one; best effort, the words show without it */
    if (purr_login_has_accounts()) {                    /* only where nobody is typing: a restart in the middle of setting up an account would lose it */
        s_step = 2;
        oobe_job_report(job, "Checking the bootloader...", 960);
        purr_stage2_step(&cli, 2);                      /* a newer bootloader is saved with a backup and swapped in; this restarts the device when it does */
    }
    purr_fetch_set_progress(NULL);
    oobe_job_report(job, "System installed", 1000);
    return 0;
}

/* ---- Wi-Fi, for when stage 2 could not get online (the same two jobs as the recovery loader's wizard) */

#define WIZ_MAX_AP 8
static purr_oobe_ap_t s_aps[WIZ_MAX_AP];
static int s_nap;
static char s_arg_a[64], s_arg_b[72];

static int job_scan(oobe_job_t *job, void *arg)
{
    (void)arg;
    purr_net_ap_t aps[WIZ_MAX_AP];
    int n = 0;
    oobe_job_report(job, "Looking for networks...", -1);
    if (purr_net_scan(aps, WIZ_MAX_AP, &n) != ESP_OK) {
        oobe_job_report(job, "Couldn't scan for networks.", -1);
        return 1;
    }
    s_nap = 0;
    for (int i = 0; i < n && s_nap < WIZ_MAX_AP; i++) {
        snprintf(s_aps[s_nap].ssid, sizeof(s_aps[0].ssid), "%s", aps[i].ssid);
        s_aps[s_nap].rssi = aps[i].rssi;
        s_aps[s_nap].secure = !aps[i].open;
        s_nap++;
    }
    return 0;
}

static int job_connect(oobe_job_t *job, void *arg)
{
    (void)arg;
    if (purr_net_wired()) {
        oobe_job_report(job, "Waiting for the wired link and an address...", -1);
        if (purr_net_connect("", "", 30000) != ESP_OK) {
            oobe_job_report(job, "No wired link or address yet.", -1);
            return 1;
        }
        return 0;
    }
    char pass[PURR_NETREC_PASS_LEN];
    snprintf(pass, sizeof(pass), "%.*s", (int)sizeof(pass) - 1, s_arg_b);
    if (pass[0] == '\0') {                              /* a remembered network connects without typing the password again */
        char rs[PURR_NETREC_SSID_LEN], rp[PURR_NETREC_PASS_LEN];
        if (purr_netrec_load(rs, rp) == 0 && strcmp(rs, s_arg_a) == 0) {
            snprintf(pass, sizeof(pass), "%.*s", (int)sizeof(pass) - 1, rp);
        }
    }
    oobe_job_report(job, "Connecting...", -1);
    if (purr_net_connect(s_arg_a, pass, 15000) != ESP_OK) {
        oobe_job_report(job, "Couldn't connect. Check the password and try again.", -1);
        return 1;
    }
    purr_netrec_save(s_arg_a, pass);                     /* remembered, so the next start does not ask again */
    return 0;
}

/* ---- the wizard's operations */

static int op_job_start(purr_oobe_job_t kind, const char *a, const char *b)
{
    if (kind == PURR_OOBE_JOB_LEGAL) {
        if (s_defer) {                                  /* the Froyo first run: a job of its own, the connection test */
            return oobe_job_start_stack(s_net_job, job_legal_test, NULL, OOBE_STACK_NET);
        }
        return s_stage2_started ? 0 : -1;               /* otherwise the stage 2 job fetches the text */
    }
    if (kind == PURR_OOBE_JOB_SCAN || kind == PURR_OOBE_JOB_CONNECT) {
        snprintf(s_arg_a, sizeof(s_arg_a), "%s", a != NULL ? a : "");
        snprintf(s_arg_b, sizeof(s_arg_b), "%s", b != NULL ? b : "");
        s_retry_after_connect = kind == PURR_OOBE_JOB_CONNECT;
        return oobe_job_start_stack(s_net_job, kind == PURR_OOBE_JOB_SCAN ? job_scan : job_connect, NULL, OOBE_STACK_NET);
    }
    if (kind != PURR_OOBE_JOB_ACCOUNT) {
        return -1;
    }
    snprintf(s_name, sizeof(s_name), "%s", a != NULL ? a : "");
    snprintf(s_pass, sizeof(s_pass), "%s", b != NULL ? b : "");
    return oobe_job_start_stack(s_acct_job, job_account, NULL, OOBE_STACK_SMALL);
}

static int op_job_poll(purr_oobe_job_t kind, char *msg, size_t cap, int *per_mille)
{
    if (kind == PURR_OOBE_JOB_LEGAL && s_defer) {
        return oobe_job_poll(s_net_job, msg, cap, per_mille);
    }
    if (kind == PURR_OOBE_JOB_LEGAL) {
        if (s_legal_text != NULL) {
            return 0;
        }
        return oobe_job_poll(s_stage2_job, NULL, 0, NULL) < 0 ? -1 : 1;     /* still working, or it ended without the text */
    }
    if (kind == PURR_OOBE_JOB_SCAN || kind == PURR_OOBE_JOB_CONNECT) {
        int r = oobe_job_poll(s_net_job, msg, cap, per_mille);
        if (kind == PURR_OOBE_JOB_CONNECT && r == 0 && s_retry_after_connect) {
            s_retry_after_connect = 0;
            if (s_need_wifi) {                                  /* online now: the install that stopped for want of a network starts again; one that is running is left alone */
                s_need_wifi = 0;
                if (oobe_job_start(s_stage2_job, job_stage2, NULL) == 0) {
                    s_stage2_started = 1;
                }
                ESP_LOGI(TAG, "stage 2 install %s after Wi-Fi", s_stage2_started ? "started again" : "could not start again");
            }
        }
        return r;
    }
    return oobe_job_poll(s_acct_job, msg, cap, per_mille);
}

static int op_scan_results(purr_oobe_ap_t *out, int max)
{
    int n = s_nap < max ? s_nap : max;
    memcpy(out, s_aps, (size_t)n * sizeof(out[0]));
    return n;
}

static int op_wired(void) { return purr_net_wired(); }
static int op_need_wifi(void) { return s_need_wifi; }

/* The Installing page was reached: the install that was held back starts now, on a connection the legal page has just tested. */
static int op_start_install(void)
{
    if (s_install_wanted) {
        s_install_wanted = 0;
        if (oobe_job_start(s_stage2_job, job_stage2, NULL) == 0) {
            s_stage2_started = 1;
        }
        ESP_LOGI(TAG, "stage 2 install %s on the Installing page", s_stage2_started ? "started" : "could not start");
    }
    return s_stage2_started;
}

/* The Retry button: the install stopped, so it can start again; one that is still running is left alone. */
static void op_retry(void)
{
    if (oobe_job_poll(s_stage2_job, NULL, 0, NULL) < 0) {
        return;
    }
    s_need_wifi = 0;
    if (oobe_job_start(s_stage2_job, job_stage2, NULL) == 0) {
        s_stage2_started = 1;
    }
    ESP_LOGI(TAG, "stage 2 install %s on request", s_stage2_started ? "started again" : "could not start again");
}

static const char *op_remembered(void)
{
    static char ssid[PURR_NETREC_SSID_LEN];
    char pass[PURR_NETREC_PASS_LEN];
    return purr_netrec_load(ssid, pass) == 0 ? ssid : NULL;
}

static const char *op_legal(void) { return s_legal_text; }

/* -1 nothing to install, 0..999 working, 1000 done, -2 failed. */
static int op_background(char *msg, size_t cap)
{
    if (!s_stage2_started) {
        return -1;
    }
    int per = -1;
    int r = oobe_job_poll(s_stage2_job, msg, cap, &per);
    if (r < 0) {
        return per < 0 ? 0 : (per > 999 ? 999 : per);
    }
    return r == 0 ? 1000 : -2;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();                                      /* the bootloader now prefers the kernel slot: PURR OS starts */
}

static void op_finish(void)
{
    xTaskCreatePinnedToCore(restart_task, "restart", 2048, NULL, 3, NULL, 0);   /* core 0: see park_app_cpu() in the bootloader */
}

static const purr_oobe_ops_t k_ops = {
    .job_start = op_job_start, .job_poll = op_job_poll, .legal = op_legal, .background = op_background, .finish = op_finish,
    .scan_results = op_scan_results, .wired = op_wired, .remembered_ssid = op_remembered, .need_wifi = op_need_wifi, .retry = op_retry, .start_install = op_start_install,
};

int oobe_firstboot_available(void)
{
    /* only right after the loader restored us: any other recovery boot gets the recovery app */
    return oobe_ui_available() && !purr_login_has_accounts() && purr_stage2_pending();
}

int oobe_firstboot_finishing_available(void)
{
    return oobe_ui_available() && purr_login_has_accounts() && purr_stage2_pending();
}

void oobe_firstboot_run(void)
{
    s_acct_job = oobe_job_new();
    s_stage2_job = oobe_job_new();
    s_net_job = oobe_job_new();
    purr_console_set_muted(1);                          /* the wizard draws the screen; the log stays in the serial log */
    s_defer = purr_theme_skin() == PURR_SKIN_ANDROID && !purr_login_has_accounts();
    if (purr_stage2_take_pending()) {                   /* the loader restored us */
        if (s_defer) {
            s_install_wanted = 1;                       /* the Froyo first run installs from its Installing page, after the legal page has tested the connection */
        } else {
            s_stage2_started = oobe_job_start(s_stage2_job, job_stage2, NULL) == 0;      /* carry on installing while they set up */
            ESP_LOGI(TAG, "stage 2 install %s", s_stage2_started ? "started in the background" : "could not start");
        }
    }
    /* The wizard is up: this start is healthy. Until now it was only marked at the login prompt, which the wizard never reaches (its last page restarts), so a person who
     * restarted the device during setup was counted as a failed start, and a couple of those sent the loader to reinstall the whole device unattended (2026-11-05: a
     * second install, and an administrator called "recovery" with a password nobody saw). */
    {
        static purr_cli_t healthy_cli;
        purr_cli_init(&healthy_cli, NULL, 0, quiet_put, NULL, "");
        purr_mark_boot_healthy(&healthy_cli);
    }
    oobe_ui_run(purr_login_has_accounts() ? PURR_OOBE_PART_FINISHING : PURR_OOBE_PART_FIRSTBOOT, &k_ops, 0);
    purr_console_set_muted(0);                          /* LVGL did not start: the caller carries on with the text setup */
}

#else

int oobe_firstboot_available(void) { return 0; }
int oobe_firstboot_finishing_available(void) { return 0; }
void oobe_firstboot_run(void) {}

#endif
