/*
 * purr_shell.c - forwards the neutral shell entry points (purr_shell.h) to the shell this image was built with.
 */
#include "purr_shell.h"

#include "sdkconfig.h"

#if CONFIG_PURR_SHELL_ANDROID
#include "purr_android.h"
#define SHELL(fn) purr_android_##fn
#define SHELL_NAME "android"
#else
#include "purr_w10m.h"
#define SHELL(fn) purr_w10m_##fn
#define SHELL_NAME "w10m"
#endif

esp_err_t purr_shell_start(void) { return SHELL(start)(); }
esp_err_t purr_shell_set_orientation(int degrees) { return SHELL(set_orientation)(degrees); }
esp_err_t purr_shell_lock(void) { return SHELL(lock)(); }
esp_err_t purr_shell_notify(const char *title, const char *text) { return SHELL(notify)(title, text); }
esp_err_t purr_shell_refresh_apps(void) { return SHELL(refresh_apps)(); }
const char *purr_shell_name(void) { return SHELL_NAME; }
