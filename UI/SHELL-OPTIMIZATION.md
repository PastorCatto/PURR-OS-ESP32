# Building a fast shell on the PURR OS UI stack

A guide for anyone writing another shell (a launcher, a home screen, a lock screen and the sheets around them) on top of the stack that the W10M shell already runs on. It explains how that stack is put together, why the W10M shell is the smoothest UI the project has had, which of its habits are doing the work, and how to apply them to a different look: an Android-style phone shell, an iOS-style springboard, a console launcher, a kiosk.

It is written from the source, not from memory. Every rule names the file it comes from. Where something has not been measured on a device, the guide says so; the project's own judgement that the W10M shell is the smoothest so far is a feel, not a benchmark, and this guide does not turn it into numbers.

Contents: 1 Scope and honesty · 2 The stack · 3 The frame, from a tap to the panel · 4 Thirteen rules · 5 Memory budget · 6 Measuring · 7 Scenarios · 8 Anti-patterns and bugs this repo already paid for · 9 Checklist · 10 Where things live · 11 Art and licences

---

## 1. Scope and honesty

**What this covers.** The path from LVGL objects to pixels on the ST7789 panel of a T-Deck Plus (320 x 240, RGB565, SPI, ESP32-S3 with 8 MB of octal PSRAM), and the habits of `purr_w10m.c` and its neighbours that keep that path cheap. The same habits apply to other boards with a colour LCD; a board with a different panel (the e-paper board) needs the changes in scenario 3.

**What it is not.** It is not an LVGL manual and not a design guide. LVGL 9.2 documents its own widgets; this guide only says which of them cost too much here and what to do instead.

**What is measured and what is not.**
- Facts quoted from configuration files are exact: one 150 KB draw buffer in PSRAM, partial rendering, a 33 ms refresh period, 16-bit colour, a 40 MHz panel bus, 16-line DMA chunks.
- There is no frame-time benchmark for the W10M shell in the repository. The reasons it feels fast are the structural ones below; a new shell should be measured the way section 6 describes, and any number you publish should come from there.
- Known costs that have *not* been removed are listed as such (for example the per-pixel byte swap in the panel driver). They are the next places to look, not secrets.

**The stance.** A shell on this hardware is fast when it does *less*: fewer objects, fewer pixels touched per frame, no work on the UI task that could wait, no memory traffic that is not needed. Nothing here is a trick. It is a list of things not to do, and a few things to do once.

---

## 2. The stack

```
 apps (freestanding modules, one task each, 8 KB internal stack)
   |  `ui` catcall: integer handles; every call hops to the UI task
   v
 shell + app windows  ......... purr_w10m*.c, purr_catrt_ui.c     (what you replace or add)
   |  lv_obj_*, styles, animations, timers
   v
 UI service task ............. purr_ui_service.c   the ONLY task that touches LVGL
   |  lv_timer_handler() every <= 5 ms; commands arrive on a queue
   v
 LVGL port ................... purr_lvgl.c         one draw buffer, flush callback, input readers, rotation
   |  display->blit(x, y, w, h, pixels)
   v
 panel driver ................ Modules/drivers/st7789_module.c   byte-swap into a DMA buffer, 16 lines per SPI transfer, SPI lock
   |
   v
 ST7789 over SPI (40 MHz), shared bus with the SD card and the radio
```

Four properties of this stack decide how a shell should be written:

1. **One thread owns the UI.** LVGL is not thread-safe, so a single task (`ui_service`, priority 5, 12 KB stack) runs `lv_timer_handler()` and every request from other tasks (an app drawing a label, the power manager asking for a repaint) arrives as a message and runs there. This is also the rule that makes the shell robust: no lock is ever held across a draw.
2. **Rendering is partial and the buffer is small.** LVGL draws into one screen-sized buffer (`320 x 240 x 2` = 153,600 bytes, in PSRAM) in `LV_DISPLAY_RENDER_MODE_PARTIAL`, and hands each dirty rectangle to the flush callback, which hands it to the driver. What costs time is the *area* that changes, not the number of objects on screen.
3. **Images are not decoded.** Art is stored in a flash partition already in LVGL's native format and mapped into the address space. An image object holds a small descriptor that points at the mapped pixels. There is no PNG or JPEG decoder in the path and the image cache is zero (`CONFIG_LV_CACHE_DEF_SIZE=0`).
4. **Internal RAM is the scarce resource, not PSRAM.** `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` sends every allocation under 4 KB to internal RAM, which is where LVGL objects land. Every object a shell creates therefore competes with the app runtime for the same pool (see section 5).

---

## 3. The frame, from a tap to the panel

Following one interaction through the stack makes the later rules obvious.

1. **Input.** The touch controller and the keyboard are *polled* by LVGL's input readers (`touch_read_cb`, `kbd_read_cb` in `purr_lvgl.c`), called from `lv_timer_handler()`. The keyboard reader runs every 10 ms (the trackball needs it); the touch reader runs at LVGL's default period. A press that only wakes a sleeping screen is swallowed there (`purr_power_sys_activity()`), so it never reaches a widget.
2. **Event.** LVGL finds the object under the finger and sends it `PRESSED`, `CLICKED` and so on. A shell's handler runs on the UI task. It must be short: switch a flag, start an animation, post a request. Anything that can wait (opening an app, a network call, a file read) goes to a short-lived task (`open_task` in `purr_w10m.c`, `job_task` in the Settings and Developer Portal pages) that reports back through the UI service.
3. **Invalidate.** A style change, a hidden flag, a moved object marks the screen areas it touched as dirty. LVGL merges the dirty rectangles.
4. **Render.** On the next timer pass LVGL redraws the dirty rectangles into the PSRAM buffer, back to front, through every object that overlaps them. This is the expensive part, and it is proportional to *pixels x layers overlapping those pixels*.
5. **Flush.** For each rectangle, `flush_cb` calls the driver. If the display is rotated, the rectangle is first rotated into a second PSRAM buffer with `lv_draw_sw_rotate`.
6. **Panel.** The driver takes the SPI lock, sets the window (`CASET`, `RASET`, `RAMWR`), converts the pixels to the panel's byte order into an internal-RAM DMA buffer 16 lines at a time, and sends them. The bus runs at 40 MHz, shared with the SD card and the radio, so the lock matters (section 8).
7. **Idle.** `lv_timer_handler()` returns the time to the next timer; the service task sleeps on its queue for at most 5 ms and calls it again. With nothing dirty the whole loop costs almost nothing, which is the state a good shell spends most of its life in.

The consequence for a shell: **the cost of an interaction is (dirty area) x (layers under it) + (work done in handlers)**. Nearly every rule below shrinks one of those factors.

---

## 4. Thirteen rules

Each rule says what to do, why it matters here, where the W10M shell does it, and how to carry it over.

### Rule 1: Never block the UI task

**Do.** Keep every handler under a few milliseconds. Hand slow work to a short task and report back with `purr_ui_service_run()` or a notification.

**Why.** While a handler blocks, nothing redraws and no touch is read. A shell that waits for an app to start looks frozen. The runtime's `open` can itself wait for the app's start-up callback, so calling it on the UI task would stall the screen for that long.

**Where.** `open_def()` and `open_task()` in `purr_w10m.c` create a 4 KB task for each launch; the task calls the dispatcher and posts a notification if it fails. `purr_catrt_ui.c` marshals app calls to the UI task and returns integer handles.

**Carry over.** Any button that does more than change local state starts a task. The task must not touch LVGL objects directly; it calls `purr_ui_service_run(fn, arg, &result)` (not from the UI task itself, which would wait on its own queue) or posts a runtime event.

### Rule 2: Make the dirty area small

**Do.** Change as few pixels as possible per frame. Move small objects, not big ones. Animate a sheet by changing its position, not its contents.

**Why.** Render cost scales with area. A full-screen invalidate redraws 76,800 pixels through every layer; a battery icon updating once a minute redraws a few hundred.

**Where.** The status strip updates its own labels. The start screen is a scrolling container, so a swipe moves one object and LVGL redraws the strip of newly exposed pixels, not the content. The busy indicator in the strip animates opacity of two small objects.

**Carry over.** If you find yourself calling `lv_obj_invalidate(lv_screen_active())`, ask why. The only legitimate full invalidations are after the panel was owned by something else (a game, a sleeping panel); `purr_ui_service_pause(0)` and `purr_ui_service_repaint()` are the two places that do it.

### Rule 3: Pre-convert art, map it, do not decode it

**Do.** Ship icons and wallpapers as LVGL-native RGB565 (or RGB565 with a separate alpha plane), packed into the picture pack by `purrstrap` (the `ui_assets` partition up to 1.0.28; a file in LittleFS from 1.0.29, flash map v2), and build an image descriptor over the mapped pixels.

**Why.** Decoding a PNG on an ESP32-S3 costs milliseconds and RAM per icon, every time it is drawn unless it is cached. Native pixels read straight from flash through the cache cost nothing extra.

**Where.** `img()` in `purr_w10m.c` looks a name up once, builds the `lv_image_dsc_t` header (`cf`, `w`, `h`, `stride`, `data`) in a PSRAM table of 128 slots, and returns a pointer. `purr_ui_assets.c` maps the pack (the partition up to 1.0.28; from 1.0.29 it is a file read from LittleFS), checks its CRC in the background, and refuses to hand out more art if the data turns out to be damaged. A missing asset returns NULL and the caller draws without it (a letter tile instead of an icon), which keeps a half-installed system usable.

**Carry over.** Convert at build time, never on the device. Keep the name-to-descriptor cache. Provide a fallback for every image so a missing file never leaves a hole or an exception. Wallpapers are the biggest assets: choose by name at runtime (see `purr_wallpaper.c`, a device-wide setting that survives a change of shell).

### Rule 4: One style per role, shared by every object

**Do.** Define a small set of named roles (sheet, strip, tile, surface, text, dim, accent background, accent text, row pressed, icon) as `lv_style_t` objects, and apply a role to an object with one call. Never set colours or fonts locally on dozens of objects.

**Why.** LVGL resolves each object's style by walking its style list; a handful of shared styles is cheap and one change updates everything. Local styles allocate per object, in internal RAM.

**Where.** `purr_w10m_theme.c` keeps `s_st[PURR_TH_COUNT]` and `purr_theme_apply(obj, role)`. Changing the accent or the light/dark setting rewrites the shared styles in place and calls a change callback, which refreshes the few things that cached a colour (`theme_changed()` in `purr_w10m.c`).

**Carry over.** A new shell should define its own role table (the roles are the design language) in its own theme file and keep the *same shape*: an enum, an array of styles, `apply()`, `changed()`. Do not copy the W10M colours or accent names; they belong to that look.

### Rule 5: Solid is fast; blending is not

**Do.** Draw opaque surfaces. Avoid per-object opacity below 255 on large areas, drop shadows, gradients, blur, large rounded corners and wide outlines.

**Why.** The software renderer blends per pixel for anything translucent and runs an extra pass for shadows and gradients. On a 320 x 240 panel with no GPU, one translucent full-screen sheet turns a copy into a read-modify-write of every pixel under it.

**Where.** `s_transparency` defaults to 0, with the comment "solid by default: see-through sheets cost a blend per pixel". Tiles have radius 0. The only opacity animation is on two small labels in the status strip.

**Carry over.** Offer translucency as an *option* that is off by default, as W10M does. When a look needs depth, fake it with two flat colours. A one-pixel line is cheaper than a shadow; a flat pressed colour is cheaper than a glow.

### Rule 6: Build screens once, hide them, move them

**Do.** Create each major screen (start, app list, control center, lock, sign-in, an app's window) once, keep it, and show it by clearing `LV_OBJ_FLAG_HIDDEN`. Hide it again when it is off screen. Slide it with a position animation of 160 to 220 ms, and set the hidden flag when the animation completes.

**Why.** Creating and deleting objects allocates and frees internal RAM, fragments it, and does layout work at the worst moment (during a gesture). A hidden object is skipped by layout and by drawing, so an off-screen sheet costs only its memory.

**Where.** The control center is built at start-up, parked above the screen, and hidden when closed (`cc_move()`, `cc_anim_done()`). The app list slides in from the right (`list_move()`) and hides at the end. App windows are created on first use and hidden on pause (`purr_catrt_ui.c`). The task manager is built on first open and its refresh timer is paused when it closes.

**Carry over.** Decide the set of layers up front and put each in one of three places: the screen (the home and the app windows), the top layer (`lv_layer_top()`: lock, sign-in, control center, status strip, task manager), or nowhere (hidden). The order matters; keep it with `lv_obj_move_to_index()` and document it in a comment, as `purr_w10m_tasks.c` does ("above the app windows, beneath the status strip, the control center and the lock screen").

### Rule 7: Place big grids by hand; use flex only where it is small

**Do.** Compute positions for the home grid yourself and set them once. Use flex or grid layout only for short lists and rows.

**Why.** LVGL's layout engines recompute on every size or child change. For 40 tiles that is real work, and hidden children still have to be skipped one by one.

**Where.** `pack_tiles()` places each tile on a cell grid (70 px cells, 4 px gaps, three cells across the short side) and `relayout()` applies absolute positions. Lists such as the task manager use a column flex of a dozen rows at most.

**Carry over.** Give every object an explicit size. Use `LV_SIZE_CONTENT` only on small leaf containers. Clear `LV_OBJ_FLAG_SCROLLABLE` on everything that does not scroll, and remove `LV_OBJ_FLAG_CLICKABLE` from containers that should let taps through (see section 8, the row that swallowed taps).

### Rule 8: Pages share a window; pages do not share state with LVGL

**Do.** Give each app one window object. Let apps create widgets through a narrow table and refer to them by integer handle; deliver user input back as events on the app's own queue.

**Why.** It keeps app code from ever holding an LVGL pointer, so an app cannot crash the UI or leak objects: when it stops, its window and all its widgets are deleted in one place. It also caps the cost an app can impose (`PURR_CC_UI_MAX_WIDGETS` per window).

**Where.** `win_t` in `purr_catrt_ui.c`: a sheet, a header, a scrolling body, a handle table. The sheet is created on the first call, shown on `RESUME`, hidden on `PAUSE`, deleted on unload.

**Carry over.** A new shell inherits this for free as long as it keeps the window contract: a sheet over the home, a header with a back action, a scrolling body. A different look changes the *styles and the header*, not the mechanism. If a shell wants a different app window (a full-screen window with no header), add a variant in `purr_catrt_ui.c`; do not let apps draw with raw LVGL.

### Rule 9: Few timers, slow timers, and only while visible

**Do.** Use a timer only for something that changes on its own. Make it as slow as the information allows. Pause it when its screen is hidden. Rebuild a list only when what it shows has changed.

**Why.** Every timer wakes the UI task and may dirty an area. An always-on 100 ms timer costs nothing visible and still prevents idle.

**Where.** The clock ticks once a second. Banners use one-shot timers (5 s). The task manager's refresh timer runs every 2 s, only while open, and the list is rebuilt only when a signature of the running apps changes (so a scroll position is not reset).

**Carry over.** When you add a refresh, write down its period and its condition for running. If a shell animates something continuously (a live wallpaper, a spinner), it should be the *only* continuous animation and it should stop when the screen is off (the power manager asks the shell to repaint on wake; it does not ask for animation while asleep).

### Rule 10: Count objects, in internal RAM

**Do.** Budget the object count per screen and measure the internal RAM cost. Prefer one label with a long mode over several; prefer one container with a style over nested wrappers.

**Why.** Each LVGL object and its styles are small allocations, which `ALWAYSINTERNAL=4096` places in internal RAM. Two hundred extra objects can cost more than an app's task. The runtime refuses to start an app when internal RAM is too low (section 5), so a heavy shell takes apps away from the user.

**Where.** The W10M shell caps tiles at 40 (`MAX_TILES`) and images at 128. The task manager caps rows at 12.

**Carry over.** Set caps; show a "more" affordance past them. When a design asks for a decoration per item (a badge, a shadow, a separator), find the version that is a property of an existing object.

### Rule 11: Read input cheaply and decide in the reader when needed

**Do.** Keep input callbacks short and idempotent. If a gesture needs raw pointer positions (an edge swipe), detect it in the read callback; do not rely on object events.

**Why.** The keyboard reader runs every 10 ms on the UI task. A slow reader makes everything laggy at once. And press events from the pointer indev were not delivered to indev-level listeners in this LVGL build, which is why the edge swipe was detected inside `touch_read_cb` (with the rotation transform applied by hand).

**Where.** `purr_lvgl.c`: `touch_read_cb`, `kbd_read_cb`, the trackball sampling, `screen_point()`, `edge_swipe()`.

**Carry over.** Keyboards matter on this hardware: give every window an `lv_group_t`, map keys to the group's focus and activation, and add global shortcuts in `kbd_read_cb` (the game surface and the trackball do). A shell for a QWERTY device must be fully usable without touch.

### Rule 12: Rotation and sizes are data, not constants

**Do.** Read the size from the display (`w10m_disp_w()`, `w10m_disp_h()`) and recompute positions in one `relayout()`. Close transient overlays on rotation.

**Why.** The panel is landscape; the shell can run portrait. A rotated frame goes through `lv_draw_sw_rotate` into a second PSRAM buffer before it reaches the driver, which is a cost per flushed rectangle; do not rotate more often than the user asks.

**Where.** `purr_lvgl_set_rotation`, the rotate buffer in `purr_lvgl_init`, `relayout()` and `lock_layout()` in `purr_w10m.c`. Opening the task manager is refused while locked and it is closed on rotation.

**Carry over.** Every shell needs a `layout()` function that places everything from the current size, and every screen should call it on build and on rotation. Hard-coded pixel positions are the most common reason a shell breaks on a second orientation.

### Rule 13: Hand the panel over cleanly

**Do.** When something else owns the panel (a game, a sleeping screen), pause LVGL and repaint everything when it comes back. While the panel sleeps, drops are fine; stale frames are not.

**Why.** Two writers to one panel corrupt it. The game interface takes the screen exclusively (`purr_ui_service_pause(1)`), the power manager puts the ST7789 to sleep with the backlight off, and both rely on the shell to redraw on return.

**Where.** `purr_ui_service_pause()`, `purr_ui_service_repaint()` and the power platform in `purr_power_plat.c`. The panel driver holds the SPI lock for each whole blit and drops draws while the panel is asleep.

**Carry over.** Never write to the panel from a shell directly. All drawing goes through LVGL and the flush callback. If a new shell wants a full-screen video or game-like surface, it uses the game catcall's exclusive path rather than inventing its own.

---

## 5. Memory budget

Numbers that shape a shell (T-Deck Plus, current build):

| Resource | Value | Consequence for a shell |
|---|---|---|
| Draw buffer | 153,600 bytes, PSRAM, one buffer, partial mode | Every rectangle is rendered then flushed; no double buffering, so a flush blocks the next render |
| Rotation buffer | same size, PSRAM | Only used when rotated |
| Internal RAM at idle | about 42 KB (developer mode) to 50 KB free after boot, as measured by the owner | An app task needs 10 KB (8 KB stack + queue); the runtime keeps a reserve and refuses a launch it cannot afford |
| Allocations under 4 KB | internal RAM | LVGL objects and styles land here |
| Asset descriptors | PSRAM table, 128 slots | Free; the pixels stay in flash |
| UI task stack | 12 KB | Do not put big buffers on it |
| Fonts | Montserrat 12, 14, 16, 20, 28, 40 | Each extra size costs flash; a new font is a build option, not a runtime one |
| Refresh period | 33 ms (LVGL default here), service loop 5 ms | Animation steps are 33 ms apart at best |

Practical rules that follow:
- A shell should *report* its own footprint (the task manager's top line shows free fast RAM and main memory) and its designer should look at it after building every screen.
- Treat "free internal RAM at the home screen" as a number to defend. The W10M shell leaves roughly 42 to 50 KB; a shell that leaves 20 KB leaves room for one app, and every launch then evicts another.
- Prefer deleting an infrequently used screen's children (and rebuilding on open) to keeping a rich screen alive, *if* that screen is big and rare (the Developer Portal is an example). Prefer keeping screens that are opened constantly (the control center, the app list).

---

## 6. Measuring

There is no profiler enabled in the build (`LV_USE_SYSMON` and `LV_USE_PERF_MONITOR` are off). Measure with what exists:

1. **Frame cost.** Wrap `lv_timer_handler()` in `service_task` with `esp_timer_get_time()` before and after and log values above a threshold (5 ms is a sensible start); a rendered frame is one where the handler took longer than an idle pass.
2. **Dirty area.** Log the rectangle sizes passed to `flush_cb`. A sum near 76,800 pixels per frame means something is invalidating the whole screen; find it by hiding objects until the sum drops.
3. **Flush cost.** Time `s_display->blit` in `flush_cb`. This includes the byte swap and the SPI transfers, and is the number that the panel driver controls. Today the driver swaps bytes pixel by pixel on the CPU, reading from PSRAM; that is a known cost and an obvious candidate for a faster path (a swapped draw buffer, or a wider copy).
4. **Object count and memory.** `lv_obj_get_child_count()` recursively, plus `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` before and after building a screen.
5. **Host preview.** `tools/lvgl_host/render.py` builds the real LVGL, the real theme and widgets on the PC and renders pages to PNG. Use it to check layouts and rotations quickly. It does not measure time; ESP32 timings do not transfer from a PC.
6. **On the device.** The task manager shows the free numbers live; the `power` command and the runtime's status commands show the power and app state; `Settings > About > Last start` tells a crash from a brownout from a deliberate restart.

Report numbers with the conditions: build, orientation, what was on screen, and whether the screen had been idle. A frame time without those is not usable.

---

## 7. Scenarios

Each scenario lists the layers, the objects to budget, the animations to allow, and the traps. None is a finished design; they show how the rules combine.

### Scenario 1: An Android-style shell for a QWERTY 320 x 240 device

*Goal: a phone-like home screen on the T-Deck Plus, fully usable from the keyboard, with soft buttons for touch.*

Layers, bottom to top:
1. **Wallpaper** (a native RGB565 image, one object, behind everything).
2. **Home pages** (a horizontally scrolling container with snap; each page a fixed grid of icons placed by hand, four across, three down).
3. **Dock** (a fixed strip of four icons along the bottom, above the soft keys, on every page).
4. **Soft key bar** (home, back, menu, search; a narrow opaque strip; hidden when a physical key mapping is active).
5. **Top layer:** status strip (clock, battery, network), the notification shade (parked above the screen, pulled down by a swipe or a key), the lock screen.
6. **App windows** over the home pages, as in rule 8, with the header drawn in the new style.

Budget: about 12 icons per page x 3 objects (image, label, container) x 3 pages, plus the dock and strips: under 150 objects. Keep the app drawer as one persistent hidden sheet with a flex grid of at most the installed apps; page it if the list is longer than about 24.

Animations: page snap (LVGL's own scroll), the shade slide (160 to 220 ms ease-out, then hide), the window slide. No parallax or icon bounce.

Keyboard: a physical home key mapped in `kbd_read_cb` (a global shortcut that calls the same function the soft Home button calls), arrow keys moving the focus ring across the icon grid with `lv_group` focus, Enter opening, Backspace going back, a key for the shade. The focus ring is a flat 2 px outline, not a glow.

Traps: do not use translucent shades (rule 5); do not build the drawer's icons on every open (rule 6); do not put the dock inside each page (it would be duplicated and scrolled).

### Scenario 2: An iOS-style springboard

*Goal: a grid of rounded icons on a wallpaper, pages with dots, folders.*

The look relies on exactly the effects this hardware punishes: blur behind folders, soft shadows under icons, large corner radii, springy animations. The rules give the equivalents:
- **Rounded icons:** bake the rounded mask into the icon art at build time (an alpha plane in RGB565A8) instead of `lv_obj_set_style_radius` plus clipping on each icon, which forces masking per frame.
- **Shadows:** none. Use a slightly darker flat tile under the icon if the design needs separation.
- **Folders:** a persistent hidden sheet with a flat opaque background that slides up; no blurred wallpaper.
- **Page dots:** one label or a tiny container of three small objects, updated on page change only.
- **Springs:** replace with ease-out at 160 to 200 ms. A bounce is two animations; do it only for the "wrong password" shake.

Budget: 4 x 4 icon grid per page at 320 x 240 is tight in landscape; either use five across by three down with smaller labels or run the shell in portrait and accept the rotation buffer cost.

### Scenario 3: A monochrome or e-paper shell

*Goal: the same system on a 1.54 inch e-paper panel.*

An e-paper panel refreshes in hundreds of milliseconds and ghosts if refreshed too often, so the animation rules invert: **no animations at all**, and update in whole-screen steps. Layers still exist, but a transition is a hide and a show. Draw with flat black and white (`LV_COLOR_FORMAT_I1`-style assets), no anti-aliased text where the panel cannot show it, and coalesce updates: invalidate, wait until input has been quiet for a moment, then flush once. The display driver already has a different `set_power` model for this board. Keep the console fallback (`purr_console`) working: it is the path that works when the graphical shell is off.

### Scenario 4: A kiosk or appliance shell

*Goal: one full-screen app, no launcher, no home screen.*

The shell reduces to the status strip (optional), one app window, and the lock screen. Build the app window at start and show it; do not build the start screen, the app list or the control center at all (their objects never exist, so they cost nothing). The rules that matter: rule 1 (the app does its work off the UI task), rule 12 (size from the display) and rule 13 (power handover). Disable the task manager tile; there is nothing to switch to.

### Scenario 5: A handheld game launcher

*Goal: a grid of game covers, launching full-screen emulators.*

The launcher is an ordinary shell (cover images are native assets read from SD or flash through the files catcall, converted ahead of time) until a game starts. Then rule 13 applies: the game catcall pauses LVGL, takes a screen-on lock, puts the keyboard in raw mode, and a held menu key returns to the shell, which repaints. Memory is the constraint: a game needs its task, the runtime reserves less internal RAM for a game than for other apps (8 KB instead of 24 KB), and the launcher should hold as few objects as it can while a game runs (hide the home sheet; do not keep a rich list alive behind a game).

### Scenario 6: Adding a settings-style page cheaply

*Goal: one more page of options, not a new shell.*

A page is a sheet that is built on first open and hidden afterwards, with rows made from the shared roles. Rows are `PURR_TH_SURFACE` containers with a label and a control; the control's handler writes a setting and, if it needs slow work, starts a job task. Reuse the helpers in `purr_w10m_widgets.c` (`w10m_text`, `w10m_dim`, `w10m_title`, `w10m_switch`, `w10m_slider`, `w10m_dropdown`, `w10m_button`, `w10m_input`) rather than creating new row styles. If the page is large and rarely opened, build it on open and delete it on close; if it is opened often, build once and hide.

---

## 8. Anti-patterns and bugs this repository already paid for

Each of these cost real debugging time. They are listed so the next shell does not repeat them.

| Symptom | Cause | Fix |
|---|---|---|
| Tapping empty space in a list did nothing, or tapped the wrong row | A container with `LV_OBJ_FLAG_CLICKABLE` swallowed the press | Remove CLICKABLE from containers that are not controls (`lv_obj_remove_flag`) |
| Stuck blank screen after an update | The service loop checked its deadline before reading its queue and starved requests | Do the receive at least once per pass (the `do/while` in `service_task`) |
| Screen showed white and the shell never appeared | A stale driver blob compiled into the kernel did not match the driver bus ABI | Re-embed and re-sign driver blobs whenever a driver changes (see `plan2.md`) |
| An app crashed on its first `switch` statement | The jump table mid-function addresses were mis-relocated | The module relocator treats any address inside a function as code (`purrstrap/scripts/modules.py`) |
| The whole UI froze when something waited for an app | A handler called a blocking runtime function on the UI task | Start a short task (rule 1) |
| A swipe from the bottom did not register | Pointer press events did not reach indev-level listeners | Detect the gesture inside the touch read callback (rule 11) |
| A game with no keys pressed looked frozen after a while | The idle timeout blanked the screen and nothing could wake it (the shell was paused) | Hold a screen lock for the game (`purr_catrt_game.c`) |
| Opening Device Manager failed with "for administrators" | An app asked for permission while the runtime thought it was not in front | A starting UI app counts as foreground for the prompt |
| Wake from sleep could deadlock | The power manager waited on the UI task while holding its own lock, which the UI task needed | Do screen changes after dropping the lock, on the power task (`purr_power_plat.c`) |
| Corrupt or torn pixels | Two tasks sending to the panel, or a sleep command between a window and its pixels | The panel driver holds the SPI lock across each whole blit; only LVGL's flush draws |
| An overlay stayed up after rotation | Layout positions were computed once | `relayout()` on every rotation; close transient overlays |
| The task manager list jumped back to the top | The list was rebuilt on a timer | Rebuild only when a signature of the content changes |

Two general lessons beyond the table: **cache the answer, not the question** (the asset descriptor table, the signature of the task list) and **if it can be an object property, do not make it an object**.

---

## 9. Checklist for a new shell

Before a shell is merged:

- [ ] All LVGL calls happen on the UI task; everything else goes through `purr_ui_service_run` or a posted event.
- [ ] No handler blocks for more than a few milliseconds; slow work is on a short task.
- [ ] Every screen is built once, hidden when off screen, and moved with a 160 to 220 ms ease-out animation.
- [ ] Layers are placed deliberately (screen, top layer, hidden) and their order is written in a comment.
- [ ] One role table (theme) drives all colours and fonts; no local colours except one-off art.
- [ ] No translucency, shadows, gradients or blur unless behind an option that is off by default.
- [ ] Art is native RGB565 (or with an alpha plane), read from the assets partition, with a fallback when missing.
- [ ] Positions come from the display size in one `layout()`; rotation tested in both orientations.
- [ ] The shell works from the keyboard alone (focus groups, a home key, a back key) and from touch alone.
- [ ] Timers are listed with their period and the condition under which they run.
- [ ] Object counts and free internal RAM measured after each screen is built, and written down.
- [ ] The panel handover paths (game, sleep, lock) tested: the screen repaints correctly afterwards.
- [ ] Theme, wallpaper, brightness and the power settings are the device-wide ones (`display.*`, `power.*`); the shell reads them and does not own copies.
- [ ] Nothing is written to the panel except through LVGL.

---

## 10. Where things live

| Piece | File |
|---|---|
| LVGL port: draw buffer, flush, rotation, touch, keyboard, trackball, edge swipe | `PurrOS/main/purr_lvgl.c` |
| The UI task, request queue, pause and repaint | `PurrOS/main/purr_ui_service.c` |
| The shell: start screen, tile packer, control center, app list, lock, sign-in, status strip | `PurrOS/main/purr_w10m.c` |
| The theme roles and styles | `PurrOS/main/purr_w10m_theme.c` |
| Shared widgets (rows, switches) | `PurrOS/main/purr_w10m_widgets.c` |
| App windows and the `ui` catcall | `PurrOS/main/purr_catrt_ui.c` |
| Settings pages | `PurrOS/main/purr_w10m_settings.c` |
| The task manager | `PurrOS/main/purr_w10m_tasks.c` |
| Art (mapped from flash up to 1.0.28, a LittleFS file from 1.0.29) | `PurrOS/main/purr_ui_assets.c`; packed by `purrstrap` |
| Wallpaper (device-wide) | `PurrOS/main/purr_wallpaper.c` |
| Panel driver | `Modules/drivers/st7789_module.c` (embedded in the kernel in `commands.c`) |
| Power and the panel sleep | `PurrOS/main/purr_power_plat.c` |
| Game surface | `PurrOS/main/purr_catrt_game.c` |
| Host layout previewer | `tools/lvgl_host/` |
| Shell choice | the `ui.shell` setting (read at start; `w10m` (the official shell) and `nav` today) |
| Plans | `plan7.md` (shell), `plan6.md` (runtime and games), `plan9.md` (ui and files catcalls), `plan10.md` (power) |

Adding a shell means: a new `purr_<name>.c` set that exports the same start and layer contracts, a theme file, an entry in the `ui.shell` setting, and a block in `PurrOS/main/CMakeLists.txt`. The runtime, the catcalls, the settings store and the panel path are shared and should not need changes. If you find you need to change them, that is a finding to write down in the plan, not a reason to fork them.

---

## 11. Art and licences

- **Code.** The shell code in this repository is the project's own. Keep new shells' sources under the same licence as the repository, and keep the SDK-facing headers (the catcall tables an app sees) permissive, which is the intended direction, so that closed apps and closed shells are clearly allowed. Check the repository's licence files for the current state before relying on this.
- **Art.** Do not ship another company's artwork or trade dress. A shell may reproduce a *layout idea* (a grid, a dock, a pull-down shade) with original art. The W10M shell started from a public reference project and its attribution is in `THIRD_PARTY.md`; art that came with it that belongs to a third party is being replaced with original art. The project's own rule is: keep the icons already in use, do not add new ones in another vendor's style, and make new art original.
- **Open resources as a source.** The Android Open Source Project publishes many resources (for example the launcher and application icons of its 2.x releases) under the Apache License 2.0, with a NOTICE file in each repository. They can be used with attribution and the notice. Google's own application icons (its mail, maps and store apps) and phone makers' skins are not part of that and are not usable; the Android robot has its own terms. Check each file's repository for its notice before using it, and record what was used in `THIRD_PARTY.md`.
- **Not legal advice.** A short review before shipping a product is sensible.

---

*Written 2026-11-04 from the source at that date. If a rule here and the code disagree, fix one of them in the same change.*
