# Picture names each shell asks for

A UI pack (`pack.bin`, the format `purrstrap uiassets build` writes) is a set of pictures looked up **by name**. This is the list of names the two official shells ask for, read from the firmware sources (`PurrOS/main/purr_w10m*.c`, `purr_android*.c`, `purr_wallpaper.c`, `purr_catrt_ui.c`, `purr_w10m_settings.c`, `purr_w10m_perm.c` and `PurrOS/components/coreos/src/purr_statusicons.c`). It is also the criterion `purr ui check --shell w10m|android` checks a pack against (`sdk/purrsdk/ui.py` carries the same list; a test compares the two).

How to read it:

- **Required (R)**: a complete pack for the shell carries it. Without it the shell shows nothing where the picture belongs, a wrong stand-in (for example a refresh arrow for a quick tile) or a bare letter, which looks unfinished. A shell's **default** pack must have every R name.
- **Fallback (F)**: the code draws a deliberate, complete alternative when the name is missing (words on the key strip, a gradient behind the icons, a list symbol on the Apps button, the generic app icon). A **theme** pack may leave any name out: lookup is layered (the active theme's pack first, then the shell's default pack, plan7.md), so a theme that only changes some pictures is fine.
- The size is what the picture is drawn at when the code says so. Nothing is scaled except where marked; a different size is drawn at its own size and may look wrong, so keep the sizes.
- Pictures with transparency are stored as RGB565 plus an 8-bit alpha plane (3 bytes a pixel, `purrstrap uiassets build --alpha`); the wallpapers are opaque RGB565.
- A name is at most 23 characters (the directory entry holds 24 bytes with the end mark).

Read from the code, not run on a device.

## Windows 10 Mobile shell (`w10m`): 21 required, 10 fallback

The shell asks through `img()` in `purr_w10m.c` and `w10m_img()` in `purr_w10m_settings.c`. It does not use the `st_*` status icons (it draws its battery from shapes) or any `kb_*` or `fr_*` name.

| Name | Size | | Where it is drawn |
|---|---|---|---|
| `wp_settings` | 32 px | R | the Settings tile on the start screen (tile icon, centred) and the Settings row in the app list (drawn at 224/256). Fallback if missing: the letter "S" |
| `wp_system` | 32 px | R | Settings home, row "System" (drawn at 160/256, 20 px). The nine section rows share this behaviour: missing, the row has no picture and its text moves left |
| `wp_devices` | 32 px | R | Settings home, "Devices" |
| `wp_network` | 32 px | R | Settings home, "Network" |
| `wp_personalization` | 32 px | R | Settings home, "Personalization" |
| `wp_apps` | 32 px | R | Settings home, "Apps" |
| `wp_account` | 32 px | R | Settings home, "Accounts" |
| `wp_time` | 32 px | R | Settings home, "Time & Language" |
| `wp_privacy` | 32 px | R | Settings home, "Privacy & security" |
| `wp_about` | 32 px | R | Settings home, "About" |
| `wifi_icon` | 24 px | R | the status bar's network icon (drawn at 160/256, 15 px). Missing: no network icon in the bar |
| `wifi_ic` | 16 px | R | quick action "Network" in the action centre |
| `brightness_ic` | 16 px | R | quick action "Brightness" |
| `settings_ic` | 16 px | R | quick action "Settings" |
| `airplane_ic` | 16 px | R | quick action "Airplane" |
| `cellular_ic` | 16 px | R | quick action "Cellular" |
| `bluetooth_ic` | 16 px | R | quick action "Bluetooth" |
| `battery_ic` | 16 px | R | quick action "Battery" |
| `hotspot_ic` | 16 px | R | quick action "Hotspot" |
| `vpn_ic` | 16 px | R | quick action "VPN" |
| `location_ic` | 16 px | R | quick action "Location" |
| `img0` to `img9` | 320 x 569 | F | the wallpapers: the official packs carry only `img0` (see below) |

For a quick action whose picture is missing the tile shows a list or refresh symbol instead of its own icon (`purr_w10m.c`), which is why those ten are required. "Rotate" and "Tasks" have no picture by design.

Other apps' tiles on the start screen have **no** picture in this shell: they are letter tiles by design (only Settings has an icon), so a pack cannot theme them.

## Android shell (`android`): 31 required, 14 or more fallback

The shell asks through `w10m_img()` in `purr_android.c` (the same helper name as the other shell). The pictures are named for what they show.

| Name | Size | | Where it is drawn |
|---|---|---|---|
| `fr_app` | 48 px | R | the generic app icon: every app without a picture of its own, the Developer Portal, and the fallback of every other app icon. Also the app icon in the title bar of an app window (drawn at 128/256, 24 px) |
| `fr_settings` | 48 px | R | Settings, and the "Devices" app |
| `fr_folder` | 48 px | R | the "Files" app (an alias, used without a check) |
| `fr_gallery` | 48 px | R | apps named "Photos" or "Gallery" (alias, used without a check) |
| `fr_messaging` | 48 px | R | apps named "Messages" or "SMS" (alias, used without a check) |
| `fr_s_app` | 16 px | R | the notification icons in the status strip (four slots), and the app icon in the permission prompt. Missing: an empty slot |
| `st_b0`, `st_b10`, `st_b20`, `st_b40`, `st_b60`, `st_b80`, `st_b100` | 19 px | R | battery at level n (the code picks the step from the percentage: 0 up to 4, 10 up to 14, 20 up to 29, 40 up to 49, 60 up to 69, 80 up to 89, 100 above) |
| `st_bc0` to `st_bc5` | 19 px | R | battery charging animation, frames 0 to 5 |
| `st_bunk` | 19 px | R | battery, level unknown |
| `st_s0` to `st_s4` | 19 px | R | service (LoRa radio) signal, 0 to 4 bars |
| `st_snull` | 19 px | R | service, no service |
| `st_w0` to `st_w4` | 19 px | R | Wi-Fi, 0 to 4 bars |
| `fr_dock_apps` | 40 px | F | the Apps button in the dock. Missing: a list symbol |
| `kb_menu`, `kb_home`, `kb_back` | 20 px | F | the key strip legends, white with alpha, recoloured by the shell (grey at rest). Missing: the words "Menu", "Home", "Back" |
| `fr_phone`, `fr_contacts`, `fr_messaging`, `fr_browser`, `fr_camera`, `fr_gallery`, `fr_music`, `fr_clock`, `fr_settings`, `fr_video`, `fr_recorder` | 48 px | F | an installed app named like the part after `fr_` (lower case letters and digits of the app's name) gets that picture; any other name falls back to `fr_app`. The list is whatever a pack provides, not fixed: a pack may add `fr_<anything>` |
| `img0` to `img9` | 320 x 569 | F | the wallpapers (see below). Missing: the shell's gradient |

Counted: 6 `fr_*` required + 25 `st_*` = 31 required; fallback: `fr_dock_apps`, 3 `kb_*`, 10 wallpapers (14), plus the optional `fr_<app>` names.

Names in today's Froyo pack that the code does **not** ask for yet: `fr_dock_phone`, `fr_dock_browser`, `fr_dock_contacts`, `fr_dock_messaging`, `fr_s_messaging`, `fr_s_settings`.

**Do not put `wp_*` names in the Android pack.** The Android build also compiles the Settings code of the other shell (`purr_w10m_settings.c`), which asks for `wp_system` and the other eight `wp_*` section names; the Android layout has no row icons only because the pack has none of them. A `wp_*` picture in an Android pack makes Settings draw it.

## Shared by both shells

- **Wallpapers `img0` to `img9`** (since 1.0.29 the packs carry one: a surface whose picture is not in the pack, or the theme, draws `img0`, and the menus list only the pictures present, `purr_wallpaper_count()`; a theme or second pack adds `img1` onward) (320 x 569, opaque): both shells call `purr_wallpaper_asset()` (`purr_wallpaper.c`). Choice 0 ("Default") uses `img0` on the start screen, `img1` on the lock screen and `img3` on the sign-in screen; choices 1 to 10 use `img0` to `img9` on all three. They are drawn centred and cropped to the screen, never scaled. Today's Froyo pack has none (the Android shell shows its gradient); the Windows pack from `tools/ui/w10m_pack.py` has `img0` only (405 KB; `--wallpapers all` adds the other nine, 3.7 MB). The picture is placeholder art (`THIRD_PARTY.md`).
- No other name is shared. `st_*` and `kb_*` belong to the Android shell only.

## What the converted Windows pack provides

`tools/ui/w10m_pack.py` takes the upstream project's pictures (`UI/w10m-assets/NOTICE` has the commit and the list): **all 21 required Windows names are provided** (12 from upstream PNGs, 9 from the upstream SquareLine arrays: `wp_devices`, `wp_network`, `wp_personalization`, `wp_apps`, `wp_account`, `wp_time`, `wp_privacy`, `wp_about`, `wifi_icon`), and one wallpaper (`img0`). Nothing is missing. Upstream has more pictures the shell does not ask for (`wp_back`, `wp_next`, `wp_search`, `wp_logo`, `windows_logo`, `padlock`, tile art, app icons such as `phone_ic`); they are not converted.

The Android pack from `tools/ui/froyo_pack.py` provides all 31 required names, `fr_dock_apps` and the 3 `kb_*` (no wallpapers).

## Not in this list

- **Picture widgets of the config UI** (`purr_ui_config.c`): they name any asset in a `/ui` config (`asset = "name"`), so their names are the config's, not a fixed list.
- **Fonts**: a pack holds pictures only; fonts are separate (`docs/21-fonts.md`).
