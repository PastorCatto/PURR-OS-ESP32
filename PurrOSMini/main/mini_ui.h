/* mini_ui.h - the few screens PURR OS Mini has of its own: a status screen, the Wi-Fi list and the password keyboard. Drawn with mini_gfx, driven by touch. */
#ifndef MINI_UI_H
#define MINI_UI_H

#include <stdbool.h>

#include "mini_wifi.h"

/* The boot screen: an original paw print in the accent colour, the name, and a progress bar. The first call (or any call with progress 0) draws all of it; later calls only move the
 * bar. hint is a small line at the bottom (NULL for none). progress is 0..100. */
void mini_ui_boot(int progress, const char *hint);

/* A title bar and one or two lines of text; colour is the second line's. */
void mini_ui_status(const char *title, const char *line1, const char *line2);
/* The Wi-Fi list: scans, shows the networks, and returns the chosen one in *out (true), or false if the user asks to keep the current setup (never today). */
bool mini_ui_pick_network(mini_ap_t *out);
/* The keyboard: types a password for ssid into pass (cap bytes). false if the user goes back. */
bool mini_ui_password(const char *ssid, char *pass, int cap);
/* Waits for a touch: returns when a finger goes down and comes up, with the last point; or false after timeout_ms (0 waits forever). */
bool mini_ui_tap(int *x, int *y, int timeout_ms);

/* The main menu (plan1.md, "Mini main menu"): a status bar (Wi-Fi bars, the title, the paired host), an optional one-line strip, and the iPod list. Apps is greyed until a host is paired.
 * Returns the row tapped (a greyed row is refused with a short flash and the menu stays). */
#define MINI_HOME_DISPLAY 0
#define MINI_HOME_APPS 1
#define MINI_HOME_PDA 2
#define MINI_HOME_WIFI 3
#define MINI_HOME_SETTINGS 4
typedef struct {
    const char *strip;              /* one line under the bar ("Reconnecting to PURR-3F2A..."), NULL for none */
    const char *host;               /* the paired host's name for the bar and the rows, NULL if none is paired */
    const char *wifi;               /* the network's name, NULL when not connected */
    bool host_online;               /* the paired host is in range now */
} mini_home_t;
int mini_ui_home(const mini_home_t *info);

/* Settings: Forget paired devices, Touch calibration, About. Returns MINI_SET_*, or MINI_SET_BACK. */
#define MINI_SET_FORGET 0
#define MINI_SET_CAL 1
#define MINI_SET_ABOUT 2
#define MINI_SET_BACK -1
int mini_ui_settings(bool have_paired);

/* The searching screen: a status and a Cancel button. mini_ui_cancel_hit says whether a touch at x, y is on it. */
void mini_ui_search(const char *line1, const char *line2);
bool mini_ui_cancel_hit(int x, int y);

/* Hosts in range: a list of names (a paired one is marked), tap one. Returns its index, -1 for Cancel, -2 if nothing was tapped within wait_ms (the caller redraws with fresh names). */
int mini_ui_hosts(const char *title, const char names[][20], const unsigned char *paired, int n, int wait_ms);
/* The pairing question: the six digits and Pair / Cancel. Returns 1, 0, or -1 if nothing was tapped within wait_ms. */
int mini_ui_code(const char *name, int code, int wait_ms);

/* Four crosses at the corners; the taps are turned into the touch calibration and kept. */
void mini_ui_calibrate(void);

#endif
