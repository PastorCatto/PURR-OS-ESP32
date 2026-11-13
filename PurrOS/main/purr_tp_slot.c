/*
 * purr_tp_slot.c - the built-in shell's entry points, for the Type shell (plan7.md, "The Type shell").
 *
 * The shell dispatcher (purr_shell.c) calls the built-in shell by five fixed names, and the shell's share of the shared interface by the `<name>_droid` names
 * (purr_shell_names.h). An image built with CONFIG_PURR_SHELL_TYPE has the Type shell in that place: this file is the whole of the glue, so neither the dispatcher
 * nor the Android shell's files needed a change.
 */
#include "purr_tp_priv.h"

#include "purr_android.h"

esp_err_t purr_android_start(void) { return purr_tp_start(); }
esp_err_t purr_android_set_orientation(int degrees) { return purr_tp_set_orientation(degrees); }
esp_err_t purr_android_lock(void) { return purr_tp_lock(); }
esp_err_t purr_android_notify(const char *title, const char *text) { return purr_tp_notify(title, text); }
esp_err_t purr_android_refresh_apps(void) { return purr_tp_refresh_apps(); }
