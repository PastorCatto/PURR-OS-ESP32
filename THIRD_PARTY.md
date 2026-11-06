# Third-party material

## windows-10-mobile-lvgl

The Windows 10 Mobile style shell (`PurrOS/main/purr_w10m.c`, plan7.md) is modelled on, and uses
the unchanged artwork of, **windows-10-mobile-lvgl** by Felix Biego,
<https://github.com/fbiego/windows-10-mobile-lvgl>, under the MIT licence:

> MIT License
>
> Copyright (c) 2024 Felix Biego
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
> associated documentation files (the "Software"), to deal in the Software without restriction,
> including without limitation the rights to use, copy, modify, merge, publish, distribute,
> sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
> NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The images in that project (icons, tile art, wallpapers, and the Windows and Lumia imagery among
them) are packed into the `ui_assets` partition unchanged for now (`purrstrap uiassets
import-lvgl`). Some of that imagery is not Felix Biego's to license (the Windows logo and Lumia art
are Microsoft's), so **it is placeholder art only and must be replaced before anything is
shipped**. The packaging is built so the replacement is a drop-in: a new `ui_assets` image.

## LVGL

LVGL (MIT) is used as a managed component, version 9.2.x, <https://lvgl.io>.

## Android Open Source Project (Android 2.2 "Froyo" icons)

The Android style shell (`PurrOS/main/purr_android.c`, plan7.md, "Android shell build-out") draws its
launcher, dock and status-bar icons from the **Android Open Source Project**, copyright (c) The
Android Open Source Project, licensed under the **Apache License, Version 2.0**
(<https://www.apache.org/licenses/LICENSE-2.0>); the licence text and notice are in
`UI/android-assets/NOTICE`. They come from the Froyo (Android 2.2) release: the launcher and dock art
from the Launcher2, Phone, Contacts, Browser, Settings, Camera, Messaging, DeskClock and Music
repositories, and the battery, signal and Wi-Fi status icons from `frameworks/base`
(`core/res/res/drawable-mdpi`, branch `froyo-release`).

**Changes made:** each icon was resampled once, offline, to the size the shell draws it at
(`tools/ui/froyo_pack.py`) and renamed for its asset. Nothing is scaled on the device. The
resampled PNGs are in `UI/android-assets/`; `purrstrap uiassets build --alpha` packs them.

**Licence of the shell itself.** The Android style shell's source (`PurrOS/main/purr_android*.c` and `.h`, each marked with an SPDX line) and the original artwork made for it (the `kb_` key legends and the wallpapers) are released under the Apache License, Version 2.0, the same licence as the Android resources above, in respect of the Android Open Source Project. The rest of PURR OS is under the GNU General Public License version 3 (`LICENSE`).

Not used, on purpose: Google application artwork (Gmail, Maps, Market and the like) and any phone
maker's skin or icons, which are not under that licence. The shell's wallpapers and layout are
original.

