/*
 * purr_card_shell.h - the card-based gesture shell (memory/[[project_appruntime_fullstack_plan]]'s
 * UI redesign, chunk I-1: static layout only, no gestures yet). Replaces purr_ui_load_and_build()
 * as what main.c calls to build the boot screen, per the explicit decision to replace the old
 * title/subtitle/app_grid/navbar shell.conf screen rather than keep it alongside this one.
 */
#ifndef PURR_CARD_SHELL_H
#define PURR_CARD_SHELL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the Home card and its three edge panels. Call after purr_lvgl_init() succeeds, same
 * point purr_ui_load_and_build() used to be called from. Always succeeds once the UI service
 * is running (no file to fail to load, unlike the old config-driven screen) -- esp_err_t
 * return kept for symmetry with that function and room for a real failure mode later. */
esp_err_t purr_card_shell_start(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CARD_SHELL_H */
