#pragma once
// purr_uiconf_core.h — loads and interprets a compiled `.puib` UI-config
// screen (see catstrap/uiconf.py for the compiler and the full format
// design in the approved plan). Backend-agnostic: zero display/touch/LVGL
// code here, same contract source/apps/system/login_ui/login_core.h
// already keeps — a renderer (purr_uiconf_render_fb.c today, a future
// purr_uiconf_render_winapi.c later) owns all drawing.
//
// First-pass scope, deliberately: flat screens (widgets directly under a
// screen) render correctly; `section` nesting (needed for settings'
// grouped-and-conditional tabs) is parsed and stored but not yet exposed
// through purr_uiconf_visible_children() below — see that function's own
// comment. Reintroduced once the first real vertical slice (a watchface +
// app-list screen, matching epaper_ui.c's own three screens) is proven on
// real hardware, per this whole effort's own "gradually re-introducing
// features" direction.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ── Binary format — MUST stay byte-for-byte in sync with catstrap/
// uiconf.py's own Compiler.serialize()/NODE_STRUCT/NODE_KIND_IDS. See
// that file's own top comment for the authoritative description; this
// header just implements the reader side of the identical layout.
#pragma pack(push, 1)
typedef struct {
    char     magic[4];       // "PUIB"
    uint8_t  version;
    uint16_t node_count;
    uint32_t pool_size;
} pui_header_t;

typedef struct {
    uint8_t  kind;
    int16_t  parent;
    int16_t  first_child;
    int16_t  next_sibling;
    uint32_t attr_off;        // offset into the pool of this node's attr run
    uint16_t attr_count;
} pui_node_t;
#pragma pack(pop)

#define PUI_MAGIC "PUIB"
#define PUI_FORMAT_VERSION 1

typedef enum {
    PUI_KIND_SCREEN       = 0,
    PUI_KIND_SECTION      = 1,
    PUI_KIND_WIDGET_TEXT  = 2,
    PUI_KIND_WIDGET_LIST  = 3,
    PUI_KIND_WIDGET_BUTTON= 4,
    PUI_KIND_ITEM         = 5,
    PUI_KIND_ON_HANDLER   = 6,
} pui_kind_t;

// Attr value kinds — matches uiconf.py's _encode_attrs() exactly.
typedef enum {
    PUI_ATTR_STRING = 0,
    PUI_ATTR_NUMBER = 1,
    PUI_ATTR_BOOL   = 2,
    PUI_ATTR_CONDLIST = 3,
} pui_attr_kind_t;

// A loaded screen — owns `data` (the whole file, malloc'd) until
// purr_uiconf_free_screen(). `nodes`/`pool` are plain pointers into it.
typedef struct {
    uint8_t   *data;
    size_t     size;
    const pui_header_t *header;
    const pui_node_t   *nodes;
    const uint8_t       *pool;
} purr_uiconf_screen_t;

// Loads /flash/ui/<app_name>/<screen_name>.puib (falls back to
// /sdcard/ui/<app_name>/<screen_name>.puib if not on flash — same
// SD-preferred-override, flash-fallback convention claw_loader.c's own
// personal_root()/system_root() already established). Returns false on
// any I/O error or a magic/version mismatch.
bool purr_uiconf_load_screen(const char *app_name, const char *screen_name,
                              purr_uiconf_screen_t *out);
void purr_uiconf_free_screen(purr_uiconf_screen_t *s);

// ── Node tree access ─────────────────────────────────────────────────
// Node 0 is always the SCREEN node itself (compile_screen() emits it
// first, with parent=-1 — see uiconf.py's own Compiler.add_node()).
#define PUI_ROOT 0
#define PUI_NONE (-1)

pui_kind_t purr_uiconf_kind(const purr_uiconf_screen_t *s, int node);
int purr_uiconf_first_child(const purr_uiconf_screen_t *s, int node);
int purr_uiconf_next_sibling(const purr_uiconf_screen_t *s, int node);

// Plain string/number/bool attr lookup by key name. Returns false if the
// node has no such attr (caller decides the default). `out_str` is
// NUL-terminated on success.
bool purr_uiconf_attr_str(const purr_uiconf_screen_t *s, int node, const char *key,
                           char *out_str, size_t out_sz);
bool purr_uiconf_attr_num(const purr_uiconf_screen_t *s, int node, const char *key, float *out);
bool purr_uiconf_attr_bool(const purr_uiconf_screen_t *s, int node, const char *key, bool *out);

// True if `node` has no visible_if attr, or every condition in it (after
// negation) currently evaluates true via the fixed condition table below.
// Same for enabled_if. A node with both gets independently checked by
// whichever of the two the caller asks about.
bool purr_uiconf_visible(const purr_uiconf_screen_t *s, int node);
bool purr_uiconf_enabled(const purr_uiconf_screen_t *s, int node);

// Resolves a scalar `bind` name (e.g. "battery_percent") to its current
// live value, formatted through `format` if given (a small fixed printf-
// style subset: %d, %s, %d%%, "User: %s" — NOT a general printf, see
// purr_uiconf_core.c's own comment on why). Returns false for an unknown
// binding name (should never happen post-validation, but the device
// still degrades to an empty string rather than garbage).
bool purr_uiconf_resolve_bind(const char *bind_name, const char *format,
                               char *out, size_t out_sz);

// ── Navigation ───────────────────────────────────────────────────────
typedef enum { PUI_NAV_NEXT, PUI_NAV_SELECT, PUI_NAV_BACK } pui_nav_event_t;

// Runtime state for ONE currently-open screen: which item (by child
// index within the currently-relevant widget) is highlighted. Kept here,
// not in the renderer, so a future second renderer (winapi) shares the
// identical selection/highlight behavior for free — same reasoning
// login_core.c already established for keeping this out of the render
// backends entirely.
typedef struct {
    purr_uiconf_screen_t screen;
    // Index into the screen's ONE unified selectable sequence: every
    // top-level `list` widget's own rows plus every top-level `button`
    // widget (one entry each), in document order — see
    // purr_uiconf_sources.h's own purr_uiconf_selectable_count()/_at()
    // for the exact rule and why a plain `text` widget or an empty list
    // contribute nothing to it. A `list`-only screen (no buttons) behaves
    // exactly as the original single-list model did.
    int selected_index;
} purr_uiconf_state_t;

// Loads `screen_name` fresh (any previous screen in `state` is freed
// first) and resets selection to 0.
bool purr_uiconf_open(purr_uiconf_state_t *state, const char *app_name, const char *screen_name);
void purr_uiconf_close(purr_uiconf_state_t *state);

// Finds the "on <event_name>" ON_HANDLER child of `node` (a screen or a
// widget), if any, and reads its `action`/`target` attrs. Action
// dispatch itself is deliberately NOT this core's job (no callback
// registry here) — the caller's own small glue reads these and calls the
// real function itself (open_screen -> purr_uiconf_open(), launch_app ->
// app_manager_launch_idx(), run_command -> a per-app fixed table). This
// mirrors the plan's own framing: a config-driven UI shell, not a
// scripting engine.
bool purr_uiconf_find_handler(const purr_uiconf_screen_t *s, int node, const char *event_name,
                               char *out_action, size_t action_sz,
                               char *out_target, size_t target_sz);
