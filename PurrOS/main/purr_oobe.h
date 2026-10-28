/*
 * purr_oobe.h - the first-run setup of KittenOS internet recovery (plan10-era design, see plan1.md "OOBE").
 *
 * A wizard sheet made of the Settings app's own widgets (purr_w10m_widgets.h) and the W10M theme, so it looks and behaves
 * like the rest of the UI. It runs in two parts, because recovery itself has two stages:
 *
 *   part 1, the recovery loader (no filesystem): "something went wrong" with the reasons the bootloader flagged, Wi-Fi (saved
 *           for the next boot), restoring. The restore is only
 *           KittenOS and what it needs to restart; the rest is KittenOS's job (part 2).
 *   part 2, KittenOS first boot (has a filesystem): pick a colour, legal and credits (a text file the install job fetches), the account, date and time, done. KittenOS installs the
 *           kernel and the modules in the background while the user answers, and the last page waits for it.
 *
 * Everything that touches the platform goes through purr_oobe_ops_t, so the pages are the same on the device and in the
 * host preview (tools/lvgl_host). Slow work is a job: started, then polled from an LVGL timer (never blocking the UI task).
 * All calls are on the UI service task.
 */
#ifndef PURR_OOBE_H
#define PURR_OOBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int rssi;
    int secure;
} purr_oobe_ap_t;

typedef struct {
    char base[80];                      /* http://192.168.1.20:47801 */
    char name[32];
    char id[24];
} purr_oobe_server_t;

typedef enum {
    PURR_OOBE_JOB_DEVSCAN,      /* look for dev servers on the network (only when the bootloader allowed one) */
    PURR_OOBE_JOB_DEVVERIFY,    /* a = the server's base address: check it has recovery files for this device; msg says what it found */
    PURR_OOBE_JOB_SCAN,         /* list the Wi-Fi networks */
    PURR_OOBE_JOB_CONNECT,      /* a = ssid, b = password (NULL/empty for open or remembered); the network is remembered */
    PURR_OOBE_JOB_RESTORE,      /* install KittenOS and what it needs to restart (the rest is KittenOS's job); per_mille and msg report progress */
    PURR_OOBE_JOB_ACCOUNT,      /* a = user name, b = password; creates the administrator account */
    PURR_OOBE_JOB_LEGAL,        /* load the legal and credits text (a text file, never built into the firmware) */
} purr_oobe_job_t;

typedef struct {
    uint16_t (*reasons)(void);                                  /* PURR_RR_*: why we are in recovery (part 1); 0 if unknown */
    /* The legal and credits text once a LEGAL job has finished (a NUL-terminated string), or NULL if it could not be loaded.
     * The loader downloads OTA/legal.txt from the recovery server next to the manifest; later stages read /etc/legal.txt. */
    const char *(*legal)(void);
    int (*dev_recovery)(void);                                  /* 1 if the bootloader allowed a dev server: adds the dev server page */
    int (*server_results)(purr_oobe_server_t *out, int max);    /* after a finished DEVSCAN */
    void (*use_server)(const purr_oobe_server_t *server);       /* the chosen dev server, or NULL for the internet; remembered */
    int (*wired)(void);                                         /* 1 on a wired network (no Wi-Fi page) */
    const char *(*remembered_ssid)(void);                       /* the saved network, or NULL */
    int (*job_start)(purr_oobe_job_t kind, const char *a, const char *b);                 /* 0 = started */
    /* -1 still running, 0 finished, 1 failed (msg says why). per_mille: progress where the job has any, else -1. */
    int (*job_poll)(purr_oobe_job_t kind, char *msg, size_t cap, int *per_mille);
    int (*scan_results)(purr_oobe_ap_t *out, int max);          /* after a finished SCAN */
    /* The rest of the system installing in the background (part 2): -1 nothing running, 0..999 progress, 1000 done,
     * -2 it failed (msg says why). Finish is available once it is not running. */
    int (*background)(char *msg, size_t cap);
    void (*finish)(void);                                       /* restart into the next stage */
    void (*use_keypad)(void *lv_group);                         /* give this LVGL group the hardware keyboard (NULL on the host) */
} purr_oobe_ops_t;

#define PURR_OOBE_PART_LOADER 1
#define PURR_OOBE_PART_FIRSTBOOT 2
/* The loader had nowhere to download from: Wi-Fi (and the dev server page when allowed), saved, then it restarts and the automatic reinstall
 * runs with a fresh heap. No legal page and no install here. */
#define PURR_OOBE_PART_LOADER_SETUP 3
/* KittenOS after a reinstall that kept the data (an account exists): one page, "just a few more minutes", a bar for the background install,
 * then it restarts into PURR OS by itself. */
#define PURR_OOBE_PART_FINISHING 4

/* Builds the wizard over the screen and shows its first page. `start_page` lets a preview open any page (0-based); -1 opens the
 * last page (the install), which is where an unattended recovery starts. */
void purr_oobe_start(int part, const purr_oobe_ops_t *ops, int start_page);
int purr_oobe_page_count(int part);

#ifdef __cplusplus
}
#endif

#endif /* PURR_OOBE_H */
