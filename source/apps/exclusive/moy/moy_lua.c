// moy_lua.c — the Lua 5.4 sandbox and the console verbs (spec 4, 6-9).
//
// Lua is vendored privately into this component (see CMakeLists.txt), the same
// way lua_runtime vendors it. That is what lets a cart run under speed demon:
// speed demon unloads the lua_runtime MODULE, but this interpreter belongs to
// the app and nothing takes it away.
//
// ── Sandbox (spec 4.1) ──────────────────────────────────────────────────────
// A cart gets base, math, string, table and coroutine. It does NOT get io, os,
// package, require, debug, load/loadstring or dofile: a cart is untrusted
// content off an SD card, and none of those are in the console's model. Whole
// libraries are simply never opened rather than opened-then-nil'd, so there is
// no half-initialised state for a cart to reach around.

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "moy.h"

static const char *TAG = "moy_lua";

static lua_State *L;
static bool  s_ok;
static char  s_err[256];

bool        moy_lua_ok(void)    { return s_ok; }
const char *moy_lua_error(void) { return s_err; }

// ── Argument helpers ────────────────────────────────────────────────────────
//
// spec 4.2: every number a cart passes is a Lua double. Coordinates are floored
// rather than rounded — consistently, in one place — so that a cart computing
// x = 10.7 lands on the same pixel in every verb.
static inline int argi(lua_State *l, int i, int dflt)
{
    if (lua_isnoneornil(l, i)) return dflt;
    return (int)floor(luaL_checknumber(l, i));
}

static inline bool argb(lua_State *l, int i, bool dflt)
{
    if (lua_isnoneornil(l, i)) return dflt;
    return lua_toboolean(l, i) != 0;
}

// ── Drawing verbs ───────────────────────────────────────────────────────────

static int l_cls(lua_State *l)   { moy_cls(argi(l, 1, 0)); return 0; }
static int l_pix(lua_State *l)   { moy_pix(argi(l,1,0), argi(l,2,0), argi(l,3,0)); return 0; }

static int l_line(lua_State *l)
{
    moy_line(argi(l,1,0), argi(l,2,0), argi(l,3,0), argi(l,4,0), argi(l,5,0));
    return 0;
}

static int l_rect(lua_State *l)
{
    moy_rect(argi(l,1,0), argi(l,2,0), argi(l,3,0), argi(l,4,0), argi(l,5,0));
    return 0;
}

static int l_rectb(lua_State *l)
{
    moy_rectb(argi(l,1,0), argi(l,2,0), argi(l,3,0), argi(l,4,0), argi(l,5,0));
    return 0;
}

static int l_circ(lua_State *l)  { moy_circ(argi(l,1,0), argi(l,2,0), argi(l,3,0), argi(l,4,0)); return 0; }
static int l_circb(lua_State *l) { moy_circb(argi(l,1,0), argi(l,2,0), argi(l,3,0), argi(l,4,0)); return 0; }

static int l_print(lua_State *l)
{
    // Accepts any type, like Lua's own print — a cart printing a number should
    // not have to tostring() it first.
    const char *s = luaL_tolstring(l, 1, NULL);
    moy_print(s, argi(l,2,0), argi(l,3,0), argi(l,4,7));
    lua_pop(l, 1);
    return 0;
}

static int l_clip(lua_State *l)
{
    if (lua_isnoneornil(l, 1)) {        // clip() with no args resets
        g_moy.clip_x = 0; g_moy.clip_y = 0;
        g_moy.clip_w = MOY_SCREEN_W; g_moy.clip_h = MOY_SCREEN_H;
        return 0;
    }
    g_moy.clip_x = argi(l,1,0);
    g_moy.clip_y = argi(l,2,0);
    g_moy.clip_w = argi(l,3,MOY_SCREEN_W);
    g_moy.clip_h = argi(l,4,MOY_SCREEN_H);
    return 0;
}

static int l_camera(lua_State *l)
{
    g_moy.cam_x = argi(l,1,0);
    g_moy.cam_y = argi(l,2,0);
    return 0;
}

static int l_pal(lua_State *l)
{
    if (lua_isnoneornil(l, 1)) {        // pal() with no args restores identity
        for (int i = 0; i < MOY_PAL_N; i++) g_moy.pal_map[i] = (uint8_t)i;
        return 0;
    }
    int c0 = argi(l,1,0) & (MOY_PAL_N-1);
    int c1 = argi(l,2,0) & (MOY_PAL_N-1);
    g_moy.pal_map[c0] = (uint8_t)c1;
    return 0;
}

static int l_palt(lua_State *l)
{
    if (lua_isnoneornil(l, 1)) {        // palt() resets to "only 0 transparent"
        for (int i = 0; i < MOY_PAL_N; i++) g_moy.transparent[i] = false;
        g_moy.transparent[0] = true;
        return 0;
    }
    g_moy.transparent[argi(l,1,0) & (MOY_PAL_N-1)] = argb(l, 2, true);
    return 0;
}

// ── Sprites and map ─────────────────────────────────────────────────────────

static int l_spr(lua_State *l)
{
    // colorkey defaults to -1 = "use palt state", which is not the same as 0.
    moy_spr(argi(l,1,0), argi(l,2,0), argi(l,3,0),
            argi(l,4,-1), argi(l,5,1), argi(l,6,0));
    return 0;
}

static int l_sspr(lua_State *l)
{
    int sx = argi(l,1,0), sy = argi(l,2,0), sw = argi(l,3,8), sh = argi(l,4,8);
    int dx = argi(l,5,0), dy = argi(l,6,0);
    // dw/dh default to the source size — an unscaled blit.
    moy_sspr(sx, sy, sw, sh, dx, dy, argi(l,7,sw), argi(l,8,sh),
             argi(l,9,-1), argi(l,10,0));
    return 0;
}

static int l_map(lua_State *l)
{
    moy_map(argi(l,1,0), argi(l,2,0), argi(l,3,g_moy.map_w), argi(l,4,g_moy.map_h),
            argi(l,5,0), argi(l,6,0), argi(l,7,-1), argi(l,8,1));
    return 0;
}

static int l_mget(lua_State *l)
{
    int t = moy_mget(argi(l,1,0), argi(l,2,0));
    lua_pushinteger(l, t < 0 ? 0 : t);   // empty reads as 0, per PICO-8 habit
    return 1;
}

static int l_mset(lua_State *l) { moy_mset(argi(l,1,0), argi(l,2,0), argi(l,3,0)); return 0; }

// ── Input ───────────────────────────────────────────────────────────────────

static int l_btn(lua_State *l)
{
    int b = moy_btn_from_name(luaL_optstring(l, 1, ""));
    lua_pushboolean(l, b >= 0 && moy_btn(b, argi(l, 2, 0)));
    return 1;
}

static int l_btnp(lua_State *l)
{
    int b = moy_btn_from_name(luaL_optstring(l, 1, ""));
    lua_pushboolean(l, b >= 0 && moy_btnp(b, argi(l, 2, 0)));
    return 1;
}

static int l_players(lua_State *l) { lua_pushinteger(l, 1); return 1; }  // spec 7.3: always >= 1

static int l_touch(lua_State *l)
{
    int x = 0, y = 0; bool tapped = false, held = false;
    if (!moy_touch(&x, &y, &tapped, &held)) { lua_pushnil(l); return 1; }
    lua_pushinteger(l, x);
    lua_pushinteger(l, y);
    lua_pushboolean(l, tapped);
    lua_pushboolean(l, held);
    return 4;
}

static int l_key(lua_State *l)
{
    if (lua_isnoneornil(l, 1)) { lua_pushinteger(l, moy_key_last()); return 1; }
    lua_pushboolean(l, moy_key_held(argi(l, 1, 0)));
    return 1;
}

static int l_keyp(lua_State *l)
{
    if (lua_isnoneornil(l, 1)) { lua_pushinteger(l, moy_key_last()); return 1; }
    lua_pushboolean(l, moy_key_pressed(argi(l, 1, 0)));
    return 1;
}

static int l_textmode(lua_State *l) { moy_textmode(argb(l, 1, true)); return 0; }

// ── State and utility (spec 9) ──────────────────────────────────────────────

static int l_time(lua_State *l)
{
    lua_pushnumber(l, (double)esp_timer_get_time() / 1000000.0);
    return 1;
}

static int l_rnd(lua_State *l)
{
    double n = luaL_optnumber(l, 1, 1.0);
    // esp_random() is the hardware RNG. A cart wanting a reproducible sequence
    // implements its own PRNG — the spec has no seed verb, so there is nothing
    // here for a seed to attach to.
    double r = (double)esp_random() / 4294967296.0;
    lua_pushnumber(l, r * n);
    return 1;
}

static int l_flr(lua_State *l) { lua_pushnumber(l, floor(luaL_checknumber(l, 1))); return 1; }

static int l_cfg(lua_State *l)
{
    // config.json, read verbatim from the cart. Parsed per call rather than
    // cached: cfg() is called a handful of times in _init and never in a hot
    // loop, and keeping the parsed tree resident would cost PSRAM for the whole
    // run to save microseconds at startup.
    const char *k = luaL_checkstring(l, 1);
    if (!g_moy.config_json) { lua_pushvalue(l, 2); return 1; }

    cJSON *root = cJSON_Parse(g_moy.config_json);
    if (!root) { lua_pushvalue(l, 2); return 1; }

    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, k);
    if (cJSON_IsNumber(it))      lua_pushnumber(l, it->valuedouble);
    else if (cJSON_IsString(it)) lua_pushstring(l, it->valuestring);
    else if (cJSON_IsBool(it))   lua_pushboolean(l, cJSON_IsTrue(it));
    else                         lua_pushvalue(l, 2);   // absent -> the default

    cJSON_Delete(root);
    return 1;
}

static int l_pmem(lua_State *l)
{
    int i = argi(l, 1, 0);
    if (i < 0 || i >= 64) { lua_pushinteger(l, 0); return 1; }
    if (lua_isnoneornil(l, 2)) { lua_pushinteger(l, g_moy.pmem[i]); return 1; }
    g_moy.pmem[i] = (int32_t)argi(l, 2, 0);
    g_moy.pmem_dirty = true;
    lua_pushinteger(l, g_moy.pmem[i]);
    return 1;
}

static int l_quit(lua_State *l) { (void)l; g_moy.quit = true; return 0; }

static int l_view(lua_State *l)
{
    // EXTENSION: viewport. Reported honestly as the full screen — this host
    // does not letterbox, so a cart asking for a smaller view gets the truth
    // rather than a lie it would then letterbox against.
    (void)l;
    lua_pushinteger(l, MOY_SCREEN_W);
    lua_pushinteger(l, MOY_SCREEN_H);
    return 2;
}

// ── Audio — accepted, not produced ──────────────────────────────────────────
//
// T-Deck Plus has no configured audio output in PURR OS today, so these are
// no-ops. They are BOUND rather than left undefined on purpose: a cart calling
// sfx() must not error out, because spec 8 makes audio part of core and a cart
// is entitled to assume the verb exists. Silence is a degraded console; a nil
// global is a crashed one.
static int l_audio_noop(lua_State *l) { (void)l; return 0; }

// ── Registration ────────────────────────────────────────────────────────────

static const luaL_Reg moy_verbs[] = {
    { "cls", l_cls }, { "pix", l_pix }, { "line", l_line },
    { "rect", l_rect }, { "rectb", l_rectb },
    { "circ", l_circ }, { "circb", l_circb },
    { "print", l_print }, { "clip", l_clip }, { "camera", l_camera },
    { "pal", l_pal }, { "palt", l_palt },

    { "spr", l_spr }, { "sspr", l_sspr },
    { "map", l_map }, { "mget", l_mget }, { "mset", l_mset },

    { "btn", l_btn }, { "btnp", l_btnp }, { "players", l_players },
    { "touch", l_touch }, { "key", l_key }, { "keyp", l_keyp },
    { "textmode", l_textmode },

    { "time", l_time }, { "rnd", l_rnd }, { "flr", l_flr },
    { "cfg", l_cfg }, { "pmem", l_pmem }, { "quit", l_quit },
    { "view", l_view },

    { "sfx", l_audio_noop }, { "beep", l_audio_noop },
    { "music", l_audio_noop }, { "music_stop", l_audio_noop },
    { "sound_stop", l_audio_noop }, { "volume", l_audio_noop },

    { NULL, NULL }
};

// Lua's allocator, routed to PSRAM. A cart's whole heap — every table, string
// and closure — lands there rather than in the internal DRAM that speed demon
// exists to protect.
static void *moy_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (nsize == 0) { heap_caps_free(ptr); return NULL; }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}

static void fail(const char *what)
{
    const char *msg = lua_tostring(L, -1);
    snprintf(s_err, sizeof(s_err), "%s: %s", what, msg ? msg : "?");
    ESP_LOGE(TAG, "%s", s_err);
    s_ok = false;
}

bool moy_lua_start(void)
{
    s_ok = false;
    s_err[0] = '\0';

    L = lua_newstate(moy_alloc, NULL);
    if (!L) { snprintf(s_err, sizeof(s_err), "no memory for Lua"); return false; }

    // Sandbox: open only what spec 4.1 allows. io/os/package/debug are never
    // opened at all.
    luaL_requiref(L, LUA_GNAME,      luaopen_base,      1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME,luaopen_math,      1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string,    1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table,     1); lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME,  luaopen_coroutine, 1); lua_pop(L, 1);

    // Remove the base-library escapes. These come with luaopen_base and are the
    // only way out of the sandbox it leaves open.
    static const char *banned[] = { "dofile", "loadfile", "load", "require",
                                    "collectgarbage", "rawset", "rawget", NULL };
    for (int i = 0; banned[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, banned[i]);
    }

    // Console verbs as globals — the spec's API is flat: cls(), spr(), btn().
    lua_pushglobaltable(L);
    luaL_setfuncs(L, moy_verbs, 0);
    lua_pop(L, 1);

    lua_pushinteger(L, MOY_SCREEN_W); lua_setglobal(L, "SCREEN_W");
    lua_pushinteger(L, MOY_SCREEN_H); lua_setglobal(L, "SCREEN_H");

    // Load main.lua. Read through the cart reader so it obeys the same path
    // rules as every other asset.
    size_t len = 0;
    char *src = moy_cart_read("main.lua", &len);
    if (!src) { snprintf(s_err, sizeof(s_err), "main.lua unreadable"); return false; }

    int rc = luaL_loadbuffer(L, src, len, "@main.lua");
    heap_caps_free(src);
    if (rc != LUA_OK) { fail("load"); return false; }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) { fail("main.lua"); return false; }

    s_ok = true;
    return true;
}

void moy_lua_stop(void)
{
    if (L) { lua_close(L); L = NULL; }
    s_ok = false;
}

// Call a cart callback if it defined one. All three are optional (spec 4): a
// cart may be draw-only, or do everything in _update.
static void call_cb(const char *name, int nargs_pusher(lua_State *))
{
    if (!s_ok || !L) return;

    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }

    int nargs = nargs_pusher ? nargs_pusher(L) : 0;
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        // spec 4.3: a cart error stops the cart. It does NOT take the host
        // down — the message goes on screen and the player gets back to the OS.
        fail(name);
    }
}

static float s_dt;
static int push_dt(lua_State *l) { lua_pushnumber(l, s_dt); return 1; }

void moy_lua_call_init(void)          { call_cb("_init", NULL); }
void moy_lua_call_draw(void)          { call_cb("_draw", NULL); }
void moy_lua_call_update(float dt)    { s_dt = dt; call_cb("_update", push_dt); }
