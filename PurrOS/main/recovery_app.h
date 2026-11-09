/*
 * recovery_app.h - the recovery app: what KittenOS shows on the screen once it has started (colour T-Deck boards).
 *
 * Built from the Settings widgets and the W10M theme, like the setup wizard. "Hi, welcome to recovery", why the device is here,
 * and the choices: restart, reinstall PURR OS and keep the data, or reinstall and erase everything. Erasing is behind a gate:
 * the system must be badly damaged (the bootloader's reasons say so, or there is nobody left to ask), or an administrator
 * proves who they are with their password.
 */
#ifndef RECOVERY_APP_H
#define RECOVERY_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the erase gate is open without a password for these bootloader reasons (PURR_RR_*): the system is damaged, missing or
 * its settings are unreadable, or it keeps failing to start. Plain C, no hardware: tested on the host. */
int recovery_gate_open_for(uint16_t reasons);

/* Builds the app over the screen (LVGL must be up). 0 on success. */
int recovery_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOVERY_APP_H */
