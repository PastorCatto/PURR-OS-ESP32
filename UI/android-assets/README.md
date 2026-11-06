# Android shell icons

Icons for the Android 2.2 (Froyo) style shell, from the Android Open Source Project under the
Apache License 2.0 (see `NOTICE` here and `THIRD_PARTY.md` at the top of the repository). One file per
asset; the file name without `.png` is the name the shell looks up in its picture pack (the `ui_assets` partition up to 1.0.28; `/ui/android/pack.bin` in LittleFS from 1.0.29).

| Prefix | What | Size |
|---|---|---|
| `fr_<app>` | launcher icons: phone, contacts, messaging, browser, camera, gallery, music, clock, settings, video, recorder; `fr_app` is the generic one, `fr_folder` a folder | 48 px |
| `fr_dock_<app>` | the dock: phone, browser, apps, contacts, messaging | 40 px |
| `fr_s_<app>` | small icons for the notification area of the status strip: messaging, settings, app | 16 px |
| `kb_menu`, `kb_home`, `kb_back` | the key strip legends (original art, white with alpha, tinted by the shell) | 20 px |
| `st_b<n>` | battery, level n (0, 10, 20, 40, 60, 80, 100) | 19 px |
| `st_bc<n>` | battery charging animation, frame n (0 to 5) | 19 px |
| `st_bunk` | battery, level unknown | 19 px |
| `st_s<n>` | service (cell) signal, n bars (0 to 4); `st_snull` is no service | 19 px |
| `st_w<n>` | Wi-Fi, n bars (0 to 4) | 19 px |

Made with `tools/ui/froyo_pack.py` from the Froyo resources; pack with

    python3 purrstrap/purrstrap.py uiassets build --board tdeck_plus --assets-dir UI/android-assets --alpha

The icons keep their alpha channel (RGB565 plus an 8-bit alpha plane, 3 bytes a pixel), 49 images in
about 145 KB. The wallpapers for the shell are separate and original.

## Licence

The icons here that come from Android (everything with a `fr_` or `st_` name) are the Android Open Source Project's, under the Apache License, Version 2.0 (`NOTICE`). The files that are
original to PURR OS (the `kb_` key legends, and any wallpaper or other art made for this shell) are offered under the same licence, the Apache License, Version 2.0, in respect of the
Android Open Source Project, as is the shell's source (the `purr_android` files in `PurrOS/main/`). The rest of PURR OS is under the GNU General Public License version 3 (`LICENSE`).

