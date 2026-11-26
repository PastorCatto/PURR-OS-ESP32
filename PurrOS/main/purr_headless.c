/* Boards with no colour screen (the Heltec V3 node: a 0.96" OLED, no PSRAM) run the full profile without the shell, the UI pack, the Store's
 * windows or the repository client. The kernel's shared code (commands.c, the store, the system updater, the hardware setup and the device
 * catcalls) still names a few hooks of those; here they are, doing nothing, so one source tree serves both kinds of board (plan9.md,
 * "Device agnostic"). Compiled only when CONFIG_PURR_PROFILE_FULL and not CONFIG_PURR_BOARD_COLOR_LCD (main/CMakeLists.txt). */
#include "sdkconfig.h"

#if CONFIG_PURR_PROFILE_FULL && !CONFIG_PURR_BOARD_COLOR_LCD

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "purr_shell.h"
#include "purr_shell_abi.h"
#include "purr_repo_dev.h"
#include "purr_ui_assets.h"
#include "purr_catrt_msg.h"
void w10m_note_post(const char *title, const char *text);
void w10m_apps_changed_post(void);

/* No shell, so no shell modules: the loader's tables are empty. */
const void *const purr_shell_exports[1] = { NULL };
const char *const purr_shell_export_names[1] = { NULL };
const size_t purr_shell_export_count = 0;
const purr_shell_layout_t purr_shell_layout[1] = { { 0 } };
const size_t purr_shell_layout_count = 0;

esp_err_t purr_shell_notify(const char *title, const char *text) { (void)title; (void)text; return ESP_ERR_NOT_SUPPORTED; }
int purr_shell_list(purr_shell_info_t *out, int max) { (void)out; (void)max; return 0; }
void purr_shell_chosen(char *id, size_t cap) { if (id != NULL && cap > 0) { size_t i = 0; for (const char *s = "builtin"; *s && i + 1 < cap; s++) id[i++] = *s; id[i] = 0; } }
int purr_shell_choose(const char *id) { (void)id; return -1; }

void w10m_note_post(const char *title, const char *text) { (void)title; (void)text; }
void w10m_apps_changed_post(void) {}

int purr_ui_theme_slots_used(const char *except) { (void)except; return 0; }
int purr_ui_theme_remove(const char *id) { (void)id; return -1; }

void purr_catrt_msg_init(void) {}

int purr_repo_dev_resolve(const char *typed, char *out, size_t cap, char *err, size_t errcap) { (void)typed; (void)out; (void)cap; (void)err; (void)errcap; return -1; }
int purr_repo_dev_discover(purr_repo_server_t *out, int max, int wait_ms) { (void)out; (void)max; (void)wait_ms; return 0; }
int purr_repo_dev_last(purr_repo_server_t *out) { (void)out; return -1; }
void purr_repo_dev_remember(const purr_repo_server_t *s) { (void)s; }

#endif
