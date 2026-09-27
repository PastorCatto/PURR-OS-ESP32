// terminal.c — PURR OS built-in terminal app (.claw)
// Uses purr_win.h — works on both KittenUI (LVGL) and MiniWin.
// Commands: help, ls, cat, echo, modules, stop, start, restart, mem, uptime, reboot, clear

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"

#define CMD_MAX   128
#define OUT_MAX   2048

static purr_win_t  s_win  = 0;
static purr_wid_t  s_out  = 0;   // output textarea
static purr_wid_t  s_in   = 0;   // input textarea (single line)
static purr_wid_t  s_send = 0;   // send button
static char        s_outbuf[OUT_MAX];
static size_t      s_outlen = 0;

// ── Output helpers ────────────────────────────────────────────────────────────

static void term_print(const char *text) {
    size_t tlen = strlen(text);
    if (s_outlen + tlen >= OUT_MAX - 1) {
        // Scroll: drop first half
        size_t half = OUT_MAX / 2;
        memmove(s_outbuf, s_outbuf + half, s_outlen - half);
        s_outlen -= half;
        s_outbuf[s_outlen] = '\0';
    }
    memcpy(s_outbuf + s_outlen, text, tlen + 1);
    s_outlen += tlen;
    purr_win_textarea_set(s_out, s_outbuf);
}

static void term_println(const char *text) {
    term_print(text);
    term_print("\n");
}

// ── Command handlers ──────────────────────────────────────────────────────────

static void cmd_help(void) {
    term_println("Commands:");
    term_println("  help               this message");
    term_println("  ls [path]          list directory");
    term_println("  cat <path>         print file");
    term_println("  echo <text>        print text");
    term_println("  modules            list loaded modules");
    term_println("  stop <name>        disable a module");
    term_println("  start <name>       enable a module");
    term_println("  restart <name>     disable+enable a module");
    term_println("  mem                free RAM");
    term_println("  uptime             uptime in seconds");
    term_println("  clear              clear screen");
    term_println("  reboot             reboot device");
}

static void cmd_ls(const char *path) {
    const char *dir = (path && *path) ? path : "/flash";
    DIR *d = opendir(dir);
    if (!d) { term_println("ls: cannot open directory"); return; }
    struct dirent *ent;
    int n = 0;
    while ((ent = readdir(d))) {
        char line[64];
        snprintf(line, sizeof(line), "  %s%s\n",
                 ent->d_name,
                 ent->d_type == DT_DIR ? "/" : "");
        term_print(line);
        n++;
    }
    closedir(d);
    if (!n) term_println("  (empty)");
}

static void cmd_cat(const char *path) {
    if (!path || !*path) { term_println("cat: missing path"); return; }
    FILE *f = fopen(path, "r");
    if (!f) { term_println("cat: file not found"); return; }
    char buf[128];
    while (fgets(buf, sizeof(buf), f)) term_print(buf);
    fclose(f);
    term_print("\n");
}

static void cmd_echo(const char *text) {
    term_println(text ? text : "");
}

static const char *module_type_name(uint8_t type) {
    switch (type) {
        case PURR_MOD_DRIVER: return "driver";
        case PURR_MOD_SYSTEM: return "system";
        case PURR_MOD_UI:     return "ui";
        case PURR_MOD_APP:    return "app";
        default:              return "unknown";
    }
}

static void cmd_modules(void) {
    term_println("Loaded modules:");
    int n = purr_kernel_module_count();
    char line[64];
    for (int i = 0; i < n; i++) {
        const purr_module_header_t *hdr = purr_kernel_module_at(i);
        if (!hdr) continue;
        // meshtastic/meshcore are "extensions" managed through MSN, not
        // generic modules the user browses here — stop/start/restart by
        // name still work (Terminal doesn't need them listed to target
        // them), this only hides them from the enumeration.
        if (strcmp(hdr->name, "meshtastic") == 0 || strcmp(hdr->name, "meshcore") == 0) continue;
        snprintf(line, sizeof(line), "  %-16s %-8s v%s\n",
                 hdr->name, module_type_name(hdr->module_type), hdr->version);
        term_print(line);
    }
    const catcall_ui_t *ui = purr_kernel_ui();
    snprintf(line, sizeof(line), "  ui: %s\n", ui ? ui->name : "(none)");
    term_print(line);
    const catcall_display_t *disp = purr_kernel_display();
    snprintf(line, sizeof(line), "  display: %s\n", disp ? disp->name : "(none)");
    term_print(line);
}

static void svc_result(const char *name, int rc) {
    char buf[64];
    switch (rc) {
        case PURR_MODCTL_OK:              snprintf(buf, sizeof(buf), "%s: ok\n", name); break;
        case PURR_MODCTL_ERR_NOT_FOUND:   snprintf(buf, sizeof(buf), "%s: not found\n", name); break;
        case PURR_MODCTL_ERR_DENYLISTED:  snprintf(buf, sizeof(buf), "%s: refused (protected module)\n", name); break;
        case PURR_MODCTL_ERR_ALREADY:     snprintf(buf, sizeof(buf), "%s: already in that state\n", name); break;
        case PURR_MODCTL_ERR_INIT_FAILED: snprintf(buf, sizeof(buf), "%s: failed to start\n", name); break;
        default:                          snprintf(buf, sizeof(buf), "%s: error %d\n", name, rc); break;
    }
    term_print(buf);
}

static void cmd_stop(const char *args) {
    if (!args || !*args) { term_println("stop: missing module name"); return; }
    svc_result(args, purr_kernel_module_set_enabled(args, false));
}

static void cmd_start(const char *args) {
    if (!args || !*args) { term_println("start: missing module name"); return; }
    svc_result(args, purr_kernel_module_set_enabled(args, true));
}

static void cmd_restart(const char *args) {
    if (!args || !*args) { term_println("restart: missing module name"); return; }
    svc_result(args, purr_kernel_module_restart(args));
}

static void cmd_mem(void) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Free RAM: %lu bytes\n",
             (unsigned long)purr_kernel_free_ram());
    term_print(buf);
}

static void cmd_uptime(void) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Uptime: %llu s\n",
             (unsigned long long)(purr_kernel_uptime_ms() / 1000ULL));
    term_print(buf);
}

static void cmd_clear(void) {
    s_outlen = 0;
    s_outbuf[0] = '\0';
    purr_win_textarea_clear(s_out);
}

// ── Command dispatch ──────────────────────────────────────────────────────────

static void run_command(const char *line) {
    char buf[CMD_MAX];
    strncpy(buf, line, CMD_MAX - 1);
    buf[CMD_MAX - 1] = '\0';

    // Trim trailing newline/space
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
        buf[--len] = '\0';

    if (!*buf) return;

    // Echo command
    char prompt[CMD_MAX + 4];
    snprintf(prompt, sizeof(prompt), "> %s\n", buf);
    term_print(prompt);

    // Split verb + args
    char *space = strchr(buf, ' ');
    const char *args = space ? (space + 1) : "";
    if (space) *space = '\0';
    const char *verb = buf;

    if      (strcmp(verb, "help")   == 0) cmd_help();
    else if (strcmp(verb, "ls")     == 0) cmd_ls(args);
    else if (strcmp(verb, "cat")    == 0) cmd_cat(args);
    else if (strcmp(verb, "echo")   == 0) cmd_echo(args);
    else if (strcmp(verb, "modules")== 0) cmd_modules();
    else if (strcmp(verb, "stop")    == 0) cmd_stop(args);
    else if (strcmp(verb, "start")   == 0) cmd_start(args);
    else if (strcmp(verb, "restart") == 0) cmd_restart(args);
    else if (strcmp(verb, "mem")    == 0) cmd_mem();
    else if (strcmp(verb, "uptime") == 0) cmd_uptime();
    else if (strcmp(verb, "clear")  == 0) cmd_clear();
    else if (strcmp(verb, "reboot") == 0) { term_println("Rebooting..."); vTaskDelay(300); purr_kernel_reboot(); }
    else {
        char err[CMD_MAX + 24];
        snprintf(err, sizeof(err), "unknown command: %s\n", verb);
        term_print(err);
    }
}

// ── UI callbacks ──────────────────────────────────────────────────────────────

static void on_send(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event; (void)user;
    const char *text = purr_win_textarea_get(s_in);
    if (text && *text) {
        run_command(text);
        purr_win_textarea_clear(s_in);
    }
}

// Physical-keyboard Enter: MiniWin fires the textarea callback with
// PURR_EVENT_ACTIVATED instead of inserting a newline — submit, same as Send.
static void on_input_event(purr_wid_t wid, purr_event_t event, void *user) {
    if (event == PURR_EVENT_ACTIVATED) on_send(wid, event, user);
}

// ── App init / deinit ─────────────────────────────────────────────────────────

static int terminal_init(void) {
    s_outlen = 0;
    s_outbuf[0] = '\0';

    s_win  = purr_win_create("Terminal");
    s_out  = purr_win_textarea(s_win, 100, 75);
    s_in   = purr_win_textarea(s_win, 80, 10);
    s_send = purr_win_button(s_win, "Send", on_send, NULL);

    purr_win_textarea_on_change(s_in, on_input_event, NULL);
    purr_win_textarea_focus(s_in);
    // win_show() must come first — Cupcake's win_show() raises the app
    // window to the front (lv_obj_move_foreground()), so calling it after
    // kb_show() would paint the window over the on-screen keyboard we just
    // asked for, hiding it behind the app.
    purr_win_show(s_win);
    purr_win_keyboard_show(s_win, s_in);

    term_println("PURR OS Terminal v1.0");
    term_println("Type 'help' for commands.");
    term_print("> ");
    return 0;
}

static void terminal_deinit(void) {
    purr_win_keyboard_hide(s_win);
    purr_win_destroy(s_win);
    s_win = 0; s_out = 0; s_in = 0; s_send = 0;
}

// ── Module header ─────────────────────────────────────────────────────────────

#include "purr_module.h"

PURR_MODULE_REGISTER(terminal) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "terminal",
    .version           = "1.0.1",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = terminal_init,
    .deinit            = terminal_deinit,
};
