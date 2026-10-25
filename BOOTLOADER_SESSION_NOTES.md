All of it is written but none of it has been built or run on hardware. IDF 5.3.5 isn't installed on this machine. The only check I could run was the CoreOS host test for the handoff helpers, which passes (`test_cfg`, 464 checks, 0 failed).

## Done

- **Ladder:** The code is the authority. In `bootloader/SPEC.md` I rewrote step 3a and the §9 note to match it. The bootloader does the increment, and the tiers are 1–3 kernel, 4–6 KittenOS, 7+ loader.
- **Handoff:**
  - `purr_handoff.c` writes the 16-byte block into IDF's custom RTC area just before the jump. `purr_kernel_handoff()` reads it, and the shell's `purrcfg` command prints it.
  - The validity helpers are shared, with a host test.
  - `FORCED_RECOVERY` and `CONFIG_DEFAULT` are real states.
  - I added `AUTO_REINSTALL`, which the recovery loader needs, and `UNCHECKED`. A normal boot only passes the `0xE9` magic check, so reporting `VERIFIED` would be false.
  - `UNCHECKED` becomes `VERIFIED` when signature checking lands.
- **Boot package at 56 KB:** 56 KB at 0x12000 would collide with `purrcfg` at 0x1E000. So `nvs` gives up 8 KB (24 KB to 16 KB), `purrcfg` moves to 0xD000, and `bootpkg` becomes 0xE000. This is applied in all six CSVs. If you'd rather keep `nvs` at 24 KB, the other free space to take is `otadata`, which `net_install` still needs.
- **mmap crash:**
  - The partition table is now read once into RAM, so partition lookups never map flash.
  - The boot package's own partition is the only mapping the bootloader ever holds. `purrcfg` is read before it is taken.
  - I removed the old caching workaround, which only worked because of call order.
- **One bootloader:** The shared source is now `purr_bootloader_component.cmake` in `bootloader/`. `PurrOS/` and `Kernel/` each have a two-line `bootloader_components/main/CMakeLists.txt` that includes it. So `idf.py flash` from either project builds and flashes the PURR bootloader instead of IDF's stock one.

## Check on first build

I wrote these from memory of the IDF API:
- The RTC member name `custom` and the symbol `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE`.
- `bootloader_common_update_rtc_retain_mem(NULL, false)`.
- Whether the app side links `bootloader_common_get_rtc_retain_mem` through `bootloader_support`.

A wrong guess should show up as a compile or link error, not a silent failure.

**Existing devices:**
- Reflash the bootloader and the partition table.
- Erase `nvs`. `purrcfg` resets to defaults at its new offset.
- `root` is untouched.
- Delete each project's cached `sdkconfig` so the new RTC defaults apply.

## Signature checking pitch

I recommend putting the signature in a **trailer inside each app partition**, which is a variant of the spec's option 1 (the separate metadata slot).

- **Layout:** The ESP image stays at offset 0, so the stock loader is untouched. The last 4 KB of `kernel`, `kittenos` and `loader` holds a `purr_image_header_t` plus signature.
- **What's signed:** The header's `payload_sha256` is set to the SHA-256 that IDF already appends to every app image. The bootloader reads those 32 bytes, checks them against the signed header, and verifies the ECDSA signature. It doesn't hash 1.9 MB itself. IDF's own digest check, which stays on, binds that digest to the actual bytes.
- **Writers:** The image and its signature are written together in one operation. A power cut between them gives a mismatch, so the slot fails and the ladder falls to the next rung, which is the safe direction. `purrstrap package-component`, `net_install` and the recovery loader would write the trailer.
- **Policy:** Start in `warn` mode so the current dev flow keeps working, then move to `enforce`. The handoff then reports `VERIFIED` or `FAILED_WARNED`.
- **Why not the alternatives:**
  - A separate `bootsig` partition splits one logical image across two writes that can desync.
  - Loading the ESP image at an offset inside a PURR container depends on an IDF capability nobody has confirmed.

One decision for you: do you want the trailer approach, or the separate partition? I can start on the trailer.
