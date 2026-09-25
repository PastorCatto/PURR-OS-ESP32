#pragma once
// moy.h — the moy console, as PURR OS implements it.
//
// moy core 0.1 (https://github.com/moybyte-org/moy-spec) is a portable game
// console spec: 320x240 palette-indexed, 64 colours, a 512-tile sheet, a
// tilemap, four audio channels and a sandboxed Lua 5.4 cart. Carts are folders
// and run unmodified on any conforming host.
//
// ── Why this port is small ──────────────────────────────────────────────────
//
// The console's shape happens to match what PURR OS already provides almost
// exactly, which is the reason it was chosen to test apps/common/purr_port.h:
//
//   spec                        PURR OS
//   320x240                     the T-Deck Plus panel, 1:1, no scaling
//   palette-indexed 8bpp        purr_port_open(&p, 8) — the fb8 path
//   64-entry RGB palette        purr_port_set_palette_rgb888()
//   present a frame             purr_port_present()
//   logical buttons, held       purr_port_key_next()'s synthesised key-up
//   Lua 5.4                     source/lib/lib_lua, already vendored
//
// So this file implements the console, not the platform. Every place it would
// otherwise have touched a catcall, it calls purr_port instead.
//
// ── Speed demon and Lua ─────────────────────────────────────────────────────
//
// Speed demon unloads the lua_runtime MODULE (it is PURR_MOD_SYSTEM), which
// would ordinarily rule out running a Lua cart with the panel to ourselves.
// This component sidesteps that by vendoring Lua 5.4 privately, exactly as
// lua_runtime itself does — see CMakeLists.txt. The interpreter here is ours,
// nothing unloads it, and a cart gets the whole machine.
//
// That is also the right design regardless: the spec requires a sandboxed
// interpreter with per-cart state and its own tick, which is not what a shared
// system-wide runtime is for.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../../common/purr_port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Console constants (spec 1) ──────────────────────────────────────────────

#define MOY_SCREEN_W    320
#define MOY_SCREEN_H    240

#define MOY_PAL_N       64          // spec 2 — indices 0-63, 0-15 are PICO-8's

#define MOY_SHEET_W     128         // spec 3.2 — 128 x 256 pixels
#define MOY_SHEET_H     256
#define MOY_TILES       512         // 16 per row, 8x8 each
#define MOY_TILE_MAX_MAP 254        // spec 3.3 — only 0-254 are placeable

#define MOY_MAP_MAX     128         // spec 3.3 — w and h at most 128

#define MOY_CART_DIR    "/sdcard/moy"

// Baked normative data (moy_data.c)
extern const uint8_t moy_palette_rgb888[MOY_PAL_N * 3];
extern const uint8_t moy_font8x8[96 * 8];

// ── Console state ───────────────────────────────────────────────────────────

typedef struct {
    // Sheet: one byte per pixel, values 0-15 (spec 2.3 — sprites are 16
    // colours; primitives get the full 64). Stored a byte per pixel rather than
    // packed nibbles: 32KB in PSRAM against a per-pixel shift and mask in the
    // innermost sprite loop, which is the wrong trade on this CPU.
    uint8_t *sheet;                 // MOY_SHEET_W * MOY_SHEET_H

    // Tilemap: one byte per cell, already decoded to tile_id (the file stores
    // tile_id + 1 so that 00 is empty; 0xFF here means empty).
    uint8_t *map;                   // map_w * map_h
    int      map_w, map_h;

    // Draw state (spec 6)
    int  clip_x, clip_y, clip_w, clip_h;
    int  cam_x, cam_y;
    uint8_t pal_map[MOY_PAL_N];     // pal() remap, identity by default
    bool    transparent[MOY_PAL_N]; // palt(), index 0 transparent by default

    // Cart
    char  title[64];
    char  dir[128];                 // absolute path of the .moy folder
    int   fps;                      // 30 or 60 (manifest, spec 5)
    char *config_json;              // raw config.json for cfg(), or NULL

    // Persistent state (spec 9 — pmem)
    int32_t pmem[64];
    bool    pmem_dirty;

    bool quit;                      // set by quit(), ends the tick loop
} moy_t;

// The console instance and the PURR OS translation layer it draws through.
// Globals rather than passed everywhere because the Lua bindings are C
// functions with a fixed signature and this is what they close over.
extern moy_t       g_moy;
extern purr_port_t g_port;

// ── moy_cart.c ──────────────────────────────────────────────────────────────

typedef enum {
    MOY_OK = 0,
    MOY_ERR_NO_DIR,        // /sdcard/moy missing
    MOY_ERR_NO_CART,       // no *.moy folder inside it
    MOY_ERR_MANIFEST,      // manifest.json missing or malformed
    MOY_ERR_FORMAT,        // manifest "format" is not one we implement
    MOY_ERR_MAIN,          // main lua script missing
    MOY_ERR_MEM,
    MOY_ERR_ASSET,         // sprites/map present but unparseable
} moy_err_t;

const char *moy_err_str(moy_err_t e);

#define MOY_MAX_CARTS 32

// Every *.moy folder found under MOY_CART_DIR, by folder name.
typedef struct {
    char name[MOY_MAX_CARTS][64];
    int  n;
} moy_cart_list_t;

moy_err_t moy_cart_scan(moy_cart_list_t *out);

// Load a cart by folder name (as returned by moy_cart_scan). Does NOT start Lua.
moy_err_t moy_cart_load(const char *folder);
void      moy_cart_free(void);

// Read the whole of a file inside the cart folder. Caller frees. NULL on error.
char *moy_cart_read(const char *filename, size_t *out_len);

// ── moy_menu.c ──────────────────────────────────────────────────────────────

// Choose a cart. Returns a folder name from `carts`, or NULL if the player
// backed out with the exit gesture. Skipped automatically when only one cart is
// installed.
const char *moy_menu_pick(const moy_cart_list_t *carts);

// ── moy_draw.c ──────────────────────────────────────────────────────────────
//
// Every one of these writes into g_port.fb8 and honours clip, camera, pal and
// palt per spec 6. Coordinates are camera-relative on entry.

void moy_draw_reset(void);          // clip = full screen, camera 0,0, identity pal

void moy_cls(int c);
void moy_pix(int x, int y, int c);
void moy_line(int x0, int y0, int x1, int y1, int c);
void moy_rect(int x, int y, int w, int h, int c);
void moy_rectb(int x, int y, int w, int h, int c);
void moy_circ(int cx, int cy, int r, int c);
void moy_circb(int cx, int cy, int r, int c);
void moy_print(const char *s, int x, int y, int c);
void moy_spr(int n, int x, int y, int colorkey, int scale, int flip);
void moy_sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
              int colorkey, int flip);
void moy_map(int mx, int my, int w, int h, int sx, int sy, int colorkey, int scale);
int  moy_mget(int x, int y);
void moy_mset(int x, int y, int tile);

// ── moy_lua.c ───────────────────────────────────────────────────────────────

bool moy_lua_start(void);           // create the VM, sandbox it, run main.lua
void moy_lua_stop(void);
void moy_lua_call_init(void);
void moy_lua_call_update(float dt);
void moy_lua_call_draw(void);
bool moy_lua_ok(void);              // false once a cart has errored out
const char *moy_lua_error(void);

// ── moy_input.c ─────────────────────────────────────────────────────────────

// Logical buttons (spec 7.3). left/right/up/down/a/b are required; run is not.
typedef enum {
    MOY_BTN_LEFT = 0, MOY_BTN_RIGHT, MOY_BTN_UP, MOY_BTN_DOWN,
    MOY_BTN_A, MOY_BTN_B, MOY_BTN_RUN,
    MOY_BTN_COUNT
} moy_btn_t;

void moy_input_reset(void);         // once per launch, BEFORE the first poll
void moy_input_poll(void);          // once per tick, before _update
bool moy_btn(int b, int player);
bool moy_btnp(int b, int player);
int  moy_btn_from_name(const char *name);   // -1 if unknown
int  moy_key_last(void);            // last typed ASCII, 0 for none
bool moy_key_held(int code);
bool moy_key_pressed(int code);
void moy_textmode(bool on);
bool moy_touch(int *x, int *y, bool *tapped, bool *held);

// The host's own exit gesture (spec 7.3 — "the host owns exit"). Hold BACKSPACE
// for a second, or tap it three times. `how` receives which form fired, which
// is also how we learn whether the bbq20 repeats held keys.
bool moy_exit_requested(const char **how);

#ifdef __cplusplus
}
#endif
