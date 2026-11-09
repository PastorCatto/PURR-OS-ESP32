/*
 * oobe_ui.h - brings up the graphical side for the setup wizard (purr_oobe.h) on the colour T-Deck boards: LVGL, the UI
 * service, the theme, the hardware keyboard. Used by the recovery loader (part 1) and KittenOS first boot (part 2).
 */
#ifndef OOBE_UI_H
#define OOBE_UI_H

#include "purr_oobe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if this board can show the wizard (a colour T-Deck build), else 0 and the caller keeps its text flow. */
int oobe_ui_available(void);

/* Starts the wizard. Returns -1 if LVGL could not start (the caller falls back to text); otherwise it does not return
 * until the wizard restarts the device. start_page: 0 for the first page, or a later one to skip ahead. */
int oobe_ui_run(int part, const purr_oobe_ops_t *ops, int start_page);

#ifdef __cplusplus
}
#endif

#endif /* OOBE_UI_H */
