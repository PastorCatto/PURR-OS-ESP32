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
