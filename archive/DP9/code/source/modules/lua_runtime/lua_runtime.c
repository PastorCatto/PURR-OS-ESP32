// lua_runtime.c — Lua 5.4 scripting runtime for PURR OS (.meow/.hiss apps)
//
// One global Lua state, matching PURR-OS-0.11's design (only one Lua VM
// runs at a time on these boards) — adapted from that version's
// lua_runtime.cpp, but plain C and bound to purr_win.h/purr_kernel.h
// instead of KITT's C++ singleton + hardware-specific display/touch calls.
//
// Threading model: a .meow script's "main body" runs synchronously inside
// meow_task() (app_manager.c), which self-deletes once init() (this file's
// lua_runtime_init()) returns — so by the time any win.* button callback
// can fire (dispatched from the UI backend's own render/pump task), the
// script's own task is already gone. Scripts that build a UI and return
// (rather than looping) never have two tasks touching the single lua_State
// at once; this runtime does not support scripts that both loop forever
// AND register button callbacks, since the UI task's callback trampoline
// isn't synchronized against a running lua_pcall() on another task.

#include "lua_runtime.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "app_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

// Only exists when meshtastic is actually compiled in (mesh_radio.c's own
// implementation is entirely #ifdef CONFIG_PURR_FEATURE_MESHTASTIC-gated) —
// this module is shared across every device, including ones with meshtastic
// disabled, so every use below is guarded the same way.
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
#include "../meshtastic/mesh_radio.h"
#define LUA_RADIO_LOCK()   mesh_radio_lock()
#define LUA_RADIO_UNLOCK() mesh_radio_unlock()
#else
#define LUA_RADIO_LOCK()   ((void)0)
#define LUA_RADIO_UNLOCK() ((void)0)
#endif

static const char *TAG = "lua_rt";

static lua_State *s_L = NULL;

// ── system.* ──────────────────────────────────────────────────────────────────

static int lua_system_time_ms(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000LL));
    return 1;
}

static int lua_system_delay(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    vTaskDelay(pdMS_TO_TICKS(ms < 1 ? 1 : (uint32_t)ms));
    return 0;
}

static int lua_system_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    ESP_LOGI(TAG, "%s", s);
    return 0;
}

static void register_system_module(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_system_time_ms); lua_setfield(L, -2, "time_ms");
    lua_pushcfunction(L, lua_system_delay);   lua_setfield(L, -2, "delay");
    lua_pushcfunction(L, lua_system_print);   lua_setfield(L, -2, "print");
    lua_setglobal(L, "system");
}

// ── sd.* ──────────────────────────────────────────────────────────────────────
// Works against /flash or /sdcard like any other PURR OS app — no separate
// "restricted" tier distinction here (that's app_manager.c's .meow/.paws/
// .claw file-extension tiering, applied at the file-manager/app-scan level,
// not re-implemented inside the VM).
//
// fopen()/fread()/fwrite() must never run directly on meow_task's own
// stack — meow_task is deliberately PSRAM-backed (see launch_meow()'s
// comment in app_manager.c), and a PSRAM-stacked task touching flash briefly
// disables the cache that makes PSRAM reachable at all, confirmed live
// elsewhere in this codebase to crash with
// esp_task_stack_is_sane_cache_disabled() (the same reason settings/fileman
// get their own dedicated static internal-RAM stacks in app_manager.c).
// Rather than giving meow_task itself a static stack — which would cost
// every .meow script the same tight internal-DRAM budget regardless of
// whether it ever touches sd.* — a small dedicated worker task with a
// static internal-RAM stack does the actual I/O, and lua_sd_read()/
// lua_sd_write() just hand it the request and block until it's done. Only
// one Lua VM runs at a time (this file's top comment), so a single-slot
// request/response pair with no extra locking is safe.
// Internal SRAM on this board is under heavy static pressure (three of
// these dedicated static stacks plus Cupcake's ~37KB of LVGL display
// buffers leave only a few KB free at boot — see app_manager.c's own
// settings/fileman static stacks for the matching constraint), so unlike
// those two — whose native_task() runs a whole app's init(), including
// building its LVGL UI through purr_win.h — this one only ever runs
// lua_io_task()'s narrow fopen()/fread()/fwrite()/malloc() body, nothing
// deeper. 4096 instead of blindly matching their 8192. lua_io_task() logs a
// warning itself if the real high-water mark ever gets uncomfortably close
// to this, so an unsafe cut here won't fail silently.
#define LUA_IO_STACK_SIZE 4096
static StackType_t   s_lua_io_stack[LUA_IO_STACK_SIZE];
static StaticTask_t  s_lua_io_tcb;
static TaskHandle_t  s_lua_io_task     = NULL;
static SemaphoreHandle_t s_lua_io_req_sem  = NULL;
static SemaphoreHandle_t s_lua_io_resp_sem = NULL;

typedef struct {
    bool        is_write;
    const char *path;
    const char *write_data;
    size_t      write_len;
    char       *read_buf;
    size_t      read_len;
    bool        ok;
} lua_io_req_t;

static lua_io_req_t s_lua_io_req;

static void lua_io_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lua_io_req_sem, portMAX_DELAY);
        lua_io_req_t *r = &s_lua_io_req;
        if (r->is_write) {
            FILE *f = fopen(r->path, "wb");
            if (!f) {
                r->ok = false;
            } else {
                size_t nwritten = fwrite(r->write_data, 1, r->write_len, f);
                fclose(f);
                r->ok = (nwritten == r->write_len);
            }
        } else {
            r->read_buf = NULL;
            r->read_len = 0;
            FILE *f = fopen(r->path, "rb");
            if (!f) {
                r->ok = false;
            } else {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *buf = (size >= 0) ? (char *)malloc((size_t)size + 1) : NULL;
                if (!buf) {
                    fclose(f);
                    r->ok = false;
                } else {
                    r->read_len = fread(buf, 1, (size_t)size, f);
                    fclose(f);
                    r->read_buf = buf;
                    r->ok = true;
                }
            }
        }

        // uxTaskGetStackHighWaterMark() reports the lowest free-stack point
        // ever seen on this task, in bytes (ESP-IDF's StackType_t is
        // byte-sized) — checked after every op rather than trusting
        // LUA_IO_STACK_SIZE's cut from 8192 down to 4096 blindly. 512 bytes
        // of headroom is the same rough safety margin app_manager.c's own
        // static-stack tasks operate with.
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        if (hw < 512) {
            ESP_LOGW(TAG, "lua_io stack low: %u bytes free (LUA_IO_STACK_SIZE may need raising)", (unsigned)hw);
        }

        xSemaphoreGive(s_lua_io_resp_sem);
    }
}

// Lazily starts the I/O worker on first sd.* call ever and leaves it running
// for the lifetime of the device — cheap (one static stack, one task) and
// avoids re-creating it on every script launch the way s_L itself is.
static void lua_io_ensure_started(void) {
    if (s_lua_io_task) return;
    s_lua_io_req_sem  = xSemaphoreCreateBinary();
    s_lua_io_resp_sem = xSemaphoreCreateBinary();
    s_lua_io_task = xTaskCreateStatic(lua_io_task, "lua_io", LUA_IO_STACK_SIZE,
                                       NULL, 4, s_lua_io_stack, &s_lua_io_tcb);
}

static int lua_sd_read(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_io_ensure_started();
    s_lua_io_req.is_write = false;
    s_lua_io_req.path     = path;
    xSemaphoreGive(s_lua_io_req_sem);
    xSemaphoreTake(s_lua_io_resp_sem, portMAX_DELAY);

    if (!s_lua_io_req.ok) { lua_pushnil(L); lua_pushstring(L, "file not found"); return 2; }
    lua_pushlstring(L, s_lua_io_req.read_buf, s_lua_io_req.read_len);
    free(s_lua_io_req.read_buf);
    return 1;
}

static int lua_sd_write(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    lua_io_ensure_started();
    s_lua_io_req.is_write    = true;
    s_lua_io_req.path        = path;
    s_lua_io_req.write_data  = data;
    s_lua_io_req.write_len   = len;
    xSemaphoreGive(s_lua_io_req_sem);
    xSemaphoreTake(s_lua_io_resp_sem, portMAX_DELAY);

    lua_pushboolean(L, s_lua_io_req.ok);
    if (!s_lua_io_req.ok) lua_pushstring(L, "cannot open file");
    return s_lua_io_req.ok ? 1 : 2;
}

static void register_sd_module(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_sd_read);  lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_sd_write); lua_setfield(L, -2, "write");
    lua_setglobal(L, "sd");
}

// ── radio.* / gps.* / kitt.* — .hiss tier only ────────────────────────────────
// Registered by register_kitt_module() below, called from lua_runtime_init()
// only when app_manager_get_pending_meow_privileged() is true. A plain
// .meow script never sees these globals at all — not hidden, just never
// added to its Lua state.

// mesh_radio_lock()/unlock() (meshtastic's mesh_radio.h) serialize every
// caller of the shared RadioLib Module/SX126x object — its own doc comment
// documents interleaved send/receive corrupting RadioLib's internal mode-
// tracking state as a confirmed live root cause of intermittent radio
// crashes. mesh_task() and mesh_router.c already take this lock around
// every radio catcall; this .hiss binding is a second, independent caller
// (runs on meow_task, pinned to the opposite CPU core from mesh_task) that
// previously called straight through purr_kernel_radio() with no locking
// at all — a genuine SMP race, not just scheduler jitter, since the two
// tasks can be mid-command-sequence on the shared chip at the same instant.
// No-op on targets without meshtastic compiled in (LUA_RADIO_LOCK() above).

static int lua_radio_send(lua_State *L) {
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->send) { lua_pushboolean(L, 0); lua_pushstring(L, "no radio"); return 2; }
    LUA_RADIO_LOCK();
    esp_err_t ret = radio->send((const uint8_t *)data, len);
    LUA_RADIO_UNLOCK();
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

static int lua_radio_receive(lua_State *L) {
    lua_Integer max_len = luaL_optinteger(L, 1, 256);
    if (max_len <= 0 || max_len > 1024) max_len = 256;
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->receive) { lua_pushnil(L); return 1; }
    uint8_t buf[1024];
    LUA_RADIO_LOCK();
    int n = radio->receive(buf, (size_t)max_len);
    LUA_RADIO_UNLOCK();
    if (n <= 0) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

static int lua_radio_available(lua_State *L) {
    const catcall_radio_t *radio = purr_kernel_radio();
    LUA_RADIO_LOCK();
    bool avail = radio && radio->data_available && radio->data_available();
    LUA_RADIO_UNLOCK();
    lua_pushboolean(L, avail);
    return 1;
}

static int lua_radio_rssi(lua_State *L) {
    const catcall_radio_t *radio = purr_kernel_radio();
    LUA_RADIO_LOCK();
    int rssi = (radio && radio->rssi) ? radio->rssi() : 0;
    LUA_RADIO_UNLOCK();
    lua_pushinteger(L, rssi);
    return 1;
}

static int lua_radio_snr(lua_State *L) {
    const catcall_radio_t *radio = purr_kernel_radio();
    LUA_RADIO_LOCK();
    lua_Number snr = (radio && radio->snr) ? (lua_Number)radio->snr() : 0.0;
    LUA_RADIO_UNLOCK();
    lua_pushnumber(L, snr);
    return 1;
}

static int lua_gps_fix(lua_State *L) {
    const catcall_gps_t *gps = purr_kernel_gps();
    if (!gps || !gps->get_fix) { lua_pushnil(L); return 1; }
    gps_fix_t fix = {0};
    bool has_fix = gps->get_fix(&fix);
    lua_newtable(L);
    lua_pushnumber(L,  fix.latitude);           lua_setfield(L, -2, "latitude");
    lua_pushnumber(L,  fix.longitude);          lua_setfield(L, -2, "longitude");
    lua_pushnumber(L,  fix.altitude_m);         lua_setfield(L, -2, "altitude_m");
    lua_pushnumber(L,  fix.speed_mps);          lua_setfield(L, -2, "speed_mps");
    lua_pushnumber(L,  fix.hdop);                lua_setfield(L, -2, "hdop");
    lua_pushinteger(L, fix.satellites);          lua_setfield(L, -2, "satellites");
    lua_pushboolean(L, has_fix && fix.valid);    lua_setfield(L, -2, "valid");
    return 1;
}

// Mirrors terminal.c's module_type_name() — small enough not to be worth
// sharing a header over.
static const char *lua_module_type_name(uint8_t type) {
    switch (type) {
        case PURR_MOD_DRIVER: return "driver";
        case PURR_MOD_SYSTEM: return "system";
        case PURR_MOD_UI:     return "ui";
        case PURR_MOD_APP:    return "app";
        default:              return "unknown";
    }
}

// kitt.breadcrumb()/klog_tail() — kernel-extension surface for .kitten
// scripts: read the kernel's log ring buffer and mark a custom diagnostic
// breadcrumb, the same primitives this session's C-level debugging used
// (purr_kernel_klog_tail(), purr_kernel_ui_breadcrumb()), now scriptable
// instead of requiring a firmware rebuild to add one more instrumentation
// point. See purr_kernel.h's doc comments for both.

// purr_kernel_ui_breadcrumb() stores the pointer as-is, no copy taken (see
// its header doc) — passing a Lua string's own pointer directly would leave
// a dangling reference the moment Lua's GC reclaims it, since nothing else
// keeps that string alive. Copied into a static C buffer instead, mirroring
// the same pattern used for the paint/control breadcrumbs added earlier
// this session (miniwin.c's z-order/control-index breadcrumbs).
static char s_kitt_breadcrumb_buf[64];

static int lua_kitt_breadcrumb(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    snprintf(s_kitt_breadcrumb_buf, sizeof(s_kitt_breadcrumb_buf), "kitten: %s", s);
    purr_kernel_ui_breadcrumb(s_kitt_breadcrumb_buf);
    return 0;
}

static int lua_kitt_klog_tail(lua_State *L) {
    char buf[1024];
    size_t n = purr_kernel_klog_tail(buf, sizeof(buf));
    lua_pushlstring(L, buf, n);
    return 1;
}

static int lua_kitt_modules(lua_State *L) {
    int n = purr_kernel_module_count();
    lua_newtable(L);
    int out_idx = 1;
    for (int i = 0; i < n; i++) {
        const purr_module_header_t *hdr = purr_kernel_module_at(i);
        if (!hdr) continue;
        lua_newtable(L);
        lua_pushstring(L, hdr->name);                          lua_setfield(L, -2, "name");
        lua_pushstring(L, lua_module_type_name(hdr->module_type)); lua_setfield(L, -2, "type");
        lua_pushstring(L, hdr->version);                       lua_setfield(L, -2, "version");
        lua_rawseti(L, -2, out_idx++);
    }
    return 1;
}

static void register_kitt_module(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_radio_send);      lua_setfield(L, -2, "send");
    lua_pushcfunction(L, lua_radio_receive);   lua_setfield(L, -2, "receive");
    lua_pushcfunction(L, lua_radio_available); lua_setfield(L, -2, "available");
    lua_pushcfunction(L, lua_radio_rssi);      lua_setfield(L, -2, "rssi");
    lua_pushcfunction(L, lua_radio_snr);       lua_setfield(L, -2, "snr");
    lua_setglobal(L, "radio");

    lua_newtable(L);
    lua_pushcfunction(L, lua_gps_fix); lua_setfield(L, -2, "fix");
    lua_setglobal(L, "gps");

    lua_newtable(L);
    lua_pushcfunction(L, lua_kitt_modules);    lua_setfield(L, -2, "modules");
    lua_pushcfunction(L, lua_kitt_breadcrumb); lua_setfield(L, -2, "breadcrumb");
    lua_pushcfunction(L, lua_kitt_klog_tail);  lua_setfield(L, -2, "klog_tail");
    lua_setglobal(L, "kitt");
}

// ── win.* ─────────────────────────────────────────────────────────────────────
// Thin Lua wrappers over purr_win.h — the same window/label/button/textarea
// API every native app uses, so a .meow script can build a simple UI.

// Button callbacks: the Lua function is stashed in the registry via
// luaL_ref(), the ref (cast to a pointer-sized int) is the callback's
// `user` — see the threading-model note at the top of this file for why
// this trampoline doesn't need its own locking.
static void lua_btn_trampoline(purr_wid_t wid, purr_event_t event, void *user) {
    (void)wid; (void)event;
    if (!s_L) return;
    int ref = (int)(intptr_t)user;
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "button callback error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
    }
}

// canvas_on_paint()/_on_touch() callbacks — same registry-ref trampoline
// pattern as lua_btn_trampoline() above. See purr_win_paint_cb_t's doc
// comment (catcall_ui.h) for why a widget-dense UI (a calculator keypad,
// say) should draw itself this way instead of one native win.button() per
// key: closing a window with ~20 native button controls was confirmed live
// to leave the UI task permanently stuck inside the backend's own
// control-teardown code. A window using canvas_on_paint()/canvas_on_touch()
// creates zero native controls, so it isn't exposed to that at all.
static void lua_paint_trampoline(purr_win_t win, void *user) {
    if (!s_L) return;
    int ref = (int)(intptr_t)user;
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(s_L, (lua_Integer)win);
    if (lua_pcall(s_L, 1, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "canvas paint callback error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
    }
}

static void lua_touch_trampoline(purr_win_t win, int16_t x, int16_t y, bool pressed, void *user) {
    if (!s_L) return;
    int ref = (int)(intptr_t)user;
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(s_L, (lua_Integer)win);
    lua_pushinteger(s_L, (lua_Integer)x);
    lua_pushinteger(s_L, (lua_Integer)y);
    lua_pushboolean(s_L, pressed);
    if (lua_pcall(s_L, 4, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "canvas touch callback error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
    }
}

static int lua_win_on_paint(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    purr_win_canvas_on_paint(win, lua_paint_trampoline, (void *)(intptr_t)ref);
    return 0;
}

static int lua_win_on_touch(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    purr_win_canvas_on_touch(win, lua_touch_trampoline, (void *)(intptr_t)ref);
    return 0;
}

static int lua_win_rect(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    int16_t x = (int16_t)luaL_checkinteger(L, 2);
    int16_t y = (int16_t)luaL_checkinteger(L, 3);
    int16_t w = (int16_t)luaL_checkinteger(L, 4);
    int16_t h = (int16_t)luaL_checkinteger(L, 5);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 6);
    purr_win_canvas_rect(win, x, y, w, h, color);
    return 0;
}

static int lua_win_text(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    int16_t x = (int16_t)luaL_checkinteger(L, 2);
    int16_t y = (int16_t)luaL_checkinteger(L, 3);
    const char *text = luaL_checkstring(L, 4);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 5);
    purr_win_canvas_text(win, x, y, text, color);
    return 0;
}

static int lua_win_repaint(lua_State *L) {
    purr_win_canvas_repaint((purr_win_t)luaL_checkinteger(L, 1));
    return 0;
}

static int lua_win_width(lua_State *L) {
    int16_t w = 0, h = 0;
    purr_win_canvas_size((purr_win_t)luaL_checkinteger(L, 1), &w, &h);
    lua_pushinteger(L, (lua_Integer)w);
    return 1;
}

static int lua_win_height(lua_State *L) {
    int16_t w = 0, h = 0;
    purr_win_canvas_size((purr_win_t)luaL_checkinteger(L, 1), &w, &h);
    lua_pushinteger(L, (lua_Integer)h);
    return 1;
}

static int lua_win_create(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)purr_win_create(title));
    return 1;
}

static int lua_win_destroy(lua_State *L) {
    purr_win_destroy((purr_win_t)luaL_checkinteger(L, 1));
    return 0;
}

static int lua_win_show(lua_State *L) {
    purr_win_show((purr_win_t)luaL_checkinteger(L, 1));
    return 0;
}

static int lua_win_label(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    const char *text = luaL_checkstring(L, 2);
    lua_pushinteger(L, (lua_Integer)purr_win_label(win, text));
    return 1;
}

static int lua_win_label_set(lua_State *L) {
    purr_wid_t wid = (purr_wid_t)luaL_checkinteger(L, 1);
    purr_win_label_set(wid, luaL_checkstring(L, 2));
    return 0;
}

static int lua_win_button(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    const char *text = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    purr_wid_t wid = purr_win_button(win, text, lua_btn_trampoline, (void *)(intptr_t)ref);
    lua_pushinteger(L, (lua_Integer)wid);
    return 1;
}

static int lua_win_textarea(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    lua_Integer w = luaL_optinteger(L, 2, 100);
    lua_Integer h = luaL_optinteger(L, 3, 60);
    lua_pushinteger(L, (lua_Integer)purr_win_textarea(win, (uint16_t)w, (uint16_t)h));
    return 1;
}

static int lua_win_textarea_set(lua_State *L) {
    purr_win_textarea_set((purr_wid_t)luaL_checkinteger(L, 1), luaL_checkstring(L, 2));
    return 0;
}

static int lua_win_textarea_get(lua_State *L) {
    const char *s = purr_win_textarea_get((purr_wid_t)luaL_checkinteger(L, 1));
    lua_pushstring(L, s ? s : "");
    return 1;
}

static int lua_win_label_align(lua_State *L) {
    purr_wid_t wid = (purr_wid_t)luaL_checkinteger(L, 1);
    // 0=left, 1=center, 2=right — matches purr_align_t (catcall_ui.h).
    purr_win_label_align(wid, (purr_align_t)luaL_checkinteger(L, 2));
    return 0;
}

// ── Layout containers ─────────────────────────────────────────────────────────
// Thin wrappers over purr_win_row/col/row_grow/col_grow/layout_end — needed
// for any script that lays out more than one widget per line (e.g. a
// button grid), since without a container every widget added to a window
// stacks in one single column.

static int lua_win_row(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)purr_win_row(win, (uint8_t)luaL_optinteger(L, 2, 4)));
    return 1;
}

static int lua_win_col(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)purr_win_col(win, (uint8_t)luaL_optinteger(L, 2, 4)));
    return 1;
}

static int lua_win_row_grow(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)purr_win_row_grow(win, (uint8_t)luaL_optinteger(L, 2, 4)));
    return 1;
}

static int lua_win_col_grow(lua_State *L) {
    purr_win_t win = (purr_win_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)purr_win_col_grow(win, (uint8_t)luaL_optinteger(L, 2, 4)));
    return 1;
}

static int lua_win_layout_end(lua_State *L) {
    purr_win_layout_end((purr_wid_t)luaL_checkinteger(L, 1));
    return 0;
}

static void register_win_module(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_win_create);        lua_setfield(L, -2, "create");
    lua_pushcfunction(L, lua_win_destroy);       lua_setfield(L, -2, "destroy");
    lua_pushcfunction(L, lua_win_show);          lua_setfield(L, -2, "show");
    lua_pushcfunction(L, lua_win_label);         lua_setfield(L, -2, "label");
    lua_pushcfunction(L, lua_win_label_set);     lua_setfield(L, -2, "label_set");
    lua_pushcfunction(L, lua_win_label_align);   lua_setfield(L, -2, "label_align");
    lua_pushcfunction(L, lua_win_button);        lua_setfield(L, -2, "button");
    lua_pushcfunction(L, lua_win_textarea);      lua_setfield(L, -2, "textarea");
    lua_pushcfunction(L, lua_win_textarea_set);  lua_setfield(L, -2, "textarea_set");
    lua_pushcfunction(L, lua_win_textarea_get);  lua_setfield(L, -2, "textarea_get");
    lua_pushcfunction(L, lua_win_row);           lua_setfield(L, -2, "row");
    lua_pushcfunction(L, lua_win_col);           lua_setfield(L, -2, "col");
    lua_pushcfunction(L, lua_win_row_grow);      lua_setfield(L, -2, "row_grow");
    lua_pushcfunction(L, lua_win_col_grow);      lua_setfield(L, -2, "col_grow");
    lua_pushcfunction(L, lua_win_layout_end);    lua_setfield(L, -2, "layout_end");
    lua_pushcfunction(L, lua_win_on_paint);      lua_setfield(L, -2, "on_paint");
    lua_pushcfunction(L, lua_win_on_touch);      lua_setfield(L, -2, "on_touch");
    lua_pushcfunction(L, lua_win_rect);          lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, lua_win_text);          lua_setfield(L, -2, "text");
    lua_pushcfunction(L, lua_win_repaint);       lua_setfield(L, -2, "repaint");
    lua_pushcfunction(L, lua_win_width);         lua_setfield(L, -2, "width");
    lua_pushcfunction(L, lua_win_height);        lua_setfield(L, -2, "height");
    lua_setglobal(L, "win");
}

// ── Run ───────────────────────────────────────────────────────────────────────

bool lua_run_file(const char *path) {
    if (!s_L) { ESP_LOGE(TAG, "runtime not initialized"); return false; }

    ESP_LOGI(TAG, "loading %s", path);
    if (luaL_loadfile(s_L, path) != LUA_OK) {
        ESP_LOGE(TAG, "load error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        return false;
    }
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "runtime error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        return false;
    }
    return true;
}

bool lua_run_code(const char *code, const char *name) {
    if (!s_L) { ESP_LOGE(TAG, "runtime not initialized"); return false; }

    if (luaL_loadbuffer(s_L, code, strlen(code), name) != LUA_OK) {
        ESP_LOGE(TAG, "compile error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        return false;
    }
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "runtime error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        return false;
    }
    return true;
}

// ── Module lifecycle ──────────────────────────────────────────────────────────

int lua_runtime_init(void) {
    // This init() runs twice in practice: once at boot, like every other
    // PURR_MOD_SYSTEM module (harmless no-op — no .meow launch is pending
    // yet), and again each time app_manager's launch_meow() actually
    // launches a .meow script. Check for pending work before allocating a
    // Lua state at all, so the boot-time call doesn't leak one.
    const char *code = app_manager_get_pending_meow_code(NULL);
    const char *path = app_manager_get_pending_meow_path();
    if (!code && (!path || !path[0])) {
        return 0;   // nothing pending — not an error, just not our turn yet
    }

    if (s_L) lua_runtime_deinit();   // previous script's VM, if any

    s_L = luaL_newstate();
    if (!s_L) { ESP_LOGE(TAG, "failed to create Lua state"); return -1; }

    luaL_openlibs(s_L);
    register_system_module(s_L);
    register_sd_module(s_L);
    register_win_module(s_L);
    // .hiss only — see app_manager.h's app_tier_t doc and this file's
    // register_kitt_module() comment. A .meow script never gets these
    // globals registered into its Lua state at all.
    if (app_manager_get_pending_meow_privileged()) {
        register_kitt_module(s_L);
    }

    // Preferred path: app_manager already read the script into a PSRAM
    // buffer (see launch_meow()'s comment) so this never has to fopen()
    // anything itself, which is what let meow_task() move back onto a
    // PSRAM-backed stack. lua_run_file() stays as a fallback/general-purpose
    // entry point (e.g. a future terminal/REPL caller) — this file's own
    // luaL_loadfile() path, not something app_manager's launch flow uses
    // anymore.
    bool ok = code ? lua_run_code(code, (path && path[0]) ? path : "meow")
                    : lua_run_file(path);
    return ok ? 0 : -1;
}

void lua_runtime_deinit(void) {
    if (s_L) {
        lua_close(s_L);
        s_L = NULL;
    }
}

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(lua_runtime) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "lua_runtime",
    .version           = "1.0.1",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = lua_runtime_init,
    .deinit            = lua_runtime_deinit,
};
