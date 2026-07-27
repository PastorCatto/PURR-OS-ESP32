// magidos_app.c — MagiDOS host: game-mode shell for the 8086 emulator.
//
// Replaces the pre-0.11 purr_wm_app/magidos_app.cpp, which opened a draggable
// MiniWin window and rendered CGA into it. That window manager no longer exists,
// and under game mode there is nothing to be a window inside of — MagiDOS owns
// the whole panel. The replacement is smaller than what it replaces.
//
// ── Why the shell is host-side ──────────────────────────────────────────────
// The prompt below is C running on the ESP32, not an 8086 program. That is a
// deliberate staging decision, not a shortcut:
//
// A guest-side COMMAND.COM would have to launch programs through INT 21h
// AH=4Bh (EXEC), which is the deep end of DOS — memory control blocks, PSP
// construction, environment blocks, parent/child return. A host-side shell
// sidesteps all of it: the host loads the program itself, runs the CPU until
// it exits, and redraws the prompt. Programs that shell out won't work yet,
// which almost nothing simple does.
//
// It is also written to LOOK like COMMAND.COM, so replacing it with a real
// guest shell later is invisible to whoever is typing at it.
//
// ── Drive mapping ───────────────────────────────────────────────────────────
// C: maps to MAGIDOS_C_HOST on the SD card, DOSBox-style — the drive letter is
// a path prefix, not a disk image, so a file dropped into that folder simply
// appears. See dos_to_host().

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "purr_kernel.h"
#include "purr_module.h"
#include "catcall_display.h"
#include "catcall_input.h"
#include "game_mode.h"
#include "app_manager.h"
#include "magidos_cga.h"

static const char *TAG = "magidos";

// Where C: lives. A folder, not an image — see this file's header.
#define MAGIDOS_C_HOST  "/sdcard/dos"

#define CGA_COLS 80
#define CGA_ROWS 25

// CGA text plane: char + attribute per cell, exactly the layout the emulator's
// own video RAM uses, so handing the emulator's VRAM to the same renderer later
// needs no translation.
static uint8_t  s_vram[CGA_COLS * CGA_ROWS * 2];
static int      s_cx = 0, s_cy = 0;
static uint8_t  s_attr = 0x07;          // light grey on black, DOS default

static uint16_t *s_fb = NULL;           // RGB565 framebuffer, screen-sized
static uint16_t  s_scr_w = 320, s_scr_h = 240;

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;

// Current working directory as DOS sees it, always relative to C:\ and always
// WITHOUT a trailing separator. "" means the root.
static char s_cwd[128] = "";

// ── CGA text plane ──────────────────────────────────────────────────────────

static void scr_clear(void)
{
    for (int i = 0; i < CGA_COLS * CGA_ROWS; i++) {
        s_vram[i * 2 + 0] = ' ';
        s_vram[i * 2 + 1] = s_attr;
    }
    s_cx = s_cy = 0;
}

static void scr_scroll(void)
{
    memmove(s_vram, s_vram + CGA_COLS * 2, (size_t)(CGA_COLS * (CGA_ROWS - 1) * 2));
    uint8_t *last = s_vram + CGA_COLS * (CGA_ROWS - 1) * 2;
    for (int i = 0; i < CGA_COLS; i++) { last[i * 2] = ' '; last[i * 2 + 1] = s_attr; }
    if (s_cy > 0) s_cy--;
}

static void scr_putc(char c)
{
    if (c == '\n') { s_cx = 0; s_cy++; }
    else if (c == '\r') { s_cx = 0; }
    else if (c == '\b') { if (s_cx > 0) { s_cx--; s_vram[(s_cy * CGA_COLS + s_cx) * 2] = ' '; } }
    else {
        int off = (s_cy * CGA_COLS + s_cx) * 2;
        s_vram[off + 0] = (uint8_t)c;
        s_vram[off + 1] = s_attr;
        s_cx++;
    }
    if (s_cx >= CGA_COLS) { s_cx = 0; s_cy++; }
    while (s_cy >= CGA_ROWS) scr_scroll();
}

static void scr_puts(const char *s) { while (*s) scr_putc(*s++); }

static void scr_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    scr_puts(buf);
}

// Push the whole text plane. One full-screen push per redraw rather than
// per-cell: at 320x240 that is 153,600 bytes, which the st7789 driver sends as
// 10 chunked transactions in ~15ms — cheaper than the bookkeeping needed to
// track and coalesce dirty cells, and the shell only redraws on input anyway.
static void scr_present(void)
{
    const catcall_display_t *d = purr_kernel_display();
    if (!d || !d->push_pixels || !s_fb) return;
    magidos_cga_render(s_vram, CGA_COLS, CGA_ROWS, s_fb, s_scr_w, s_scr_h);
    d->push_pixels(0, 0, s_scr_w, s_scr_h, s_fb);
}

// ── Drive mapping ───────────────────────────────────────────────────────────

// "C:\GAMES\X.EXE", "\GAMES\X.EXE" or "X.EXE" -> "/sdcard/dos/GAMES/X.EXE".
//
// Only C: exists for now. Anything else is rejected rather than silently
// treated as C:, so a program asking for A: gets a clean "invalid drive"
// instead of mysteriously reading the wrong disk.
static bool dos_to_host(const char *dos, char *out, size_t out_sz)
{
    const char *p = dos;
    bool absolute = false;

    if (p[0] && p[1] == ':') {
        if (toupper((unsigned char)p[0]) != 'C') return false;
        p += 2;
        absolute = true;
    }
    if (*p == '\\' || *p == '/') { absolute = true; p++; }

    int n;
    if (absolute) n = snprintf(out, out_sz, "%s/%s", MAGIDOS_C_HOST, p);
    else if (s_cwd[0]) n = snprintf(out, out_sz, "%s/%s/%s", MAGIDOS_C_HOST, s_cwd, p);
    else n = snprintf(out, out_sz, "%s/%s", MAGIDOS_C_HOST, p);
    if (n < 0 || (size_t)n >= out_sz) return false;

    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
    return true;
}

// ── Shell commands ──────────────────────────────────────────────────────────

static void cmd_dir(const char *arg)
{
    (void)arg;
    char host[192];
    if (!dos_to_host("", host, sizeof(host))) { scr_puts("Invalid path\n"); return; }

    DIR *dir = opendir(host);
    if (!dir) {
        scr_printf("Cannot open C:\\%s\n", s_cwd);
        scr_puts("(is the SD card mounted, and does " MAGIDOS_C_HOST " exist?)\n");
        return;
    }
    scr_printf(" Directory of C:\\%s\n\n", s_cwd);

    int files = 0, dirs = 0;
    long total = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        // Bounded: host is already up to 192 bytes and a filename can be long,
        // so the compiler is right that this could truncate. A truncated path
        // just fails the stat() below and the entry is skipped, which is the
        // correct outcome for a name too long to address anyway.
        char full[416];
        snprintf(full, sizeof(full), "%.200s/%.200s", host, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scr_printf("%-14s <DIR>\n", e->d_name); dirs++; }
        else { scr_printf("%-14s %8ld\n", e->d_name, (long)st.st_size); files++; total += st.st_size; }
    }
    closedir(dir);
    scr_printf("\n%8d file(s) %10ld bytes\n%8d dir(s)\n", files, total, dirs);
}

static void cmd_cd(const char *arg)
{
    if (!arg || !*arg) { scr_printf("C:\\%s\n", s_cwd); return; }
    if (strcmp(arg, "\\") == 0 || strcmp(arg, "/") == 0) { s_cwd[0] = '\0'; return; }
    if (strcmp(arg, "..") == 0) {
        char *slash = strrchr(s_cwd, '/');
        if (slash) *slash = '\0'; else s_cwd[0] = '\0';
        return;
    }

    // Validate before committing, so a typo leaves the cwd where it was rather
    // than moving into a directory that does not exist.
    //
    // Both snprintf()s bound their inputs explicitly. Letting them truncate
    // silently would be worse than a long path failing: a cut-short directory
    // name can still name a REAL directory, so the user would land somewhere
    // plausible but wrong.
    char probe[320];
    char want[160];
    if (s_cwd[0]) snprintf(want, sizeof(want), "%.100s/%.50s", s_cwd, arg);
    else          snprintf(want, sizeof(want), "%.150s", arg);

    snprintf(probe, sizeof(probe), "%s/%.200s", MAGIDOS_C_HOST, want);
    for (char *q = probe; *q; q++) if (*q == '\\') *q = '/';

    struct stat st;
    if (stat(probe, &st) != 0 || !S_ISDIR(st.st_mode)) { scr_puts("Invalid directory\n"); return; }
    // want is deliberately larger than s_cwd (it is built from s_cwd plus a new
    // component), so this last copy is where a too-deep path is rejected rather
    // than silently clipped into a shorter, real, wrong directory.
    if (strlen(want) >= sizeof(s_cwd)) { scr_puts("Path too long\n"); return; }
    snprintf(s_cwd, sizeof(s_cwd), "%.127s", want);
    for (char *q = s_cwd; *q; q++) if (*q == '\\') *q = '/';
}

static void cmd_type(const char *arg)
{
    if (!arg || !*arg) { scr_puts("Required parameter missing\n"); return; }
    char host[192];
    if (!dos_to_host(arg, host, sizeof(host))) { scr_puts("Invalid path\n"); return; }
    FILE *f = fopen(host, "rb");
    if (!f) { scr_puts("File not found\n"); return; }
    int c;
    while ((c = fgetc(f)) != EOF) scr_putc((char)c);
    fclose(f);
    scr_putc('\n');
}

static void cmd_ver(void)
{
    scr_puts("\nMagiDOS [PurrDOS " PURR_KERNEL_VERSION "]\n");
    scr_puts("8086 emulation on PURR OS. C: is " MAGIDOS_C_HOST "\n\n");
}

static void cmd_help(void)
{
    scr_puts("\nDIR          list the current directory\n");
    scr_puts("CD [dir]     change directory (.. and \\ work)\n");
    scr_puts("TYPE file    print a file\n");
    scr_puts("CLS          clear the screen\n");
    scr_puts("VER          version\n");
    scr_puts("MEM          free memory\n");
    scr_puts("EXIT         leave MagiDOS\n\n");
}

static void cmd_mem(void)
{
    scr_printf("\n%8u bytes free internal\n%8u bytes free PSRAM\n\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Returns false when the shell should exit.
static bool run_line(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return true;

    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }

    for (char *p = line; *p; p++) *p = (char)toupper((unsigned char)*p);

    if      (!strcmp(line, "DIR"))  cmd_dir(arg);
    else if (!strcmp(line, "CD"))   cmd_cd(arg);
    else if (!strcmp(line, "CHDIR"))cmd_cd(arg);
    else if (!strcmp(line, "TYPE")) cmd_type(arg);
    else if (!strcmp(line, "CLS"))  scr_clear();
    else if (!strcmp(line, "VER"))  cmd_ver();
    else if (!strcmp(line, "MEM"))  cmd_mem();
    else if (!strcmp(line, "HELP")) cmd_help();
    else if (!strcmp(line, "EXIT")) return false;
    else scr_printf("Bad command or file name - %s\n", line);
    return true;
}

// ── Keyboard ────────────────────────────────────────────────────────────────

// Resolved by capability, not by name — a keyboard-class driver implements
// set_backlight (bbq20's under-key LEDs), a trackball does not. Same test
// mochi_hal.c uses. purr_kernel_input() is NOT usable here: it returns the
// FIRST registered input, which on T-Deck Plus is the trackball.
static const catcall_input_t *find_keyboard(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (in && in->poll_event && in->set_backlight) return in;
    }
    return NULL;
}

// ── Task ────────────────────────────────────────────────────────────────────

static void magidos_task(void *arg)
{
    (void)arg;

    // Game mode is entered from THIS task, never from init(). init() may be
    // called on the UI render task, and entering game mode unloads the UI
    // backend — which would delete the very task making the call.
    purr_game_mode_enter("MagiDOS");

    const catcall_display_t *d = purr_kernel_display();
    if (d && d->get_info) {
        display_info_t info;
        d->get_info(&info);
        s_scr_w = info.width;
        s_scr_h = info.height;
    }

    // PSRAM: this is a pure pixel payload handed to push_pixels, which is
    // exactly what PSRAM is abundant for here. Internal DRAM is the scarce
    // resource and must not be spent on it.
    //
    // ZEROED, not just allocated. The CGA grid does not cover the panel: cell
    // height is out_h / rows, so at 240px and 25 rows that is 9px per row and
    // 9 x 25 = 225 — the bottom 15 rows are never written by the renderer. An
    // uninitialised buffer left those rows showing whatever PSRAM happened to
    // hold, which appeared on hardware as a band of colour static along the
    // bottom edge, coincidentally about the height of the nav bar. Clearing
    // once is enough: the renderer covers rows 0..224 on every frame and never
    // touches the remainder, so it stays black.
    s_fb = heap_caps_calloc(1, (size_t)s_scr_w * s_scr_h * 2, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%ux%u)", s_scr_w, s_scr_h);
        purr_game_mode_exit();
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    const catcall_input_t *kbd = find_keyboard();

    scr_clear();
    cmd_ver();
    if (!kbd) scr_puts("No keyboard driver found - type EXIT is unavailable.\n");
    scr_printf("C:\\%s>", s_cwd);
    scr_present();

    char line[128];
    int  len = 0;

    while (s_running) {
        // The only thing watching the device while the UI is unloaded.
        purr_game_mode_heartbeat();

        bool dirty = false;
        input_event_t ev;
        while (kbd && kbd->poll_event(&ev)) {
            if (ev.type != INPUT_EVENT_KEY_DOWN) continue;
            uint8_t c = (uint8_t)ev.keycode;

            if (c == '\r' || c == '\n') {
                scr_putc('\n');
                line[len] = '\0';
                len = 0;
                if (!run_line(line)) { s_running = false; break; }
                scr_printf("C:\\%s>", s_cwd);
                dirty = true;
            } else if (c == '\b' || c == 0x7F) {
                if (len > 0) { len--; scr_putc('\b'); dirty = true; }
            } else if (c >= 0x20 && c < 0x7F) {
                if (len < (int)sizeof(line) - 1) { line[len++] = (char)c; scr_putc((char)c); dirty = true; }
            }
        }

        if (dirty) scr_present();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    heap_caps_free(s_fb);
    s_fb = NULL;

    // Restores every module game mode unloaded, with its own progress splash.
    purr_game_mode_exit();

    // Tell app_manager we are done. It tracks running native apps by task
    // handle, and this task is about to delete itself — without this the slot
    // stays occupied, and the next tap on the MagiDOS icon takes the "already
    // running, re-show its window" path. This app owns the panel directly and
    // has no window, so that path does nothing at all: the launcher just sits
    // there and nothing unloads. Observed exactly that on the second launch.
    app_manager_notify_exited();

    s_task = NULL;
    vTaskDelete(NULL);
}

// ── Module ──────────────────────────────────────────────────────────────────

static int magidos_init(void)
{
    if (s_task) return 0;
    s_running = true;
    // Own task, and init() returns immediately. See magidos_task()'s first
    // comment for why game mode must not be entered from here.
    if (xTaskCreatePinnedToCore(magidos_task, "magidos", 8192, NULL, 4, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_running = false;
        return -1;
    }
    return 0;
}

static void magidos_deinit(void)
{
    s_running = false;
    // The task tears down game mode and deletes itself; wait for it rather than
    // vTaskDelete()ing from outside, which would strand the OS with every
    // module still unloaded.
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

PURR_MODULE_REGISTER(magidos) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "magidos",
    .version           = "0.2.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = magidos_init,
    .deinit            = magidos_deinit,
};
