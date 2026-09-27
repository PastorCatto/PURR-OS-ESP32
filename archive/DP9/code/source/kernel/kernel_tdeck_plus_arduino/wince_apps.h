// wince_apps.h — baked-in WinCE app windows for tdeck_plus_arduino.
// Each app reads kernel state directly via purr_kernel_*() — no KITT layer,
// no .purr module wrapper, no Lua/emulator dependencies (those 0.11 apps
// needed subsystems — Lua VM, MagicMac/MagiDOS emulator, NVS boot-mode
// switch — that don't exist in this kernel yet).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void app_about_launch(void);
void app_files_launch(void);
void app_wifi_launch(void);
void app_lora_launch(void);
void app_restart_launch(void);

#ifdef __cplusplus
}
#endif
