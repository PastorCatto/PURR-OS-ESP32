# Pounce — raw-framebuffer, keyboard+trackball-first, Meshtastic-first UI Backend + `.kitten` extension

> **Status as of v1.0.0-dp8 — both parts are BUILT. The "not yet implemented"
> line below was wrong for most of the DP cycle.**
>
> - **Part A (Pounce)** is implemented: `source/modules/pounce/` is ~1,575 lines
>   across `pounce_win.c`, `pounce_render.c`, `pounce_focus.c`, `pounce_status.c`,
>   `pounce_launcher.c` and `pounce_module.c`, with its own 6×8 font. It is wired
>   into `Kconfig.projbuild` as `PURR_UI_BACKEND_POUNCE` and has a device target,
>   `tdeck_plus_pounce`.
> - **Part B (`.kitten`)** is implemented: `APP_TIER_KITTEN` exists in
>   `app_manager.c`, the first `.kitten` found on SD autoruns at boot, and it is
>   gated behind Developer Mode exactly as `.hiss` is — with a stricter rationale,
>   since an autorunning script needs the gate more than a launched one does.
>
> **Two things remain true and are the reason this file is not deleted:**
>
> 1. **Pounce has never been run on hardware.** Not once. It compiles and links,
>    which proves nothing about what it draws.
> 2. **Pounce omits four optional `catcall_ui_t` members** — `label_set_big`,
>    `tile_grid_create`, `tile_grid_set_items` and `list_set_items_icon` (grepped:
>    zero occurrences in the module). It leaves them NULL and callers fall back,
>    which is the contract working as designed — but it means the MSN tile-grid
>    Home screen and Settings' tile-grid category picker degrade to plain lists
>    under Pounce. That is a deliberate, documented gap, not a bug.
>
> Also note the contract version drift: the text below says `CATCALL_UI_VERSION` 4.
> It is **7** as of dp8. Pounce implements the members it needs and NULLs the rest,
> so the bump did not break it — but the plan's version claim is stale.

**Working tracking/design doc. Delete once Pounce has been verified on real
hardware and its optional-member gaps are either filled or formally scoped out
(see `meshplan.md` for the convention this follows).**

This plan has two independent parts: **(A)** a brand-new UI backend ("Pounce")
and **(B)** a new `.kitten` app-file extension, which is UI-agnostic and
unrelated to Pounce specifically (works on any backend). They touch entirely
different files.

---

## Part A — Pounce UI backend

### Context

Every existing UI backend (Cupcake, KittenUI, Cardstack = LVGL; MiniWin = a large
vendored C library) is touch-primary, with keyboard/trackball support bolted on
afterward and incomplete: MiniWin's on-screen-keyboard hooks are literal
empty-body stubs, KittenUI has a code comment admitting it has no
group/encoder-navigation infrastructure, and **no backend anywhere in this
codebase has real widget-to-widget keyboard/trackball focus navigation today**.
Pounce is a new backend built the opposite way: no LVGL, no MiniWin, direct
`fill_rect`/`push_pixels` calls only, designed keyboard+trackball first, with
performance as the explicit top priority, and Meshtastic status/quick-access
built into its chrome rather than left as just another app.

It implements the same `catcall_ui_t`/`purr_win_t` contract MiniWin and KittenUI
implement (confirmed: `CATCALL_UI_VERSION` 4 — window/label/button/textarea/
list/layout/keyboard hooks), so all 8 existing system apps (settings, terminal,
fileman, calculator, meshchat, drivermgr, hwtest, services) run **unmodified**.

### A1. Module layout and wiring

New module `source/modules/pounce/` (files below), registered like any other UI
`.purr` module:
- `source/modules/pounce/module.pcat` — mirrors `kittenui/module.pcat`
- `source/modules/pounce/CMakeLists.txt` — `REQUIRES esp_common driver freertos
  nvs_flash` only — **no `lvgl__lvgl`**, that's the whole point
- New Kconfig leaf in `CoreOS/main/Kconfig.projbuild`'s existing
  `choice PURR_UI_BACKEND` block: `PURR_UI_BACKEND_POUNCE`, help text describing
  it as direct-framebuffer, keyboard+trackball-first. **Do not change the
  choice's `default`** (stays `PURR_UI_BACKEND_KITTENUI`).
- `purrstrap/purrstrap.py`: add `"pounce": "POUNCE"` to `UI_BACKEND_MAP`
  (`purrstrap.py:599`) so `device.pcat` can select it by name later.

**Landing target: opt-in only, via a new full `source/devices/tdeck_plus_pounce/
device.pcat`** (same precedent as `tdeck_plus_arduino`/`tdeck_plus_test` already
being their own separate device slugs, not overrides layered on `tdeck_plus`) —
copied from `tdeck_plus/device.pcat` with `[modules] ui = "pounce"`. Do **not**
touch `tdeck_plus/device.pcat` itself — its MiniWin/WinCE setup is this
session's active, still-hardware-unverified work, and regenerating its
sdkconfig risks disturbing that in-flight verification.

**Bluetooth/Meshtastic gates now come from `device.pcat`, not hand-edited
`.overrides`.** `[modules] bt = "bt_mgr"` / `mesh = "meshtastic"` fields already
exist in `device.pcat`'s schema but never actually drove
`CONFIG_BT_NIMBLE_ENABLED`/`CONFIG_PURR_FEATURE_MESHTASTIC` — those were only
ever hand-set in `sdkconfig_tdeck_plus.overrides` (now commented out from
last night's BT/Meshtastic-gating pass). Extending
`purrstrap.py`'s `_sdkconfig_lines()` (`purrstrap.py:659-666`) with the same
pattern already used for `ui` → `CONFIG_PURR_UI_BACKEND_*`:
```python
if cfg.get("modules.bt", ""):
    lines += ["", "# Bluetooth (NimBLE)", "CONFIG_BT_ENABLED=y", "CONFIG_BT_NIMBLE_ENABLED=y"]
if cfg.get("modules.mesh", ""):
    lines += ["", "# Meshtastic", "CONFIG_PURR_FEATURE_MESHTASTIC=y"]
```
`tdeck_plus_pounce/device.pcat` sets `mesh = "meshtastic"` under `[modules]`
(so the status strip in A6 shows real data) but leaves `bt` unset (Pounce's
design has no BLE phone-companion UI, so no reason to pay for it). This is
generically useful beyond Pounce — any device can now toggle these features
via `device.pcat` alone and regenerate, matching how `ui` already works.

### A2. Widget/window data model

Two flat, fixed-size pools (mirrors KittenUI's linear-scan pool pattern,
sized against real usage — `settings.c` alone creates ~10 labels + ~20 buttons +
3 lists across its main+WiFi+BT windows):

```c
#define PW_MAX_WINS 16
#define PW_MAX_WIDS 256

typedef enum { PW_LABEL, PW_BUTTON, PW_TEXTAREA, PW_LIST, PW_LAYOUT } pw_kind_t;

typedef struct {
    bool       alive;
    pw_kind_t  kind;
    purr_win_t win;
    int16_t    x, y, w, h;              // computed by the layout pass
    bool       focusable, focused;
    int16_t    tab_prev, tab_next;      // creation-order linear tab order
    int16_t    group_id, index_in_group; // enclosing layout_begin() group — see A4
    union {
        struct { char *text; purr_align_t align; } label;
        struct { char *text; bool enabled; purr_win_cb_t cb; void *user; } button;
        struct { char *buf; size_t buf_cap, len; bool editing;
                 purr_win_cb_t cb; void *user; } textarea;
        struct { char **items; int count, selected, top_visible;
                 purr_win_cb_t cb; void *user; } list;
        struct { purr_layout_t dir; uint8_t pad; bool grow;
                 int16_t first_child, child_count; } layout;
    };
} pw_widget_t;

typedef struct {
    bool alive, visible;
    char title[32];
    purr_win_cb_t on_close_cb; void *on_close_user;
    int16_t tab_head, tab_tail, focus_wid, active_layout;
} pw_window_t;
```

Every `char*` text field (label/button/textarea/list-item strings) is
individually `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`-backed, sized to
`strlen()+1` at set-time. Deliberate, not incidental: `settings.c`'s About
label is a 512-byte string; PSRAM sits at ~8.3MB free while internal DRAM
flatlines to ~1-2KB shortly after boot on tdeck_plus (documented in
`sdkconfig_tdeck_plus.overrides`). The **pools** stay fixed-size and
non-relocating; only payload strings are individually allocated — same as
MiniWin/KittenUI already do implicitly via their own libraries.

**Row and col are both real**, via one shared axis-generic partition function
(`pw_layout_compute()` swaps which axis it equal-partitions based on `dir`) —
not a stub for `col`. Confirmed 21 real `purr_win_row()` calls across system
apps and 0 `purr_win_col()` calls, but a 3rd-party `.meow`/`.hiss`/`.kitten` app
could call `col`, and making it real costs nothing extra once the partition
logic is axis-generic.

### A3. Rendering — synchronous immediate redraw at the mutation call site

No dirty-flag/tick-scan architecture. Every `catcall_ui_t` mutator
(`label_set`, `textarea_set`, `list_set_selected`, etc.) already runs
**synchronously under `purr_kernel_ui_lock()`** (guaranteed by `purr_win.h`'s
`_UI_CALL`/`_UI_VOID` macros), in whatever app task called it — so each
function computes its widget's cached `(x,y,w,h)` and calls
`fill_rect`/`push_pixels` **immediately, right there**. This is strictly
faster than any scan (zero untouched widgets ever get redrawn) and simpler
than dirty-bit bookkeeping. BlackPurr already proves this pattern works on
this exact hardware (`move_sel()`'s direct two-cell redraw on selection change).

**Window model: full-screen modal stack, not overlapping/floating windows.**
KittenUI's own windows are already forced full-screen
(`lv_obj_set_size(win, LV_PCT(100), LV_PCT(100))`) — this isn't a feature cut,
it matches how every real app already behaves (`settings.c` opens WiFi/BT as
stacked full-screen dialogs, never side-by-side). A small MRU stack of window
handles tracks what's visible; `win_show()` pushes + full-repaints,
`win_hide()`/`win_destroy()` pops + full-repaints whatever's now on top. This
sidesteps clipping/z-order pixel composition — the single most expensive part
of a real windowing system — entirely, which is the single biggest lever for
the "speed first" goal, at zero real cost against observed usage. Window
layout always reserves the status strip's height at the top (see A5) so app
content and chrome never overlap.

### A4. Keyboard + trackball focus navigation (the genuinely new part)

**Ordering: creation order, grouped by enclosing row/col.** Every real app's
layout is `layout_begin(ROW) → N widgets → layout_end()`, repeated
top-to-bottom (verified in `settings.c`, `calculator.c`'s 4-wide keypad). Group
table built at `win_show()`/layout time: each contiguous run of focusable
widgets sharing an enclosing `layout_begin()` is one group (singleton group
for an ungrouped widget), ordered by first appearance.
- **Left/Right** (trackball `dx`) move within the current group; fall through
  to the adjacent group's last/first widget at an edge (no dead stops).
- **Up/Down** (trackball `dy`) move to the previous/next group, same
  `index_in_group` clamped to that group's size.
- Gives calculator's keypad real 2D grid nav and settings' button-rows
  sensible vertical traversal, derived entirely from data every app already
  provides — no per-app changes needed.

**Trackball deltas → discrete steps, backend-local only — no driver change.**
The trackball driver already quantizes+accelerates+throttles
(`trackball.c`). Pounce accumulates `dx`/`dy` and fires one nav step per `±3`
crossed, matching BlackPurr's own proven threshold constant.

**Keyboard equivalents:** `Tab` (`0x09`) = next widget in flat tab order
(BBQ20 emits raw ASCII only, no arrow-key codes, no modifier bits — confirmed
in `bbq20.c`). `Enter` (`0x0D`) = Activate, and trackball click (keycode
`0x0028`) maps to the same `activate_focused()` — both treated uniformly.

**Activate semantics:**
- **Button** → fires its click callback.
- **List**: genuine two-stage model. While focused, `dy` moves the list's own
  highlighted index (fires `PURR_EVENT_SELECTED`, auto-scrolls); `dx` falls
  through group boundaries to leave the list (so **dy browses inside, dx
  leaves**). Enter/click fires `PURR_EVENT_ACTIVATED` on the highlighted item
  — the distinction those two enum values in `catcall_ui.h` already reserve
  room for but that no existing backend actually implements (KittenUI fires
  both simultaneously on every tap today).
- **Textarea**: Activate enters edit mode; while editing, a per-window
  `edit_target` intercepts every `KEY_DOWN` before focus-nav sees it and
  inserts/backspaces directly into that textarea's buffer. `Escape` (`0x1B`)
  exits edit mode without submitting. **No on-screen keyboard** — `kb_show`/
  `kb_hide` are empty-body stubs, exactly mirroring MiniWin's own existing,
  already-shipping precedent for physical-keyboard devices.

**Critical correctness rule: never fire a focus-change event through a
widget's own click callback.** Every real button callback in the 8 system
apps ignores its `purr_event_t` parameter and runs its side effect
unconditionally (`settings.c`'s `on_reboot`, `on_theme_wce`, etc. all do
`(void)e;`). If tabbing past a button fired the same callback used for
clicks, simply navigating with the trackball would trigger reboots/theme
changes as a side effect. Visual focus indication is handled purely
internally by the renderer (a 1px border rect redrawn as a two-rect diff on
focus change) — zero callback dispatch for focus movement, ever.

### A5. Hacker-terminal aesthetic

Flat, high-contrast, monospace — no bevels, gradients, or rounded corners
(those cost extra draw calls for zero readability gain, and don't fit the
brief). Own 6x8 monospace bitmap font (`font6x8.h`, own copy — not shared
with `blackpurr_shell.c`'s, keeping this landing fully isolated from existing
shipping code; de-duplicating later is a cheap optional follow-up).

```c
#define PW_COL_BG        0x000000   // black
#define PW_COL_FG        0x00FF41   // phosphor/"matrix" green — primary text & borders
#define PW_COL_DIM        0x008022  // dimmed green — secondary/disabled text
#define PW_COL_ACCENT     0xFFB000  // amber — warnings, low signal, alerts
#define PW_COL_FOCUS_BG   PW_COL_FG // focused widget: inverted video
#define PW_COL_FOCUS_FG   PW_COL_BG // (classic terminal cursor/selection look)
```

Widget borders are thin single-pixel `fill_rect` outlines, not text-drawn
box-drawing characters — keeps the "direct pixel push, no extra glyph blits
for decoration" performance goal intact while still reading unmistakably as a
terminal.

### A6. Meshtastic-first chrome

**Persistent status strip** — one row reserved at the top of the screen
(`STATUS_H` = `CHAR_H` + 2px padding = 10px), drawn by the shell itself
(`pounce_status.c`, not any app window), always visible regardless of which
window is on top of the modal stack. Window layout always subtracts
`STATUS_H` from available height, so app content and the strip never overlap
— no compositing needed, matching A3's "no clipping" simplification.
Redrawn on a timer (every ~1s, matching the WinCE taskbar's own RAM-readout
refresh precedent from this session's earlier work) since its data changes
slowly.

**Driven by a swappable provider table, not hardcoded fields** — this is the
actual mechanism for "able to swap the info out": each slot is a
name+render-callback pair; changing what the strip shows is a one-line table
edit, not a rewrite of any drawing code:

```c
typedef struct {
    const char *name;
    void (*render)(char *buf, size_t buflen);
} pw_status_item_t;

static const pw_status_item_t s_status_items[] = {
    { "mesh_rssi",   status_render_mesh_rssi   },
    { "mesh_nodes",  status_render_mesh_nodes  },
    { "mesh_unread", status_render_mesh_unread },
};
```

All three default providers use **existing, already-plumbed APIs — zero new
data plumbing required anywhere else in the codebase**:
- `mesh_rssi`/`snr` — `catcall_radio_t::rssi()`/`snr()` via `purr_kernel_radio()`
- `mesh_nodes` — `mesh_manager_node_count()` (already used by `meshchat.c`'s
  buddy list)
- `mesh_unread` — `purr_kernel_notify_count()` (the mesh router already calls
  `purr_kernel_notify()` for every incoming `TEXT_MESSAGE_APP` packet — this
  is the same counter WinCE's Start Menu already surfaces as a badge)

**Dedicated compose hotkey**: pressing `M` while **not** currently editing a
textarea (`edit_target == -1` — see A4) is intercepted globally, before
normal tab-order key handling, and calls `app_manager_launch_idx()` to
foreground/launch MeshChat. Safe by construction: BBQ20 has no modifier keys
(confirmed — raw ASCII only), so a bare-letter global hotkey is only safe
while nav-mode already owns the keystroke stream (i.e., not mid-typing) —
which is exactly the state this checks. `M` is a single `#define`, trivially
rebindable.

**v1 scope note, stated plainly**: this jumps to MeshChat's existing buddy
list, not directly into a compose text field — MeshChat's app entry point has
no argument/intent-passing mechanism today, and none of the other 7 apps do
either (a real, reusable, cross-app "launch with intent" mechanism would be
a legitimate but separate `app_manager.c` change, not scope-creeped into this
landing). Landing this now gets the one-key "get me to messaging" win
immediately; true inline quick-compose is a natural, cleanly separable
follow-up once intent-passing exists.

### A7. File-by-file plan

| File | Contents | Est. lines |
|---|---|---|
| `pounce/module.pcat` | module descriptor | ~12 |
| `pounce/CMakeLists.txt` | conditional `SRCS` on `CONFIG_PURR_UI_BACKEND_POUNCE` | ~25 |
| `pounce/pounce.h` | widget/window structs, pool decls, shared prototypes | ~130 |
| `pounce/font6x8.h` | own 6x8 monospace glyph table | ~110 |
| `pounce/pounce_win.c` | full `catcall_ui_t` implementation + `pounce_win_register()` | ~600 |
| `pounce/pounce_render.c` | draw primitives, `pw_layout_compute()`, `pw_win_full_repaint()`, per-widget redraw | ~320 |
| `pounce/pounce_focus.c` | group table, trackball accumulate+threshold, `move_focus()`/`activate_focused()`, textarea edit routing, focus-border draw | ~280 |
| `pounce/pounce_status.c` | status-strip render loop + provider table + `M` hotkey interception | ~120 |
| `pounce/pounce_module.c` | `.purr` header, dedicated FreeRTOS task (poll input, `purr_kernel_ui_lock()`-wrapped, ~60Hz), init/deinit | ~170 |

Total ≈ 1770 lines across 6 `.c`/2 `.h` files. Larger in aggregate than either
existing single shim (MiniWin's 808-line `miniwin_win.c`, KittenUI's 406-line
`kittenui_win.c`) — correctly reflects that Pounce does two things neither
existing backend does at all (its own renderer *and* a genuinely new
focus-navigation subsystem) — but carries zero third-party library weight,
and no individual file exceeds ~600 lines.

`pounce_module.c`'s task loop mirrors `blackpurr_module.c`'s input-polling
pattern, but must additionally take `purr_kernel_ui_lock()`/
`purr_kernel_ui_heartbeat()` around its per-tick work — matching the explicit
lesson already learned and fixed this session in `miniwin_task()`: any UI
backend's own render/input task must serialize against app tasks calling
`purr_win_*()` concurrently, or risk racing the shared SPI display driver.

### A8. Explicit v1 scope cuts

- No on-screen keyboard rendering (kb_show/kb_hide stubs, matches MiniWin's
  own existing precedent).
- Touch wired (module checks `purr_kernel_touch()`, null-safe) but not
  designed around — a minimal tap-to-activate hit-test is cheap to add, no
  gesture/drag/scroll chrome. GT911 stays secondary per the design brief.
- No word-wrap — labels split only on explicit `\n` (matches `settings.c`'s
  existing About text and BlackPurr's own clip-not-wrap precedent).
- No window drag/resize/floating chrome — full-screen modal stack only (A3).
- No type-to-jump in lists — a nice-to-have, cleanly separable follow-up.
- Fixed pools (`PW_MAX_WINS=16`/`PW_MAX_WIDS=256`) sized against real usage;
  exhaustion returns `0` from `alloc_wid()`, matching KittenUI/MiniWin's own
  existing graceful-exhaustion convention, not a new failure mode.
- True inline quick-compose (skip straight to a text field) deferred — see
  A6's v1 scope note.

---

## Part B — `.kitten` app extension (UI-agnostic)

### Context

Confirmed with the user, twice — `.kitten` is **not** Pounce-specific (works
under any UI backend), and it's an **interpreted Lua script tier like
`.hiss`** (not a compiled tier like `.claw`/`.paws`, my first read — corrected
by the user), with one new distinguishing behavior: a `.kitten` file found on
SD **autoruns at boot**, without the user manually launching it.

### Implementation

1. **`source/modules/app_manager/app_manager.h`**: add `APP_TIER_KITTEN` to
   `app_tier_t`, alongside `APP_TIER_MEOW`/`APP_TIER_HISS`.
2. **`source/modules/app_manager/app_manager.c`**:
   - `tier_from_ext()` recognizes `.kitten` → `APP_TIER_KITTEN`; `tier_name()`
     returns `"kitten"`.
   - `scan_dir()`'s extension filter (currently rejects anything but
     `.meow`/`.hiss`/`.paws`/`.claw`) adds `.kitten`.
   - The interpreted-tier dispatch in `app_manager_launch_idx()`
     (`if (app->tier == APP_TIER_MEOW || app->tier == APP_TIER_HISS) return
     launch_meow(app, i);`) extends to include `APP_TIER_KITTEN`, so `.kitten`
     scripts run through the exact same Lua VM/task path as `.meow`/`.hiss`.
   - `launch_meow()`'s privilege check
     (`s_meow_pending_privileged = (app->tier == APP_TIER_HISS)`) extends to
     `(app->tier == APP_TIER_HISS || app->tier == APP_TIER_KITTEN)` — "like a
     `.hiss`" means the same `kitt.*`/`radio.*`/`gps.*` Lua namespace access,
     not just the same file-discovery treatment.
   - **New: boot-time autorun.** After `app_manager_init()`'s initial
     `app_manager_scan()` completes, a one-shot check
     (`static bool s_kitten_autorun_done`, checked only here — **not** from
     `app_manager_scan()` itself, so a later manual rescan from Fileman/hot-
     reload never re-triggers it) looks for the first `APP_TIER_KITTEN` entry
     in the registry and calls `app_manager_launch_idx()` on it.
   - **Constraint to respect, not work around:** this codebase's Lua runtime
     is explicitly single-instance — `app_manager_get_pending_meow_path()`'s
     own doc comment states "only one Lua VM runs at a time on these boards."
     Autorun therefore launches the **first** `.kitten` file found, not every
     one present — identical to the existing limitation on manually launching
     multiple `.meow`/`.hiss` apps concurrently today. Not a new restriction
     introduced by this feature.
3. **`catstrap/catstrap.py`**: add a `.kitten` branch alongside the existing
   `.hiss` one in the app-discovery loop (`elif f.endswith(".hiss"): ...` →
   add the `.kitten` equivalent, tier string `"kitten"`), plus a tier colour
   table entry. The `.hiss`-specific `purr-sig` signature-tag/Developer-Mode
   gating system is **not** replicated for `.kitten` in this landing — out of
   scope for "basic loading, for now"; `.kitten` scripts run unconditionally
   like `.meow` does today, with the caveat that they also autorun at boot,
   so no signature/consent gate exists yet for that path either. Flagged
   explicitly, not silently glossed over.
4. **`purrstrap/purrstrap.py`**: add `"kitten"` alongside `"hiss"` in the
   runtime hot-load extension list(s) (`pkg app install/remove/upgrade`'s
   `for ext in ("meow", "hiss")` scan) — `.kitten` is SD-sourced and
   interpreted, so it belongs on the hot-load path, not the compiled-tier
   `.claw`/`.paws` build-time staging path.

**Follow-up worth flagging, not building now:** autorun with zero consent
gate is a real trust-boundary question (drop a `.kitten` file on someone's SD
card and it runs unattended at next boot) — `.hiss`'s `purr-sig`/Developer
Mode precedent exists specifically because of this class of concern. Left
out per "basic loading, for now," but the plan above deliberately keeps
`APP_TIER_KITTEN` a distinct enum value (not an alias) so that gate can be
added later without disturbing anything else.

Docs updates (`docs/06_Apps.md` etc.) are a natural follow-up, not part of
this landing.

---

## Verification

ESP-IDF v5.3.5 is now installed and working — builds can be run directly as
implementation progresses, not just eyeballed. For both parts:
- `purrstrap build tdeck_plus_pounce` (Part A) and a normal
  `purrstrap build tdeck_plus` (Part B, since `.kitten` recognition doesn't
  depend on which UI backend is active) should both compile cleanly.
- On-device verification (Pounce: window/widget rendering, focus-nav feel,
  status strip accuracy, `M` hotkey; `.kitten`: drop a trivial `.kitten` Lua
  script onto SD, power-cycle, and confirm it autoruns without manual
  launch, with `kitt.*`/`radio.*`/`gps.*` namespaces available and
  `terminal`'s `modules`/app list showing tier `kitten`) is deferred to
  actual hardware — will not be claimed done without a real flash+test pass,
  consistent with this session's existing practice.
