/*
 * purr_catrt_devices.c - the `devices` catcall (purr_catcalls.h): finding and setting up hardware, driver and system updates, the developer
 * server. These used to be pages of Settings; the Device Manager app now drives them through this table, so what the app may do is exactly the
 * list below, behind one permission ("manage-devices", administrators only).
 *
 * Work that takes time runs on one worker task. start() returns at once; when the job ends the app gets PURR_APPEV_UI with the name "job"
 * (arg 0 = it worked, 1 = it did not), and result() holds the sentence. One job at a time.
 */
#include "purr_catrt_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "commands.h"
#include "login.h"
#include "purr_board.h"
#include "purr_brand.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_catrt_power.h"
#include "purr_driver.h"
#include "purr_hwscan_plat.h"
#include "purr_hwsetup.h"
#include "purr_perm.h"
#include "purr_repo_dev.h"
#include "purr_sysupdate.h"
#include "purr_users.h"

static purr_hwscan_report_t s_hw;
static purr_repo_server_t s_found[4];
static int s_nfound;

static struct {
    volatile int busy;
    int slot;
    char what[16], a[72], b[72];
    char msg[160];
    int rc;
} s_job;

static char s_last[160];

static int is_admin_now(void)
{
    return purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN;
}

/* 0 if the calling app may use the table now, else DENIED or ASK (a prompt is up). */
static int gate(void)
{
    if (!is_admin_now()) {
        return PURR_DEVICES_DENIED;
    }
    return purr_catrt_perm_gate(PURR_PERM_MANAGE_DEVICES);
}

static int to_set_up(void)
{
    int n = 0;
    for (int i = 0; i < s_hw.nfound; i++) {
        n += s_hw.found[i].known && !purr_hwsetup_is_set_up(&s_hw.found[i]);
    }
    return n;
}

static int run(void)
{
    const char *w = s_job.what;
    char *m = s_job.msg;
    size_t mc = sizeof(s_job.msg);
    if (strcmp(w, "find") == 0) {
        int rc = purr_hwscan_run(-1, &s_hw);
        snprintf(m, mc, "Looked at %d free pins; found %d device(s).", s_hw.looked_at, s_hw.nfound);
        return rc;
    }
    if (strcmp(w, "setup") == 0) {
        return purr_hwsetup_install(&s_hw, NULL, m, mc) > 0 ? 0 : -1;
    }
    if (strcmp(w, "update-drivers") == 0) {
        return purr_hwsetup_update_drivers(NULL, m, mc) >= 0 ? 0 : -1;
    }
    if (strcmp(w, "get-drivers") == 0) {
        return purr_hwsetup_get_missing(NULL, m, mc) > 0 ? 0 : -1;
    }
    if (strcmp(w, "add") == 0) {
        int sda = 0, scl = 0, addr = 0;
        if (sscanf(s_job.b, "%d %d %i", &sda, &scl, &addr) != 3) {
            snprintf(m, mc, "Give the driver name, two pin numbers and an address like 0x55.");
            return -1;
        }
        return purr_hwsetup_add_manual(s_job.a, sda, scl, addr, m, mc) == 0 ? 0 : -1;
    }
    if (strcmp(w, "read") == 0) {
        return purr_hwsetup_read_text(s_job.a, m, mc) == 0 ? 0 : -1;
    }
    if (strcmp(w, "remove") == 0) {
        return purr_hwsetup_remove(s_job.a, m, mc) == 0 ? 0 : -1;
    }
    if (strcmp(w, "check") == 0) {
        int f = purr_sysupdate_check(s_job.a[0] == 'd', m, mc);
        return f;                                       /* 1: something newer, 0: up to date, -1: could not ask */
    }
    if (strcmp(w, "update") == 0 || strcmp(w, "reinstall") == 0) {
        int force = w[0] == 'r';
        if (purr_sysupdate_request(s_job.a[0] == 'd', force) != 0) {
            snprintf(m, mc, "Could not ask for that.");
            return -1;
        }
        snprintf(m, mc, force ? "Ready to reinstall. Restart to begin." : "Ready to update. Restart to begin.");
        return 0;
    }
    if (strcmp(w, "dev-find") == 0) {
        s_nfound = purr_repo_dev_discover(s_found, 4, 2500);
        snprintf(m, mc, s_nfound ? "Found %d server(s). Pick one." : "No developer server answered on this network.", s_nfound);
        return 0;
    }
    if (strcmp(w, "dev-use") == 0) {
        int i = atoi(s_job.a);
        if (i >= 0 && i < s_nfound && purr_devsrv_set(&s_found[i]) == 0) {
            snprintf(m, mc, "Using %s.", s_found[i].name);
            return 0;
        }
        snprintf(m, mc, "Could not use that server.");
        return -1;
    }
    if (strcmp(w, "dev-set") == 0) {
        char err[80] = "";
        if (purr_devsrv_set_typed(s_job.a, err, sizeof(err)) != 0) {
            snprintf(m, mc, "%s", err[0] ? err : "That address was not accepted.");
            return -1;
        }
        snprintf(m, mc, "Using that server.");
        return 0;
    }
    snprintf(m, mc, "I do not know that job.");
    return -1;
}

static void job_task(void *arg)
{
    (void)arg;
    s_job.msg[0] = '\0';
    s_job.rc = run();
    snprintf(s_last, sizeof(s_last), "%s", s_job.msg);
    int slot = s_job.slot;
    s_job.busy = 0;
    purr_catrt_post_ui(slot, s_job.rc < 0 ? 1 : s_job.rc == 1 ? 2 : 0, "job");
    vTaskDelete(NULL);
}

static int cc_start(const char *what, const char *a, const char *b)
{
    int g = gate();
    if (g != 0) {
        return g;
    }
    if (what == NULL) {
        return PURR_DEVICES_BAD;
    }
    if (s_job.busy) {
        return PURR_DEVICES_BUSY;
    }
    int slot = purr_catrt_caller(NULL, 0);
    if (slot < 0) {
        return PURR_DEVICES_BAD;
    }
    s_job.busy = 1;
    s_job.slot = slot;
    snprintf(s_job.what, sizeof(s_job.what), "%s", what);
    snprintf(s_job.a, sizeof(s_job.a), "%s", a != NULL ? a : "");
    snprintf(s_job.b, sizeof(s_job.b), "%s", b != NULL ? b : "");
    if (xTaskCreate(job_task, "dev-job", 12288, NULL, 3, NULL) != pdPASS) {
        s_job.busy = 0;
        return PURR_DEVICES_BAD;
    }
    return PURR_DEVICES_OK;
}

static int cc_result(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return PURR_DEVICES_BAD;
    }
    snprintf(out, cap, "%s", s_last);
    return (int)strlen(out);
}

static int put(char *a, size_t acap, const char *t)
{
    snprintf(a, acap, "%s", t);
    return 1;
}

static int cc_list(const char *what, int i, char *a, size_t acap, char *b, size_t bcap)
{
    int g = gate();
    if (g != 0) {
        return g;
    }
    if (what == NULL || a == NULL || b == NULL || i < 0) {
        return PURR_DEVICES_BAD;
    }
    a[0] = b[0] = '\0';
    if (strcmp(what, "found") == 0) {
        if (i >= s_hw.nfound) {
            return 0;
        }
        const purr_hwscan_found_t *f = &s_hw.found[i];
        put(a, acap, f->known ? f->match.name : "Unknown device");
        if (f->known) {
            snprintf(b, bcap, "I2C 0x%02X, pins %d / %d, driver \"%s\"%s%s", f->addr, f->sda, f->scl, f->match.compatible, f->match.by_id ? "" : " (guessed)",
                     purr_hwsetup_is_set_up(f) ? "  - set up" : "");
        } else {
            snprintf(b, bcap, "I2C 0x%02X, pins %d / %d", f->addr, f->sda, f->scl);
        }
        return 1;
    }
    if (strcmp(what, "profile") == 0) {
        static purr_hwp_t p;
        int n = purr_hwsetup_profile(&p);
        if (i >= n) {
            return 0;
        }
        put(a, acap, p.dev[i].name);
        snprintf(b, bcap, "%s%s%s: pins %d / %d, driver \"%s\"%s", p.dev[i].label, p.dev[i].label[0] ? " " : "", p.dev[i].bus[0] ? p.dev[i].bus : "i2c",
                 p.dev[i].sda, p.dev[i].scl, p.dev[i].compatible, purr_device_state(p.dev[i].name) == PURR_DEV_BOUND ? "" : " (not running)");
        return 1;
    }
    if (strcmp(what, "board") == 0) {
        const purr_board_t *bd = purr_board();
        if (i >= bd->device_count) {
            return 0;
        }
        put(a, acap, bd->devices[i].name);
        snprintf(b, bcap, "driver \"%s\"%s", bd->devices[i].compatible, purr_device_state(bd->devices[i].name) == PURR_DEV_BOUND ? "" : " (no driver loaded)");
        return 1;
    }
    if (strcmp(what, "missing") == 0) {
        char dev[32], compat[32];
        if (i >= purr_hwsetup_missing(dev, sizeof(dev), i, compat, sizeof(compat))) {
            return 0;
        }
        put(a, acap, dev);
        snprintf(b, bcap, "needs the \"%s\" driver", compat);
        return 1;
    }
    if (strcmp(what, "drivers") == 0) {
        static purr_hwsetup_driver_t drv[PURR_HWSETUP_MAX_DRIVERS];
        int n = purr_hwsetup_list_drivers(drv, PURR_HWSETUP_MAX_DRIVERS);
        if (i >= n) {
            return 0;
        }
        put(a, acap, drv[i].name);
        snprintf(b, bcap, "version %s", drv[i].version[0] ? drv[i].version : "?");
        return 1;
    }
    if (strcmp(what, "servers") == 0) {
        if (i >= s_nfound) {
            return 0;
        }
        put(a, acap, s_found[i].name);
        put(b, bcap, s_found[i].base);
        return 1;
    }
    return PURR_DEVICES_BAD;
}

static int cc_info(const char *what, char *out, size_t cap)
{
    int g = gate();
    if (g != 0) {
        return g;
    }
    if (what == NULL || out == NULL || cap == 0) {
        return PURR_DEVICES_BAD;
    }
    out[0] = '\0';
    if (strcmp(what, "versions") == 0) {
        char kv[16], ov[16];
        purr_sysupdate_versions(kv, ov);
        snprintf(out, cap, PURR_BRAND_PRODUCT " %s, " PURR_BRAND_RECOVERY " %s", kv, ov);
    } else if (strcmp(what, "devserver") == 0) {
        purr_repo_server_t sv;
        if (purr_devsrv_get(&sv) == 0) {
            snprintf(out, cap, "%s  %s", sv.name, sv.base);
        }
    } else if (strcmp(what, "dev-mode") == 0) {
        snprintf(out, cap, "%d", purr_security_dev_active() ? 1 : 0);
    } else if (strcmp(what, "to-set-up") == 0) {
        snprintf(out, cap, "%d", to_set_up());
    } else if (strcmp(what, "missing") == 0) {
        snprintf(out, cap, "%d", purr_hwsetup_missing(NULL, 0, -1, NULL, 0));
    } else if (strcmp(what, "bootloader-update") == 0) {
        snprintf(out, cap, "%d", purr_sysupdate_includes_bootloader() ? 1 : 0);
    } else {
        return PURR_DEVICES_BAD;
    }
    return (int)strlen(out);
}

static void cc_restart(void)
{
    if (gate() != 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static int cc_is_admin(void)
{
    return is_admin_now();
}

static const purr_cc_devices_t s_cc_devices = {
    .version = 1, .is_admin = cc_is_admin, .start = cc_start, .result = cc_result, .list = cc_list, .info = cc_info, .restart = cc_restart,
};

void purr_catrt_devices_init(void)
{
    purr_catrt_set_devices(&s_cc_devices);
}
