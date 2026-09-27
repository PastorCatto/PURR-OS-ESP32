# PURR OS Kernel Spine

This is the entire kernel. It is intentionally tiny.

## What it does
- Mounts flash VFS at boot
- Scans `/flash/modules` for `.purr` system modules
- Runs the catcall registry (one slot per catcall type)
- Provides system info (RAM, uptime, SD, WiFi, reboot)
- Parks in idle after handing off to modules

## What it does NOT do
- No display code
- No touch code
- No UI code
- No app launching
- No driver knowledge

All of those are modules.

## Files
| File | Purpose |
|------|---------|
| `boot.c` | `app_main()` — mount VFS, scan modules, idle |
| `purr_kernel.h` | Public kernel API (catcall registry, module loader, system info) |
| `purr_kernel.c` | Implementation |
| `purr_module.h` | `.purr` binary ABI — every module starts with `purr_module_header_t` |

## Catcalls
Defined in `../catcalls/`. One header per capability. Drivers implement these structs and register them via `purr_kernel_register_*()`. Everything else calls through `purr_kernel_display()` etc. — never directly into a driver.
