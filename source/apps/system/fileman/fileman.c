// fileman.c — PURR OS file manager (.claw)
// Uses purr_win.h only — works on KittenUI (LVGL) and MiniWin identically.
//
// Two panels: left = directory listing, right = text file preview.
// Roots:  /flash   (PURR internal flash, SPIFFS)
//         /sdcard  (SD card, if mounted)

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — see s_entries's own comment below

#define MAX_ENTRIES  64
#define PATH_MAX_LEN 256
#define PREVIEW_MAX  2048

// ── State ─────────────────────────────────────────────────────────────────────

static purr_win_t s_win        = 0;
static purr_wid_t s_path_lbl   = 0;   // current path header
static purr_wid_t s_file_list  = 0;   // left: selectable entry list
static purr_wid_t s_prev_area  = 0;   // right: file preview (textarea)
static purr_wid_t s_status_lbl = 0;

static char s_cwd[PATH_MAX_LEN] = "/flash";
// EXT_RAM_BSS_ATTR (PSRAM, not internal DRAM) — pure rebuilt-on-refresh
// directory-listing text, never touched before PSRAM is up (fileman is a
// normal launched app). Same class MiniWin's own control/list arrays and
// settings.c's/msn.c's row buffers already use this for — real, confirmed
// via purr_os.map: a genuine link-time DRAM overflow once the current app
// set was all compiled in together (see msn.c's matching comment,
// 2026-09-01).
static EXT_RAM_BSS_ATTR char s_entries[MAX_ENTRIES][64];
static EXT_RAM_BSS_ATTR int  s_entry_types[MAX_ENTRIES]; // 0=file, 1=dir
static int  s_entry_count = 0;
static int  s_selected    = -1;

// Formatted list labels ("[dir]" / " file") — kept off the stack since
// refresh_list() can be called from a task with a modest stack budget.
static EXT_RAM_BSS_ATTR char s_label_bufs[MAX_ENTRIES][68];
static const char *s_label_ptrs[MAX_ENTRIES];

// ── File ops dialog (New Folder / Rename / Delete confirm) ────────────────────
// One small popup window reused for all three ops — only one can be open at
// a time, matching how the rest of this app already assumes single-focus use.

typedef enum { DLG_NONE, DLG_NEW_FOLDER, DLG_RENAME, DLG_DELETE_CONFIRM } dlg_mode_t;

static dlg_mode_t s_dlg_mode  = DLG_NONE;
static purr_win_t s_dlg_win   = 0;
static purr_wid_t s_dlg_input = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void set_status(const char *msg) {
    purr_win_label_set(s_status_lbl, msg);
}

static void refresh_list(void) {
    s_entry_count = 0;
    s_selected    = -1;

    DIR *d = opendir(s_cwd);
    if (!d) {
        set_status("Cannot open directory.");
        purr_win_list_clear(s_file_list);
        return;
    }

    struct dirent *ent;

    // Parent entry if not at a root
    bool at_root = (strcmp(s_cwd, "/flash") == 0 ||
                    strcmp(s_cwd, "/sdcard")     == 0);
    if (!at_root) {
        strncpy(s_entries[s_entry_count], "..", sizeof(s_entries[0]) - 1);
        s_entry_types[s_entry_count] = 1;
        s_entry_count++;
    }

    while ((ent = readdir(d)) != NULL && s_entry_count < MAX_ENTRIES) {
        if (ent->d_name[0] == '.') continue;

        bool is_dir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            // Determine via stat for filesystems that don't set d_type
            char full[PATH_MAX_LEN];
            snprintf(full, sizeof(full), "%s/%s", s_cwd, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0) is_dir = S_ISDIR(st.st_mode);
        }

        strncpy(s_entries[s_entry_count], ent->d_name, sizeof(s_entries[0]) - 1);
        s_entry_types[s_entry_count] = is_dir ? 1 : 0;
        s_entry_count++;
    }
    closedir(d);

    for (int i = 0; i < s_entry_count; i++) {
        snprintf(s_label_bufs[i], sizeof(s_label_bufs[i]),
                 s_entry_types[i] ? "[%s]" : " %s", s_entries[i]);
        s_label_ptrs[i] = s_label_bufs[i];
    }
    purr_win_list_set_items(s_file_list, s_label_ptrs, s_entry_count);

    purr_win_label_set(s_path_lbl, s_cwd);
    purr_win_textarea_set(s_prev_area, "");
    set_status("Select a file to preview.");
}

static void preview_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        purr_win_textarea_set(s_prev_area, "(cannot open file)");
        return;
    }

    char buf[PREVIEW_MAX];
    size_t n = fread(buf, 1, PREVIEW_MAX - 1, f);
    fclose(f);
    buf[n] = '\0';

    // Replace non-printable bytes with '.' to stay UI-safe
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c != '\n' && c != '\r' && c != '\t' && (c < 0x20 || c > 0x7E))
            buf[i] = '.';
    }

    purr_win_textarea_set(s_prev_area, buf);

    char status[PATH_MAX_LEN + 24];
    snprintf(status, sizeof(status), "%lu bytes", (unsigned long)n);
    set_status(status);
}

// ── Navigation buttons ────────────────────────────────────────────────────────

static void navigate_to(const char *path) {
    strncpy(s_cwd, path, PATH_MAX_LEN - 1);
    refresh_list();
}

// "Up" — go to parent
static void on_up(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (strcmp(s_cwd, "/flash") == 0 || strcmp(s_cwd, "/sdcard") == 0) {
        // At a root — show root chooser
        navigate_to("/flash");
        return;
    }
    char parent[PATH_MAX_LEN];
    strncpy(parent, s_cwd, sizeof(parent) - 1);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
    } else {
        strcpy(parent, "/flash");
    }
    navigate_to(parent);
}

// Navigate to SPIFFS root
static void on_goto_spiffs(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    navigate_to("/flash");
}

// Navigate to SD root
static void on_goto_sd(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (!purr_kernel_sd_available()) {
        set_status("SD card not mounted.");
        return;
    }
    navigate_to("/sdcard");
}

// List entries are selected and opened in one tap/click — MiniWin and
// KittenUI both fire PURR_EVENT_SELECTED immediately followed by
// PURR_EVENT_ACTIVATED for a list, so this only needs to act on ACTIVATED.

static void update_selection_label(void) {
    if (s_selected < 0 || s_selected >= s_entry_count) {
        set_status("No selection.");
        return;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "[%d/%d] %s%s",
             s_selected + 1, s_entry_count,
             s_entries[s_selected],
             s_entry_types[s_selected] ? "/" : "");
    set_status(msg);
}

static void on_list_event(purr_wid_t w, purr_event_t e, void *u) {
    (void)u;
    int idx = purr_win_list_get_selected(s_file_list);
    if (idx < 0 || idx >= s_entry_count) return;
    s_selected = idx;

    if (e == PURR_EVENT_SELECTED) {
        update_selection_label();
        return;
    }
    if (e != PURR_EVENT_ACTIVATED) return;

    const char *name = s_entries[s_selected];

    if (strcmp(name, "..") == 0) {
        on_up(w, e, u);
        return;
    }

    char full[PATH_MAX_LEN];
    snprintf(full, sizeof(full), "%s/%s", s_cwd, name);

    if (s_entry_types[s_selected] == 1) {
        navigate_to(full);
    } else {
        preview_file(full);
        purr_win_label_set(s_path_lbl, full);
    }
}

// ── File ops (New Folder / Rename / Delete) ───────────────────────────────────

static void close_dialog(void) {
    if (s_dlg_win) purr_win_destroy(s_dlg_win);
    s_dlg_win   = 0;
    s_dlg_input = 0;
    s_dlg_mode  = DLG_NONE;
}

static void on_dialog_cancel(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    close_dialog();
}

static void on_dialog_confirm(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    const char *name = purr_win_textarea_get(s_dlg_input);
    dlg_mode_t mode = s_dlg_mode;

    if ((mode == DLG_NEW_FOLDER || mode == DLG_RENAME) && (!name || !*name)) {
        return;  // ignore empty name, leave dialog open
    }

    char full[PATH_MAX_LEN];
    if (mode == DLG_NEW_FOLDER) {
        snprintf(full, sizeof(full), "%s/%s", s_cwd, name);
        if (mkdir(full, 0755) != 0) set_status("New folder failed.");
    } else if (mode == DLG_RENAME) {
        if (s_selected < 0 || s_selected >= s_entry_count) { close_dialog(); return; }
        char oldpath[PATH_MAX_LEN];
        snprintf(oldpath, sizeof(oldpath), "%s/%s", s_cwd, s_entries[s_selected]);
        snprintf(full, sizeof(full), "%s/%s", s_cwd, name);
        if (rename(oldpath, full) != 0) set_status("Rename failed.");
    }

    close_dialog();
    refresh_list();
}

static void on_delete_yes(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_selected >= 0 && s_selected < s_entry_count) {
        char full[PATH_MAX_LEN];
        snprintf(full, sizeof(full), "%s/%s", s_cwd, s_entries[s_selected]);
        int rc = s_entry_types[s_selected] ? rmdir(full) : remove(full);
        if (rc != 0) set_status("Delete failed (directory not empty?).");
    }
    close_dialog();
    refresh_list();
}

static void on_delete_no(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    close_dialog();
}

// Opens the shared dialog window with a title label, input textarea +
// on-screen keyboard, and Create/Rename + Cancel buttons.
static void open_text_dialog(dlg_mode_t mode, const char *title) {
    if (s_dlg_win) close_dialog();
    s_dlg_mode = mode;

    s_dlg_win = purr_win_create(title);
    purr_win_label(s_dlg_win, "Name:");
    s_dlg_input = purr_win_textarea(s_dlg_win, 90, 20);

    purr_wid_t row = purr_win_row(s_dlg_win, 4);
    purr_win_button(s_dlg_win, (mode == DLG_RENAME) ? "Rename" : "Create",
                     on_dialog_confirm, NULL);
    purr_win_button(s_dlg_win, "Cancel", on_dialog_cancel, NULL);
    purr_win_layout_end(row);

    purr_win_textarea_focus(s_dlg_input);
    // win_show() first — see terminal.c's terminal_init() for why (Cupcake's
    // win_show() raises the window above whatever kb_show() just showed).
    purr_win_show(s_dlg_win);
    purr_win_keyboard_show(s_dlg_win, s_dlg_input);
}

static void on_new_folder(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    open_text_dialog(DLG_NEW_FOLDER, "New Folder");
}

static void on_rename(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_selected < 0 || s_selected >= s_entry_count) { set_status("No file selected."); return; }
    if (strcmp(s_entries[s_selected], "..") == 0) return;
    open_text_dialog(DLG_RENAME, "Rename");
}

static void on_delete(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_selected < 0 || s_selected >= s_entry_count) { set_status("No file selected."); return; }
    if (strcmp(s_entries[s_selected], "..") == 0) return;

    s_dlg_mode = DLG_DELETE_CONFIRM;
    char title[96];
    snprintf(title, sizeof(title), "Delete %s?", s_entries[s_selected]);
    s_dlg_win = purr_win_create(title);
    purr_win_label(s_dlg_win, "This cannot be undone.");
    purr_wid_t row = purr_win_row(s_dlg_win, 4);
    purr_win_button(s_dlg_win, "Yes", on_delete_yes, NULL);
    purr_win_button(s_dlg_win, "No",  on_delete_no,  NULL);
    purr_win_layout_end(row);
    purr_win_show(s_dlg_win);
}

// ── App lifecycle ─────────────────────────────────────────────────────────────

static int fileman_init(void) {
    s_win = purr_win_create("File Manager");

    // Toolbar row: SPIFFS | SD | Up | New | Rename | Delete
    purr_wid_t toolbar = purr_win_row(s_win, 4);
    purr_win_button(s_win, "SPIFFS", on_goto_spiffs, NULL);
    purr_win_button(s_win, "SD",     on_goto_sd,     NULL);
    purr_win_button(s_win, "Up",     on_up,          NULL);
    purr_win_button(s_win, "New",    on_new_folder,  NULL);
    purr_win_button(s_win, "Rename", on_rename,      NULL);
    purr_win_button(s_win, "Delete", on_delete,      NULL);
    purr_win_layout_end(toolbar);

    // Path header
    s_path_lbl = purr_win_label(s_win, s_cwd);

    // Content row: file list (60%) | preview (40%) — grow variant so this
    // row fills the remaining window height instead of hugging its own
    // content, which is required for the percentage-sized list/textarea
    // inside it to resolve against a real (non-zero) parent height.
    purr_wid_t content = purr_win_row_grow(s_win, 0);
    s_file_list = purr_win_list(s_win, 55, 70);
    purr_win_list_on_select(s_file_list, on_list_event, NULL);
    s_prev_area = purr_win_textarea(s_win, 40, 70);
    purr_win_layout_end(content);

    // Status bar
    s_status_lbl = purr_win_label(s_win, "Ready.");

    purr_win_show(s_win);
    refresh_list();
    return 0;
}

static void fileman_deinit(void) {
    close_dialog();
    purr_win_destroy(s_win);
    s_win = 0;
    s_entry_count = 0;
    s_selected = -1;
    strncpy(s_cwd, "/flash", PATH_MAX_LEN - 1);
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(fileman) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "fileman",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = fileman_init,
    .deinit            = fileman_deinit,
};
