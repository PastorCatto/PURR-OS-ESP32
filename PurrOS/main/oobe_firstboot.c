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

#if CONFIG_PURR_PROFILE_RECOVERY && (CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS)

static const char *TAG = "oobe-first";

static oobe_job_t *s_acct_job, *s_stage2_job;
static int s_stage2_started;
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

static void fetch_legal(void)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (purr_fetch_alloc(purr_recovery_legal_url(), &buf, &len, LEGAL_MAX) != ESP_OK) {
        ESP_LOGW(TAG, "legal text not loaded");         /* the page says so and points to Settings; the install carries on */
        return;
    }
    char *t = malloc(len + 1);
    if (t != NULL) {
        memcpy(t, buf, len);
        t[len] = '\0';
        s_legal_text = t;
    }
    free(buf);
}

static int job_stage2(oobe_job_t *job, void *arg)
{
    (void)arg;
    oobe_job_report(job, "Getting back online...", 0);
    if (purr_net_wired()) {
        if (purr_net_connect("", "", 30000) != ESP_OK) {
            oobe_job_abort(job, "No network, so the rest of the system wasn't installed.");
        }
    } else {
        char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
        if (purr_netrec_load(ssid, pass) != 0 || purr_net_connect(ssid, pass, 15000) != ESP_OK) {
            oobe_job_abort(job, "Couldn't get back on Wi-Fi, so the rest of the system wasn't installed.");
        }
    }
    fetch_legal();
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
        purr_fetch_set_progress(NULL);
        oobe_job_abort(job, "Some modules didn't install. You can add them later from Settings.");
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

/* ---- the wizard's operations */

static int op_job_start(purr_oobe_job_t kind, const char *a, const char *b)
{
    if (kind == PURR_OOBE_JOB_LEGAL) {                  /* no job of its own: the stage 2 job fetches the text */
        return s_stage2_started ? 0 : -1;
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
    if (kind == PURR_OOBE_JOB_LEGAL) {
        if (s_legal_text != NULL) {
            return 0;
        }
        return oobe_job_poll(s_stage2_job, NULL, 0, NULL) < 0 ? -1 : 1;     /* still working, or it ended without the text */
    }
    return oobe_job_poll(s_acct_job, msg, cap, per_mille);
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
    purr_console_set_muted(1);                          /* the wizard draws the screen; the log stays in the serial log */
    if (purr_stage2_take_pending()) {                   /* the loader restored us: carry on installing while they set up */
        s_stage2_started = oobe_job_start(s_stage2_job, job_stage2, NULL) == 0;
        ESP_LOGI(TAG, "stage 2 install %s", s_stage2_started ? "started in the background" : "could not start");
    }
    oobe_ui_run(purr_login_has_accounts() ? PURR_OOBE_PART_FINISHING : PURR_OOBE_PART_FIRSTBOOT, &k_ops, 0);
    purr_console_set_muted(0);                          /* LVGL did not start: the caller carries on with the text setup */
}

#else

int oobe_firstboot_available(void) { return 0; }
int oobe_firstboot_finishing_available(void) { return 0; }
void oobe_firstboot_run(void) {}

#endif
