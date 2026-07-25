# 09 — System UI

> **Accurate as of v1.0.0-dp8.** Verified against `source/modules/systemui/`.

The system UI is the persistent chrome that draws *above* every app window:
status bar, drag-down panels, the nav bar or home indicator, the app switcher,
and the lock screen.

It is a module (`source/modules/systemui/`), not part of any UI backend. A
backend **hosts** it — Cupcake, Tabby and Mochi all do — by handing over a table
of callbacks and then driving it from its own render loop.

---

## Why it is a separate module

All of this used to live inside `cupcake_ui.c`, which had grown to hold both the
launcher (home screen, wallpaper, dock, app drawer) and the chrome. Splitting
them meant a second backend could reuse the chrome instead of copying it — and
that reuse is the actual proof the split worked, since Tabby and Mochi now host
the identical module.

The separation was always latent: every surface here is built on
`lv_layer_top()`, a compositing layer LVGL paints and hit-tests above the active
screen's entire tree. None of it ever depended on the launcher's object
hierarchy — the two just shared a file and a pile of file-static state.

---

## Two styles, one contract

Exactly one of two implementations compiles into a build:

| File | Style | Character |
|---|---|---|
| `systemui_android.c` | Android | Back/Home/Recents nav bar, drag-down panels as plain text rows, battery-left status bar, centred lock screen. |
| `systemui_ios.c` | iOS | No nav bar (host draws a home indicator). Notifications as rounded cards with app icon, accent colour and relative timestamp. Clock-left status bar. Horizontal card app-switcher. |

Both implement the identical `purr_systemui_*` API, so **a host needs no
changes to switch styles**.

### Selecting a style

Kconfig choice `PURR_SYSTEMUI_STYLE`, driven from `device.pcat`:

```ini
[ui]
systemui_style = "ios"      # or "android" (the default)

[modules]
ui       = "mochi"
systemui = "systemui"       # presence of this enables the module at all
```

**`systemui_style` is deliberately not in `[modules]`.** purrstrap's
`_generate_glue()` turns every `modules.*` value into a static module
registration, so a style name there would emit
`extern purr_module_ios;` — a symbol that does not exist. `[ui]` is inert to
glue generation and read only by `_sdkconfig_lines()`.

### How one-of-two compilation works

Both `.c` files are always listed in `CMakeLists.txt`. Each guards itself:

```c
// systemui_ios.c
#if defined(CONFIG_PURR_SYSTEMUI) && defined(CONFIG_PURR_SYSTEMUI_STYLE_IOS)
    /* real implementation */
#endif

// systemui_android.c
#if defined(CONFIG_PURR_SYSTEMUI) && defined(CONFIG_PURR_SYSTEMUI_STYLE_ANDROID)
    /* real implementation */
#elif !defined(CONFIG_PURR_SYSTEMUI)
    /* no-op stubs for every API function */
#endif
```

The unselected file compiles to an empty translation unit, so exactly one set of
symbols links. Listing both unconditionally keeps the file set identical across
a style switch — changing visual language is a rebuild, not a CMake reconfigure.

When `CONFIG_PURR_SYSTEMUI` is off entirely, `systemui_android.c` supplies no-op
stubs for the whole API (the same shape `meshtastic_module.c` uses for its own
gate), so **a host calls `purr_systemui_*()` unconditionally with no `#ifdef`**
and the linker always resolves.

---

## Threading — the module has no task

This is the constraint everything else follows from: **LVGL is not
thread-safe**, and the whole UI serialises on the host backend's render loop
under `purr_kernel_ui_lock()`.

So `systemui` deliberately has no task of its own. The host calls:

- `purr_systemui_init(&host)` once, after its own screens exist;
- `purr_systemui_tick()` periodically (~200ms) from inside its render loop.

Everything in the module runs on the host's task. Giving it a task would mean
taking the UI lock from two places, which is not worth the deadlock surface.

The module's `init()` in `systemui_module.c` therefore builds *nothing* — it is
a presence marker so the module appears in the registry (and in terminal's
`modules` command). The real construction happens later, on the host's task.

---

## The host contract

```c
typedef struct {
    uint16_t (*width)(void);
    uint16_t (*height)(void);

    const lv_img_dsc_t *(*icon_for_app)(const char *name);   // never NULL-returning
    lv_color_t          (*tint_color)(const char *name, uint8_t base);

    void (*hide_drawer)(void);              // OPTIONAL — NULL if no drawer
    void (*hide_foreground_windows)(void);
    uint64_t (*last_activity_ms)(void);

    const lv_img_dsc_t *(*wallpaper)(void); // OPTIONAL — may return NULL
    bool suppress_navbar;
} purr_systemui_host_t;
```

Build it as a `static const` — it is retained, not copied.

**No `lv_obj_t*` crosses this boundary in either direction.** The host and the
system UI each own their object trees; only values, indices and calls pass
between them. That is what makes the module backend-agnostic rather than
Cupcake code in a different folder.

| Field | Why the host answers, not the module |
|---|---|
| `icon_for_app` / `tint_color` | So a Recents card carries the same visual identity the app has on the launcher. Mochi returns a fixed saturated palette; Cupcake hashes to a tint. |
| `hide_drawer` | Only the host knows whether it *has* an app drawer. Mochi's grid *is* the home screen, so it resets to page 0 instead. |
| `hide_foreground_windows` | Must hide **every** window the app opened, not just the one `app_manager` tracked — an app that opened a sub-window would otherwise leave it visible. |
| `wallpaper` | Each host resolves "current wallpaper" differently: Mochi has one compiled into firmware, Cupcake loads a user-selected image off SPIFFS/SD and may have none. **NULL is a normal answer**, not an error. |
| `suppress_navbar` | A plain `bool`, not a callback, so a `static const` that omits it zero-fills — existing hosts keep their nav bar with no edit. |

### `suppress_navbar`

Omits only the Back/Home/Recents bar; every other surface stays. For a host
whose design language has no such bar — an iOS springboard uses a home indicator
and gestures — an Android nav bar underneath simply looks wrong.

When set, `purr_systemui_navbar_height()` also reports 0, so a host laying out
bottom-docked content reclaims the space rather than leaving a gap.

Recents is **not** lost when the bar is suppressed. The host reaches it via
`purr_systemui_open_recents()` — Mochi binds that to a long-press on its home
indicator.

---

## API

```c
void    purr_systemui_init(const purr_systemui_host_t *host);
void    purr_systemui_tick(void);
int16_t purr_systemui_navbar_height(void);   // 0 if suppressed or compiled out

int  purr_systemui_foreground_idx(void);     // -1 = home screen
void purr_systemui_enter_app(int idx);       // records + auto-hides the bars
void purr_systemui_return_home(void);        // clears + restores the bars

void purr_systemui_open_recents(void);
bool purr_systemui_recents_open(void);
void purr_systemui_close_recents(void);

bool purr_systemui_is_locked(void);
void purr_systemui_wake(void);               // restore brightness, stay locked
```

### Foreground tracking lives here, not in the host

The module owns "which app is foregrounded" because almost every mutation point
is its own — nav Back/Home, the Running Apps panel's Open/Kill, Recents card
tap/kill. The host only contributes its launcher taps, via
`purr_systemui_enter_app()`.

**A host that suppresses the nav bar must call `purr_systemui_return_home()`
itself** when it returns to its home screen. Under the Android style that call
is the nav bar Home button's job; with the bar suppressed it has no owner, and
omitting it leaves the status indicators hidden forever after the first app
launch.

---

## Notification privacy

The lock screen defaults to showing a **count only** — `3 Notifications` plus
`swipe up to show` — rather than message contents. A lock screen is by
definition what an unauthenticated onlooker sees, so bodies and sender names
should not be readable across a desk by default.

Backed by `purr_kernel_lock_hide_notifications()` and toggled in
**Settings → Customization**, persisted to NVS under
`"purr_settings"/"lock_hide_notifs"`. Both styles honour it.

The reveal is **per-lock**: locking again re-hides, or someone picking the
device up later would find the list already open.

> Both lock screens guard against a subtlety: LVGL delivers a click on release
> even after a swipe, so without checking the input vector the reveal gesture
> would unlock the device instead of showing the notifications.

Cupcake's lock screen is asymmetric here — it only ever displayed a bare count,
i.e. it was already permanently "hidden". Honouring the setting there meant
building the **revealed** state, not the hidden one, and it renders as a
compact multi-line label capped at three entries rather than a card list.

---

## The shared notification card (iOS style)

`build_notif_card()` renders the app's icon on its accent colour, the source
name, a relative timestamp (`now` / `3m` / `2h`), title and body — and **both**
the Notification Center and the lock screen call it.

That sharing is the point, not an optimisation: on iOS a notification looks
identical wherever it appears, so a single builder is what keeps the two
matching instead of drifting apart.

No kernel change was needed for any of it — `purr_notification_t` already
carried `title`, `body`, `source` and `timestamp_ms`.

Swipe a card sideways to reveal a red clear button; tapping it removes that one
notification via `purr_kernel_notify_remove(idx)`. Revealing a button rather
than deleting on the swipe itself is deliberate: an accidental brush should not
silently destroy something, and there is no undo.

---

## Adding a new host

1. Build a `static const purr_systemui_host_t` (see Mochi's
   `mochi_springboard.c` or Cupcake's `cupcake_ui.c`).
2. Call `purr_systemui_init(&host)` after your own screens exist — no `#ifdef`.
3. Call `purr_systemui_tick()` from your render loop.
4. Call `purr_systemui_enter_app(idx)` when you launch or restore an app.
5. Call `purr_systemui_return_home()` when you return home — **required** if you
   set `suppress_navbar`.
6. Lay bottom-docked content out against `purr_systemui_navbar_height()`, not a
   constant.

---

## Known constraint

LVGL-only today. Every surface is built on `lv_layer_top()`, and MiniWin has no
equivalent compositing layer (see `miniwin_lock.h`) — which is why MiniWin keeps
its own separate lock-screen implementation rather than consuming this module.
Pounce likewise.

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. This
document was written against `source/modules/systemui/` as implemented.*
