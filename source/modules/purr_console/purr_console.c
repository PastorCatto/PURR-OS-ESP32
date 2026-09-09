// purr_console.c — see purr_console.h for the full picture.
//
// Named purr_console, not console: ESP-IDF ships its own built-in
// component literally called "console" (esp_console_*, argtable3,
// linenoise) — a plain "console" directory here silently lost the name
// collision (component-name resolution picked IDF's, not this one), which
// meant NOTHING in this file was ever actually compiled despite the build
// reporting success end-to-end, right up until the final link step
// failed on undefined references to every purr_console_*() symbol.
// Confirmed live, on real hardware, the first time this was actually
// built rather than just reviewed. Every other name in this codebase
// avoids exactly this trap by prefixing with purr_/catcall_/etc.; this
// file previously didn't, and paid for it.
//
// Command handlers below are ported from source/apps/system/terminal/
// terminal.c (help/ls/cat/echo/modules/stop/start/restart/mem/uptime/
// reboot/clear) and each kernel_*_boot.c's own serial_console_task() (kb)
// — same behavior, retargeted from purr_win_textarea_*() calls (which
// need a full UI backend to exist) onto this module's own
// purr_console_io_t, and from a hardcoded UART0 read onto the same
// abstraction so `kb` works over any bound transport, not just UART0.
//
// terminal.c's s_outbuf/scrolling logic is deliberately NOT carried over:
// it existed only because purr_win_textarea_set() needs the textarea's
// WHOLE text re-supplied on every update (an LVGL widget constraint).
// Writing straight to a byte-stream I/O has no such constraint — each
// line goes out as it's produced, and the real terminal on the other end
// (a serial monitor, a USB-CDC terminal emulator) handles its own
// scrollback the way every other tool already using this UART expects.
//
// One deliberate, documented departure from the plan's own prose: `scan`
// (an I2C bus sweep) is NOT one of this module's built-ins, unlike `kb`.
// scan needs a per-device SDA/SCL pin pair (see kernel_tdp_boot.c's own
// i2c_scan_cmd(), hardcoded to this board's SDA=18/SCL=8) — baking a
// device-specific pin pair into this shared, device-agnostic module would
// be exactly the layering mistake docs/17 warns against elsewhere in this
// effort. It's registered instead via purr_console_register_commands()
// from the specialized kernel boot that swaps this module in, which is
// what that entry point exists for.
//
// A second departure, also deliberate: purr_probe_cmd.c's '%'-per-line
// sentinel + '%ok'/'%err' terminator discipline is NOT applied to every
// line of output here. That scheme exists to serve a scripted HOST parser
// (purr_probe_cmd.c's whole purpose) that must never be derailed by an
// ESP_LOG line arriving mid-response — a real, different problem from
// this module's primary audience, a human reading a terminal. Prefixing
// every line of `ls`/`cat`/`modules` output with '%' would make this
// console strictly worse to use interactively than terminal.c already is,
// for no benefit to that use case, and would fail this phase's own
// verification bar ("matching today's terminal.c/serial_console_task
// behavior"). The `> ` prompt this module already prints — same as
// terminal.c and serial_console_task before it — already gives a human
// (and a not-too-clever host parser) an unambiguous point to resync on.
// If a real scripted-host use case shows up for THIS console later, add
// the sentinel scheme then, as its own opt-in mode — don't pay for it now
// on the path that doesn't need it.

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "purr_module.h"

static const char *TAG = "console";

#define CMD_MAX  128

typedef struct {
    const purr_console_cmd_t *cmds;
    int                        count;
} cmd_table_t;

#define MAX_EXTRA_TABLES 4
static cmd_table_t s_extra_tables[MAX_EXTRA_TABLES];
static int         s_extra_table_count = 0;

static const purr_console_io_t *s_io   = NULL;
static bool                     s_echo = true;
static purr_console_login_fn    s_login_fn = NULL;
static purr_console_exec_fn     s_exec_fn  = NULL;
// See purr_console_request_stop()'s own doc comment in the header for
// exactly what this is for and why vTaskDelete() from outside was tried
// first and found unsafe on real hardware. volatile: read by whichever
// task is running purr_console_run()'s loop, written by a DIFFERENT task
// requesting the stop.
static volatile bool            s_stop_requested = false;

void purr_console_set_login_fn(purr_console_login_fn fn) { s_login_fn = fn; }
void purr_console_set_exec_fn(purr_console_exec_fn fn)   { s_exec_fn  = fn; }
void purr_console_request_stop(void)                     { s_stop_requested = true; }

// ── Output ─────────────────────────────────────────────────────────────────

void purr_console_print(const char *text) {
    if (!s_io || !text) return;
    s_io->write(text, strlen(text));
}

void purr_console_println(const char *text) {
    purr_console_print(text);
    purr_console_print("\r\n");
}

void purr_console_set_echo(bool enabled) {
    s_echo = enabled;
}

// ── Built-in command handlers ───────────────────────────────────────────────

static void cmd_help(const char *args) {
    (void)args;
    purr_console_println("Commands:");
    purr_console_println("  help               this message");
    purr_console_println("  ls [path]          list directory");
    purr_console_println("  cat <path>         print file");
    purr_console_println("  echo <text>        print text");
    purr_console_println("  modules            list loaded modules");
    purr_console_println("  stop <name>        disable a module");
    purr_console_println("  start <name>       enable a module");
    purr_console_println("  restart <name>     disable+enable a module");
    purr_console_println("  startui <name>     enable a UI module (e.g. 'startui mochi')");
    purr_console_println("  exec <app>         launch an app by name");
    purr_console_println("  mem                free RAM");
    purr_console_println("  uptime             uptime in seconds");
    purr_console_println("  kb                 echo keyboard keypresses (q to exit)");
    purr_console_println("  clear              clear screen");
    purr_console_println("  reboot             reboot device");
    for (int t = 0; t < s_extra_table_count; t++) {
        for (int i = 0; i < s_extra_tables[t].count; i++) {
            const purr_console_cmd_t *c = &s_extra_tables[t].cmds[i];
            char line[96];
            snprintf(line, sizeof(line), "  %-18s %s", c->name, c->help ? c->help : "");
            purr_console_println(line);
        }
    }
}

static void cmd_ls(const char *path) {
    const char *dir = (path && *path) ? path : "/flash";
    DIR *d = opendir(dir);
    if (!d) { purr_console_println("ls: cannot open directory"); return; }
    struct dirent *ent;
    int n = 0;
    while ((ent = readdir(d))) {
        // sizeof(ent->d_name) (up to 256 on this toolchain) + "  "/"/" +
        // NUL — terminal.c's own cmd_ls() this was ported from used a
        // fixed 64-byte buffer for the same snprintf and never caught
        // this: it compiles via catstrap's own, more lenient warning set,
        // not this kernel spine's -Werror=format-truncation. Sized
        // against the real field instead of a guessed constant so this
        // can't drift out of sync with it again.
        char line[sizeof(ent->d_name) + 4];
        snprintf(line, sizeof(line), "  %s%s",
                 ent->d_name,
                 ent->d_type == DT_DIR ? "/" : "");
        purr_console_println(line);
        n++;
    }
    closedir(d);
    if (!n) purr_console_println("  (empty)");
}

static void cmd_cat(const char *path) {
    if (!path || !*path) { purr_console_println("cat: missing path"); return; }
    FILE *f = fopen(path, "r");
    if (!f) { purr_console_println("cat: file not found"); return; }
    char buf[128];
    while (fgets(buf, sizeof(buf), f)) purr_console_print(buf);
    fclose(f);
    purr_console_print("\r\n");
}

static void cmd_echo(const char *text) {
    purr_console_println(text ? text : "");
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

static void cmd_modules(const char *args) {
    (void)args;
    purr_console_println("Loaded modules:");
    int n = purr_kernel_module_count();
    char line[64];
    for (int i = 0; i < n; i++) {
        const purr_module_header_t *hdr = purr_kernel_module_at(i);
        if (!hdr) continue;
        // Same exclusion terminal.c's own cmd_modules() applies — these are
        // "extensions" managed through MSN, not generic modules browsed
        // here. stop/start/restart by name still work either way.
        if (strcmp(hdr->name, "meshtastic") == 0 || strcmp(hdr->name, "meshcore") == 0) continue;
        snprintf(line, sizeof(line), "  %-16s %-8s v%s",
                 hdr->name, module_type_name(hdr->module_type), hdr->version);
        purr_console_println(line);
    }
    const catcall_ui_t *ui = purr_kernel_ui();
    snprintf(line, sizeof(line), "  ui: %s", ui ? ui->name : "(none)");
    purr_console_println(line);
    const catcall_display_t *disp = purr_kernel_display();
    snprintf(line, sizeof(line), "  display: %s", disp ? disp->name : "(none)");
    purr_console_println(line);
}

static void svc_result(const char *name, int rc) {
    char buf[64];
    switch (rc) {
        case PURR_MODCTL_OK:              snprintf(buf, sizeof(buf), "%s: ok", name); break;
        case PURR_MODCTL_ERR_NOT_FOUND:   snprintf(buf, sizeof(buf), "%s: not found", name); break;
        case PURR_MODCTL_ERR_DENYLISTED:  snprintf(buf, sizeof(buf), "%s: refused (protected module)", name); break;
        case PURR_MODCTL_ERR_ALREADY:     snprintf(buf, sizeof(buf), "%s: already in that state", name); break;
        case PURR_MODCTL_ERR_INIT_FAILED: snprintf(buf, sizeof(buf), "%s: failed to start", name); break;
        default:                          snprintf(buf, sizeof(buf), "%s: error %d", name, rc); break;
    }
    purr_console_println(buf);
}

static void cmd_stop(const char *args) {
    if (!args || !*args) { purr_console_println("stop: missing module name"); return; }
    svc_result(args, purr_kernel_module_set_enabled(args, false));
}

static void cmd_start(const char *args) {
    if (!args || !*args) { purr_console_println("start: missing module name"); return; }
    svc_result(args, purr_kernel_module_set_enabled(args, true));
}

static void cmd_restart(const char *args) {
    if (!args || !*args) { purr_console_println("restart: missing module name"); return; }
    svc_result(args, purr_kernel_module_restart(args));
}

// `startui <name>` — same purr_kernel_module_set_enabled() choke point
// `start`/`stop` already use above; a distinct, self-documenting name for
// the specific "I'm at a boot shell, now bring the graphical UI up"
// moment the modular-core/Unix-boot plan is built around, not a new
// mechanism. `start mochi` does the exact same thing today; this exists
// so the command someone actually reaches for at that moment says what
// they mean.
static void cmd_startui(const char *args) {
    if (!args || !*args) { purr_console_println("startui: missing UI module name (e.g. 'startui mochi')"); return; }
    svc_result(args, purr_kernel_module_set_enabled(args, true));
}

// Registered hook, not a direct app_manager_launch_by_name() call — see
// purr_console_exec_fn's own doc comment for why (same reasoning as the
// login gate: this component must not carry an unconditional link-time
// dependency on app_manager just to expose this one command).
static void cmd_exec(const char *args) {
    if (!s_exec_fn) { purr_console_println("exec: not available on this build"); return; }
    s_exec_fn(args ? args : "");
}

static void cmd_mem(const char *args) {
    (void)args;
    char buf[48];
    snprintf(buf, sizeof(buf), "Free RAM: %lu bytes", (unsigned long)purr_kernel_free_ram());
    purr_console_println(buf);
}

static void cmd_uptime(const char *args) {
    (void)args;
    char buf[48];
    snprintf(buf, sizeof(buf), "Uptime: %llu s",
             (unsigned long long)(purr_kernel_uptime_ms() / 1000ULL));
    purr_console_println(buf);
}

static void cmd_clear(const char *args) {
    (void)args;
    // ANSI clear-screen + home-cursor. terminal.c's own cmd_clear() cleared
    // an LVGL textarea widget instead — no such widget exists on a raw
    // byte-stream console, so this is the standard real-terminal
    // equivalent. A terminal that doesn't understand ANSI escapes prints a
    // few harmless stray characters, same tradeoff every other tool
    // emitting "clear" over a serial line already accepts.
    purr_console_print("\x1b[2J\x1b[H");
}

static void cmd_reboot(const char *args) {
    (void)args;
    purr_console_println("Rebooting...");
    s_io->flush();
    purr_kernel_reboot();
}

// Ported from each kernel_*_boot.c's own kb_test_loop(), generalized off
// UART_NUM_0 onto whichever io is currently bound.
static void cmd_kb(const char *args) {
    (void)args;
    const catcall_input_t *kbd = purr_kernel_input();
    if (!kbd) {
        purr_console_println("[KB TEST] no keyboard catcall — driver not ready");
        return;
    }
    purr_console_println("[KB TEST] press keys on the device keyboard. type 'q' here to exit.");
    for (;;) {
        int c = s_io->read_byte(0);   // non-blocking poll — see the loop below's own delay
        if (c == 'q' || c == 3) break;
        input_event_t ev;
        while (kbd->poll_event(&ev)) {
            if (ev.type == INPUT_EVENT_KEY_DOWN && ev.keycode) {
                uint16_t k = ev.keycode;
                char line[32];
                if (k >= 0x20 && k <= 0x7E)
                    snprintf(line, sizeof(line), "[KB] '%c' (0x%02X)", (char)k, k);
                else
                    snprintf(line, sizeof(line), "[KB] 0x%02X", k);
                purr_console_println(line);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    purr_console_println("[KB TEST] done.");
}

static const purr_console_cmd_t s_builtin_cmds[] = {
    { "help",    cmd_help,    "this message" },
    { "ls",      cmd_ls,      "[path]  list directory" },
    { "cat",     cmd_cat,     "<path>  print file" },
    { "echo",    cmd_echo,    "<text>  print text" },
    { "modules", cmd_modules, "list loaded modules" },
    { "stop",    cmd_stop,    "<name>  disable a module" },
    { "start",   cmd_start,   "<name>  enable a module" },
    { "restart", cmd_restart, "<name>  disable+enable a module" },
    { "startui", cmd_startui, "<name>  enable a UI module" },
    { "exec",    cmd_exec,    "<app>   launch an app by name" },
    { "mem",     cmd_mem,     "free RAM" },
    { "uptime",  cmd_uptime,  "uptime in seconds" },
    { "kb",      cmd_kb,      "echo keyboard keypresses (q to exit)" },
    { "clear",   cmd_clear,   "clear screen" },
    { "reboot",  cmd_reboot,  "reboot device" },
};
#define BUILTIN_COUNT (sizeof(s_builtin_cmds) / sizeof(s_builtin_cmds[0]))

void purr_console_register_commands(const purr_console_cmd_t *cmds, int count) {
    if (!cmds || count <= 0) return;
    if (s_extra_table_count >= MAX_EXTRA_TABLES) {
        ESP_LOGE(TAG, "purr_console_register_commands: table slots full (max %d)", MAX_EXTRA_TABLES);
        return;
    }
    s_extra_tables[s_extra_table_count].cmds  = cmds;
    s_extra_tables[s_extra_table_count].count = count;
    s_extra_table_count++;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

static void run_command(char *buf) {
    // Trim trailing newline/space
    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = '\0';

    if (!*buf) return;

    // Split verb + args
    char *space = strchr(buf, ' ');
    const char *args = space ? (space + 1) : "";
    if (space) *space = '\0';
    const char *verb = buf;

    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(verb, s_builtin_cmds[i].name) == 0) {
            s_builtin_cmds[i].fn(args);
            return;
        }
    }
    for (int t = 0; t < s_extra_table_count; t++) {
        for (int i = 0; i < s_extra_tables[t].count; i++) {
            if (strcmp(verb, s_extra_tables[t].cmds[i].name) == 0) {
                s_extra_tables[t].cmds[i].fn(args);
                return;
            }
        }
    }

    char err[CMD_MAX + 24];
    snprintf(err, sizeof(err), "unknown command: %s", verb);
    purr_console_println(err);
}

// ── Line editing ─────────────────────────────────────────────────────────────
//
// Matches every serial_console_task() this replaces: '\r'/'\n' submits,
// 0x7F/'\b' backspaces, printable bytes (>=0x20) are appended and (unless
// echo is suppressed, see purr_console_set_echo()) echoed back — the
// terminal on the other end is what actually renders them, this module
// never assumes a specific one. Shared by the REPL loop below and by any
// registered login_fn prompting for a username/password on the same io.

size_t purr_console_read_line(const purr_console_io_t *io, char *buf, size_t bufsz) {
    size_t len = 0;
    for (;;) {
        // Checked BETWEEN io->read_byte() calls, never inside one — see
        // purr_console_request_stop()'s own doc comment for why that
        // matters. An empty line here just means "the caller's loop
        // should re-check and park," not a real submitted line.
        if (s_stop_requested) { buf[0] = '\0'; return 0; }
        int c = io->read_byte(50);
        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            purr_console_print("\r\n");
            buf[len] = '\0';
            return len;
        } else if ((c == 0x7F || c == '\b') && len > 0) {
            len--;
            if (s_echo) purr_console_print("\b \b");
        } else if (len < bufsz - 1 && c >= 0x20) {
            buf[len++] = (char)c;
            if (s_echo) { char ch = (char)c; io->write(&ch, 1); }
        }
    }
}

// ── Read-eval-print loop ─────────────────────────────────────────────────────

void purr_console_run(const purr_console_io_t *io, bool with_login) {
    if (!io || !io->read_byte || !io->write || !io->flush) return;
    s_io = io;
    s_stop_requested = false;   // a fresh session always starts clean,
                                // even if a PREVIOUS one on this io was
                                // stopped via purr_console_request_stop()

    // See purr_console_run()'s own doc comment in the header: with_login
    // is what guarantees a panic-context caller (with_login=false, always)
    // never reaches a login_fn a normal boot registered earlier, even
    // though the registration itself is unconditional global state.
    if (with_login && s_login_fn) s_login_fn(io);

    char line[CMD_MAX];

    purr_console_print("\r\nPURR OS console\r\n> ");

    for (;;) {
        purr_console_read_line(io, line, sizeof(line));
        if (s_stop_requested) {
            // See purr_console_request_stop()'s own doc comment — park
            // here, touching `io` no further, rather than returning (there
            // is nothing meaningful for this task to do afterward either
            // way, and returning would just fall off the end of whatever
            // task function called this).
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        run_command(line);
        purr_console_print("> ");
    }
}
