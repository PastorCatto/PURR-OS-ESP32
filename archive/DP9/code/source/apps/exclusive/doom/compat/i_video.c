/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2006 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *  ESP32 port Copyright 2018 Espressif Systems, Apache-2.0
 *  PURR OS port 2026
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * DESCRIPTION:
 *  DOOM graphics and input, on PURR OS catcalls.
 *
 *-----------------------------------------------------------------------------
 */

// Rewritten for PURR OS. What was here drove a private SPI LCD driver
// (spi_lcd.c) and a PSX gamepad (gamepad.c); both are gone. Output now goes
// through catcall_display and input comes from catcall_input, so DOOM draws to
// whatever panel the device declares rather than to a hardcoded one.
//
// ── Resolution ──────────────────────────────────────────────────────────────
// PrBoom is configured for 320x240 here (doomdef.h), not Doom's native 320x200
// — Espressif had already made that change. It happens to be exactly the
// T-Deck Plus panel, so the image is 1:1 with no letterboxing and no scaling.
// A device with a different panel gets the top-left 320x240 of it; scaling is
// left undone deliberately rather than guessed at.

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "m_argv.h"
#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "d_event.h"
#include "i_video.h"
#include "z_zone.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "lprintf.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "purr_kernel.h"
#include "catcall_display.h"
#include "catcall_input.h"
#include "speed_demon.h"

int use_fullscreen = 0;
int use_doublebuffer = 0;

// Joystick globals. There is no joystick and none of these are read, but
// m_misc.c's `defaults[]` table takes their ADDRESSES to register them as
// config keys, so they have to exist or the link fails — which is exactly what
// happened when gamepad.c (the PSX controller driver, deleted with the rest of
// the old input path) went away.
//
// usejoystick stays 0 so I_InitJoystick/I_PollJoystick are never reached.
int usejoystick = 0;
int joyleft, joyright, joyup, joydown;

void I_InitJoystick(void) { }
void I_PollJoystick(void) { }

// Provided by doom_app.c — resolves the keyboard by capability rather than by
// being the first registered input (which is the trackball on T-Deck Plus).
extern const catcall_input_t *doom_find_keyboard(void);

// ── Framebuffers ────────────────────────────────────────────────────────────

unsigned char   *screenbuf   = NULL;   // 8-bit paletted, what PrBoom renders into
static uint16_t *s_rgb       = NULL;   // RGB565, what the panel is given
static bool      s_fb_internal = false;

// Palette, native-endian RGB565.
//
// The original stored it byte-swapped — `lcdpal[i]=(v>>8)+(v<<8)` — because its
// SPI driver wrote raw bytes straight out. PURR OS's st7789 driver does the
// byte swap itself on the way to DMA, so keeping the swap here would apply it
// twice and produce wrong colours in a way that looks like a broken palette
// rather than an endianness bug. Stored native; the driver swaps.
uint16_t lcdpal[256];

// ── Graphics API ────────────────────────────────────────────────────────────

void I_ShutdownGraphics(void)
{
    if (s_rgb) { heap_caps_free(s_rgb); s_rgb = NULL; }
    if (screenbuf) { heap_caps_free(screenbuf); screenbuf = NULL; }
}

void I_UpdateNoBlit(void) { }
void I_StartFrame(void)   { }

int I_StartDisplay(void)
{
    // The synchronous push in I_FinishUpdate has already completed by the time
    // it returns, so there is nothing to wait for here. The original called
    // spi_lcd_wait_finish() because its driver queued transfers asynchronously.
    return true;
}

void I_EndDisplay(void) { }

//
// I_FinishUpdate — palette-expand the frame and hand it to the panel.
//
void I_FinishUpdate(void)
{
    if (!s_rgb || !screenbuf) return;

    const catcall_display_t *d = purr_kernel_display();
    if (!d || !d->push_pixels) return;

    // 8bpp -> RGB565. Unrolled by four: this runs 76,800 times a frame and is
    // the single hottest loop outside the renderer, so the loop overhead is
    // worth removing. The source is byte-addressed and the destination is
    // 16-bit, so there is no alignment trick available beyond this.
    const uint8_t *src = screenbuf;
    uint16_t      *dst = s_rgb;
    int n = SCREENWIDTH * SCREENHEIGHT;
    while (n >= 4) {
        dst[0] = lcdpal[src[0]];
        dst[1] = lcdpal[src[1]];
        dst[2] = lcdpal[src[2]];
        dst[3] = lcdpal[src[3]];
        dst += 4; src += 4; n -= 4;
    }
    while (n--) *dst++ = lcdpal[*src++];

    d->push_pixels(0, 0, SCREENWIDTH, SCREENHEIGHT, s_rgb);
}

void I_SetPalette(int pal)
{
    int pplump = W_GetNumForName("PLAYPAL");
    const byte *palette = W_CacheLumpNum(pplump);
    palette += pal * (3 * 256);

    // i < 256, not i < 255 as the original had it. That left entry 255
    // uninitialised — it is a real colour in Doom's palette and showed up as
    // whatever happened to be in that slot.
    for (int i = 0; i < 256; i++) {
        lcdpal[i] = (uint16_t)(((palette[0] >> 3) << 11) |
                               ((palette[1] >> 2) << 5)  |
                                (palette[2] >> 3));
        palette += 3;
    }
    W_UnlockLumpNum(pplump);
}

void I_PreInitGraphics(void)
{
    // The 8-bit buffer PrBoom renders into: 320*240 = 76,800 bytes.
    //
    // Internal DRAM first, because the renderer's access to it is scattered
    // (column-major spans, sprite blits) and PSRAM's cache-miss penalty is paid
    // per access rather than per frame. Espressif's port forced it internal and
    // asserted if that failed, which on this device would be a hard failure on
    // a board that merely has less internal DRAM free.
    //
    // So: try internal, fall back to PSRAM, and say which was used. A slower
    // DOOM is a much better outcome than an assert, and the log line makes the
    // difference visible when frame times are being looked at.
    screenbuf = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_fb_internal = (screenbuf != NULL);
    if (!screenbuf) {
        screenbuf = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT, MALLOC_CAP_SPIRAM);
    }
    if (!screenbuf) {
        I_Error("I_PreInitGraphics: no memory for the %dx%d framebuffer",
                SCREENWIDTH, SCREENHEIGHT);
        return;
    }
    memset(screenbuf, 0, SCREENWIDTH * SCREENHEIGHT);

    // The RGB565 copy handed to the panel: 153,600 bytes, PSRAM. This one is
    // written linearly and read once by DMA, which is the access pattern PSRAM
    // is fine at, and it is far too large to spend internal DRAM on.
    s_rgb = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!s_rgb) {
        I_Error("I_PreInitGraphics: no PSRAM for the RGB565 buffer");
        return;
    }
    memset(s_rgb, 0, SCREENWIDTH * SCREENHEIGHT * 2);

    lprintf(LO_INFO, "I_PreInitGraphics: 8bpp fb in %s, RGB565 in PSRAM\n",
            s_fb_internal ? "internal DRAM" : "PSRAM (slower)");
}

// CPhipps - I_SetRes - sets the screen resolution
void I_SetRes(void)
{
    for (int i = 0; i < 3; i++) {
        screens[i].width       = SCREENWIDTH;
        screens[i].height      = SCREENHEIGHT;
        screens[i].byte_pitch  = SCREENPITCH;
        screens[i].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
        screens[i].int_pitch   = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);
    }

    // statusbar
    screens[4].width       = SCREENWIDTH;
    screens[4].height      = (ST_SCALED_HEIGHT + 1);
    screens[4].byte_pitch  = SCREENPITCH;
    screens[4].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
    screens[4].int_pitch   = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);

    // not_on_heap: screens[0] is ours, allocated in I_PreInitGraphics, and
    // V_FreeScreens() must not try to Z_Free() it.
    screens[0].not_on_heap = true;
    screens[0].data        = screenbuf;

    lprintf(LO_INFO, "I_SetRes: Using resolution %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
}

void I_InitGraphics(void)
{
    static int firsttime = 1;
    if (!firsttime) return;
    firsttime = 0;

    atexit(I_ShutdownGraphics);
    lprintf(LO_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
    I_UpdateVideoMode();
}

void I_UpdateVideoMode(void)
{
    lprintf(LO_INFO, "I_UpdateVideoMode: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

    V_InitMode(VID_MODE8);
    V_DestroyUnusedTrueColorPalettes();
    V_FreeScreens();

    I_SetRes();

    V_AllocScreens();
    R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
}

// ── Input ───────────────────────────────────────────────────────────────────
//
// ── The problem this solves ─────────────────────────────────────────────────
// DOOM needs key RELEASES: holding forward walks, letting go stops. The bbq20
// driver never sends one — it only ever emits INPUT_EVENT_KEY_DOWN (see
// bbq20.c, which polls the keyboard's current key over I2C every 20ms and
// posts an event whenever the byte is non-zero). Feed that to DOOM directly
// and the player walks forward forever after one tap.
//
// So releases are synthesised. A key is considered held while events for it
// keep arriving, and released once one has not arrived for KEY_HOLD_MS.
//
// This degrades sensibly whichever way the keyboard firmware actually behaves,
// which matters because it could not be confirmed without a person at the
// device:
//
//   - If a held key repeats (each 20ms poll re-reports it, which is what the
//     driver's structure implies), the timer keeps being refreshed and
//     movement is smooth and continuous.
//   - If instead each press reports exactly once, every tap yields KEY_HOLD_MS
//     of movement — a step per press. Playable, if stilted. NOT broken.
//
// KEY_HOLD_MS is 120: six 20ms poll intervals, enough to ride out a couple of
// missed I2C reads without the player stuttering, short enough that a released
// key stops promptly.
#define KEY_HOLD_MS 120

typedef struct {
    uint8_t  doomkey;      // 0 = slot free
    int64_t  last_seen_us;
} held_key_t;

#define MAX_HELD 8
static held_key_t s_held[MAX_HELD];

// ASCII from the keyboard -> DOOM keycode.
//
// WASD, keyboard only — the trackball is deliberately not used for turning
// here. Movement:  W/S forward+back, A/D turn, Q/E strafe.
// Action:  F or Enter fire, Space use/open, Left-shift run.
// Menus:   Enter selects, Backspace is Escape (the bbq20 has no Esc key, and
//          without a route to the menu there is no way to save, load or quit).
static int map_key(uint8_t c)
{
    switch (c) {
    case 'w': case 'W': return KEYD_UPARROW;
    case 's': case 'S': return KEYD_DOWNARROW;
    case 'a': case 'A': return KEYD_LEFTARROW;
    case 'd': case 'D': return KEYD_RIGHTARROW;

    case 'q': case 'Q': return ',';            // strafe left  (DOOM default)
    case 'e': case 'E': return '.';            // strafe right

    case 'f': case 'F': return KEYD_RCTRL;     // fire
    case ' ':           return KEYD_SPACEBAR;  // use / open

    case '\r': case '\n': return KEYD_ENTER;
    case '\b': case 0x7F: return KEYD_ESCAPE;

    // Weapon select. Straight through: DOOM reads '1'..'7' as themselves.
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7':
        return c;

    case '\t':          return KEYD_TAB;       // automap
    default:            return 0;
    }
}

static void post(int type, int key)
{
    event_t ev;
    ev.type  = type;
    ev.data1 = key;
    ev.data2 = 0;
    ev.data3 = 0;
    D_PostEvent(&ev);
}

void I_StartTic(void)
{
    // Speed demon's liveness beacon. This is the right place for it: I_StartTic
    // runs once per tic (35Hz) for as long as the engine is alive, so it stops
    // exactly when DOOM stops. The requirement is at least every 5 seconds and
    // nothing else in this app is running often enough to do it.
    purr_speed_demon_heartbeat();

    const catcall_input_t *kbd = doom_find_keyboard();
    int64_t now = esp_timer_get_time();

    if (kbd && kbd->poll_event) {
        input_event_t ev;
        while (kbd->poll_event(&ev)) {
            if (ev.type != INPUT_EVENT_KEY_DOWN) continue;

            int dk = map_key((uint8_t)ev.keycode);
            if (!dk) continue;

            // Already held? Refresh it and post nothing — DOOM does its own
            // key repeat for menus, and re-posting keydown every 20ms would
            // make menu navigation uncontrollable.
            int slot = -1;
            for (int i = 0; i < MAX_HELD; i++) {
                if (s_held[i].doomkey == (uint8_t)dk) { slot = i; break; }
            }
            if (slot >= 0) {
                s_held[slot].last_seen_us = now;
                continue;
            }

            for (int i = 0; i < MAX_HELD; i++) {
                if (s_held[i].doomkey == 0) {
                    s_held[i].doomkey      = (uint8_t)dk;
                    s_held[i].last_seen_us = now;
                    post(ev_keydown, dk);
                    break;
                }
            }
        }
    }

    // Expire held keys into releases.
    for (int i = 0; i < MAX_HELD; i++) {
        if (!s_held[i].doomkey) continue;
        if (now - s_held[i].last_seen_us > (int64_t)KEY_HOLD_MS * 1000) {
            post(ev_keyup, s_held[i].doomkey);
            s_held[i].doomkey = 0;
        }
    }
}
