// SDL stub for ESP32 — replaces desktop SDL so 8086tiny.c compiles on bare metal.
// All display/audio/event calls are no-ops; keyboard input comes via drv_8086's
// purr_dos_kb_push() instead of SDL events.
#pragma once
#include <stdint.h>

#define SDL_INIT_AUDIO  0
#define SDL_INIT_VIDEO  0
#define AUDIO_U8        0
#define KMOD_ALT        0
#define KMOD_SHIFT      0
#define KMOD_CTRL       0
#define SDL_KEYDOWN     0
#define SDL_KEYUP       1

typedef struct { int type; struct { struct { int sym; int unicode; int mod; } keysym; } key; } SDL_Event;
typedef void (*SDL_AudioCallback)(void *userdata, uint8_t *stream, int len);
typedef struct { int freq; int format; int channels; int silence; int samples; SDL_AudioCallback callback; void *userdata; } SDL_AudioSpec;
typedef struct { void *pixels; int w; int h; int pitch; } SDL_Surface;

static inline int  SDL_Init(int flags)                          { (void)flags; return 0; }
static inline int  SDL_OpenAudio(SDL_AudioSpec *s, void *o)     { (void)s; (void)o; return 0; }
static inline void SDL_PauseAudio(int p)                        { (void)p; }
static inline SDL_Surface *SDL_SetVideoMode(int w, int h, int b, int f) { (void)w;(void)h;(void)b;(void)f; return 0; }
static inline void SDL_EnableUNICODE(int e)                     { (void)e; }
static inline void SDL_EnableKeyRepeat(int d, int i)            { (void)d; (void)i; }
static inline int  SDL_PollEvent(SDL_Event *e)                  { (void)e; return 0; }
static inline void SDL_Flip(SDL_Surface *s)                     { (void)s; }
static inline void SDL_PumpEvents(void)                         {}
static inline void SDL_Quit(void)                               {}
static inline void SDL_QuitSubSystem(int f)                     { (void)f; }
