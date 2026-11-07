/*
 * purr_shell_abi.h - the contract between the kernel and a loadable shell (plan7.md, "Shells as installable items, switched like themes").
 *
 * A shell that is not compiled in is a signed module (a `shell.cat`, built by purrstrap from the same C sources a built-in shell uses). It is linked
 * against the placeholder addresses of purr_shell_exports.def and relocated at load time; this header is what both sides agree on besides that list:
 *
 *   - PURR_SHELL_ABI_VERSION: bumped when a name leaves or moves in the list, or the ops table below changes. A shell whose number differs is not loaded.
 *   - the module's entry point, purr_shell_module_main(): the loader calls it once, the module returns its ops table;
 *   - the ops table: the five entry points purr_shell.h forwards and the 18 functions the shared code (Settings, the Developer Portal, the task manager,
 *     the permission prompt, the ui catcall windows, the notify log) calls on whichever shell runs (the interface of purr_w10m_priv.h plus
 *     purr_shell_notified()).
 *
 * The kernel side, purr_shell_abi.c, owns the table of real addresses; the loader (a later step) owns verifying, loading and the boot counter.
 */
#ifndef PURR_SHELL_ABI_H
#define PURR_SHELL_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_SHELL_ABI_VERSION   1
#define PURR_SHELL_IMPORT_BASE   0x0F000000u        /* the placeholder address of export 0; export i is at BASE + 4 * i. Nothing is mapped there on the chip; it is in the region of the module's link addresses, see purrstrap/lib/shellabi.py. */
#define PURR_SHELL_MODULE_ENTRY  "purr_shell_module_main"

/* The skin a shell draws the shared screens (Settings, windows) in: purr_theme_skin(). */
#define PURR_SHELL_SKIN_W10M     0
#define PURR_SHELL_SKIN_ANDROID  1

typedef struct {
    uint32_t abi_version;                           /* PURR_SHELL_ABI_VERSION the module was built against */
    uint32_t skin;                                  /* PURR_SHELL_SKIN_* */
    const char *id;                                 /* "w10m": the folder and the pack name (/ui/<id>/pack.bin) */

    /* the entry points purr_shell.h forwards */
    esp_err_t (*start)(void);
    esp_err_t (*set_orientation)(int degrees);
    esp_err_t (*lock)(void);
    esp_err_t (*notify)(const char *title, const char *text);
    esp_err_t (*refresh_apps)(void);

    /* the shared interface (purr_w10m_priv.h, purr_notify.h) */
    void (*apply_orientation)(int degrees);
    void (*apps_changed)(void);
    void (*apps_changed_post)(void);
    void (*banner)(const char *msg, int bad);
    void (*banner_post)(const char *msg, int bad);
    void (*busy_begin)(const char *msg);
    void (*busy_end)(void);
    void (*dev_badge_update)(void);
    int (*disp_h)(void);
    int (*disp_w)(void);
    int (*get_brightness)(void);
    const lv_image_dsc_t *(*img)(const char *name);
    int (*is_locked)(void);
    void (*note_post)(const char *title, const char *text);
    void (*place_sheet)(lv_obj_t *sheet);
    void (*save_display)(void);
    void (*set_brightness)(int level);
    void (*shell_notified)(const char *title, const char *text);
} purr_shell_ops_t;

/* What a module exports under PURR_SHELL_MODULE_ENTRY. */
typedef const purr_shell_ops_t *(*purr_shell_module_main_t)(void);

/* The kernel's table: the real address of every name in purr_shell_exports.def, in the file's order. The loader writes
 * purr_shell_exports[i] over every word of the module that holds PURR_SHELL_IMPORT_BASE + 4 * i. */
extern const void *const purr_shell_exports[];
extern const size_t purr_shell_export_count;

#ifdef __cplusplus
}
#endif

#endif /* PURR_SHELL_ABI_H */
