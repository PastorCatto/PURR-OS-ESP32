# The kit on Windows

A Windows kit is built on a Windows computer with `python sdk\build-kit.py`. It has `bin\purr.cmd` and `PURR-Studio.cmd` in place of `bin/purr` and `PURR-Studio.command`, and it has **no emulator** (the patched QEMU is not built for Windows). The commands in the other guides are the same; write `bin\purr` for `bin/purr`.

**Status:** written on 2026-11-08 and **not yet run on Windows**. The full steps (install, clone, line endings, long paths, flashing over a COM port) and the list of what is untested are in [docs/22-building-on-windows.md](../../docs/22-building-on-windows.md).
