# Notes: building a whole system from source (what happened, 2026-11-09)

A running log written while the feature was built, so the next person (or the next Claude) starts from "last time this is what happened".
Plan entries are in `plan5.md` ("Compile from source in build-a-system", "Compile everything, and whose keys the image trusts"). The user-facing description is in `sdk/docs/16-device-image.md`.

## What was asked, in order
1. A button in the SDK to **pull the latest source and compile against it**, instead of packing pre-compiled parts into the image.
2. The Android Froyo screen is part of the kernel: pulling must bring it (it does: `UI/android-assets` is packed into the kernel by CMake when `sdkconfig.shell.android` is on).
3. **Everything compiled**: firmware, drivers, system modules, apps. The person should be able to treat our code as untrusted, vet the source, and build an image that trusts only their own keys.
4. **Keep our keys by default**, with a toggle to remove them, so someone who builds their own image can still take our OTAs (toggle on) or trust only themselves (toggle off).

## The road (purrsdk/source.py + purrsdk/system.py)
`system.build()` in source mode: pull -> compiler -> trust -> compile 3 profiles -> package 5 parts -> sign (boot key) -> compile modules/drivers/apps (developer key) -> UI -> check parts -> image -> delivery.
- **pull:** git partial clone (`--filter=blob:none --sparse`, then `sparse-checkout set PATHS`), tarball fallback for no git. The repo is huge (`archive/` has ~10k files); the pulled tree is ~6 MB. The tree is replaced as a whole; `PurrOS/build` is moved across so the next compile is incremental.
- **compiler:** `coreos.find_idf()` (it did not know `~/esp/esp-idf-v5.3.5`; fixed). First cold compile of the three profiles took ~7 minutes on this Mac (ccache not needed).
- **build the pulled tree with our purrstrap:** `PURR_REPO_ROOT=<tree> purrstrap coreos build --board B --profile P --shell android`. `PURR_REPO_ROOT` (new, `purrstrap/lib/app.py`) makes purrstrap treat another folder as the repo; `--shell` (new, `coreos.py`) adds the `sdkconfig.shell.android` overlay and a `-android` build folder.
- **package:** `coreos package-component --component kernel|loader|bootloader`, `coreos package --profile recovery` (KittenOS), `bootpkg build`, then `keys sign` for each. Version string is `src-<7 letters of commit>` (12 chars max in the header).
- **modules/drivers/apps:** `modules build --kind driver` (all eight system modules are *driver-kind*, checked in the published headers: type 3 module, flags 5), `--kind app` for apps. Entry symbols: modules `<name>_module_entry`; drivers `<stem>_entry` (file `Modules/drivers/<stem>_module.c`); apps `<name>_entry` from `Apps/<name>/<name>.c`, version from `manifest.toml`.

## Trust: where the keys live
- One list, `PurrOS/components/coreos/keys/purr_default_keys.c`, compiled into the bootloader (`purr_bootcfg.c`, `purr_bootpkg.c`, `purr_slotverify.c` via `purr_bootloader_component.cmake`), the kernel (`commands.c`) and everything that verifies. Roles: boot signs everything below the OS, developer signs modules/drivers/apps. Ids 0..31 (32-bit revocation mask). PURR's own are 1-5.
- `source.apply_trust()` rebuilds that file from `keys export-public` into a scratch folder: PURR's entries re-created from `purr_default_keys.c.ours` (a copy of the file as pulled, kept so the toggle can flip either way), plus the user's `boot` and `developer` `.pub` files. Toggle off = only the user's.
- Rules checked up front (`trust_problems`): both keys chosen, different ids, ids 0..31, and while PURR's keys are kept the user's ids must not be 1-5. (My first real run signed with id 4 = PURR's developer id, which could never verify against the user's key; that is why the check exists.)
- Public key = the `.pub` next to the `.key`.

## Things that went wrong, and what they taught
- **`coreos package` read the wrong build folder.** It resolves `PurrOS/build/...` from `ctx.repo_root`; the first real run packaged nothing for KittenOS because I forgot `PURR_REPO_ROOT` on that one call. Every purrstrap call that reads the tree needs the env. (Test added.)
- **GitHub `main` lacks `sdkconfig.shell.android`** (that work is on PURR2.0). The build stops with the file named. Use `source_ref = PURR2.0` or merge. A local `file://` repo works as a source too (committed state only: uncommitted changes are not in the pull).
- **The kit's purrstrap is older than the repo's.** A scratch kit built earlier has no `--shell` and no `PURR_REPO_ROOT`; run the real compile with `PURR_PURRSTRAP=<repo>/purrstrap` and the kit's Python (macOS's own python3 is 3.9: no `tomllib`, no `cryptography`).
- **Token regex in my render scripts** (`[A-Za-z0-9]*` cut the Studio token at a dash): use `[A-Za-z0-9_-]*`.
- **zsh/background traps:** `pgrep -f` matches the waiting command itself; never wait with it. Use `until grep -q RESULT log` in a background command and wait for the notification.
- **Display did not come up** (the big one, see next section): with PURR's keys removed the image booted the bootloader (boot package verified, "signed by key 7") but the kernel/loader could not bring up the display.

- **`bad-version` on every module and driver** (found on the second keys-out boot, after the baked-in binaries had been rebuilt and the screen *still* did not come up). The serial log said `modules: /system/about.cat: bad-version` for all of them, then `display did not come up`. Cause: I had given parts the version `src-<commit>`; the device parses `major[.minor[.patch]]` (`purr_version_parse`, optionally followed by `-` or `+` and a suffix) and rejects anything else *before* it looks at a signature. The bootloader said the same for the kernel slot (`slot @0x1e0000: bad-version`) and only booted it because `secure_mode` was `warn`. Lesson: **read the serial log for the first rejection, do not guess from the last error line**; and every version string in the build must parse. Now the system has a numeric `version` (default 1.1.0, checked up front by `version_problem`) and parts carry `1.1.0+<commit letters>` (the device compares the numbers, the letters say which source; 12 characters fit in a header). Mind the anti-rollback floor: a version below a device's floor is rejected, so keep the number at or above what the device already runs.

## Baked-in binaries (why "everything compiled" needs one more step)
`PurrOS/main/commands.c` contains **precompiled, signed binaries as byte arrays**, signed by PURR's developer key: `s_about_module`, `s_apps_module`, `s_wifi_module`, `s_netinstall_module`, `s_sysinfo_module`, `s_fs_module`, `s_login_module`, `s_appmgr_module`, `s_debug_module` (system modules/kernelmods), `s_kbd_i2c_driver_module`, `s_gt911_driver_module`, `s_st7789_driver_module`, `s_battery_driver_module`, `s_kbd_backlight_driver_module` (drivers), and two test blobs (`s_hello_app_package`, `s_hello_driver_module`). The recovery loader loads its display/keyboard/touch drivers from those arrays (`purr_loader_load_drivers`), the kernel plants them into `/boot/drivers`. With PURR's developer key out of the key list none of them verifies, so the display never starts ("display did not come up", every 2 s, in the serial log).
`PurrOS/main/purr_builtin_apps.c` holds the Store and its companion (unsigned, trusted through the kernel hash; generated by `tools/release/gen_builtin_apps.py`).
So a build that trusts nobody's binaries must **regenerate these from source** before compiling the kernel and loader. `source.rebake()` does it, after `apply_trust` and before `compile_and_package`:
- keeps `commands.c.orig` (as pulled) and reads every array from it, so rebuilding twice gives the same file;
- for each of the 14 arrays (`source.BAKED`: array -> source file and entry symbol) it takes the **name and kind (header flags) from the binary that was there**, compiles with `modules build`, signs with the developer key and id, and rewrites the array in `commands.c` (12 bytes a line, same layout);
- builds `Apps/store` and `Apps/storesvc` (`--kind app`, unsigned) and runs `tools/release/gen_builtin_apps.py` inside the tree (its `ROOT` comes from its own location, so it writes the tree's `purr_builtin_apps.c`). That is why `tools/release` is in the sparse set (nested paths work in `sparse-checkout set`; the tarball filter was changed to match prefixes);
- leaves `s_hello_app_package` and `s_hello_driver_module` (test blobs for `testhello`-style commands, trusted by nothing) as they were.
The pulled tree is only refreshed when the commit changes, so after adding a path to `PATHS` delete `<workspace>/source/source.json` to force a new pull.

## Verified for real so far (first run) (scratch workspace, throwaway keys; nothing of the user's)
- Keep-ours build, 1.1-era source: compiled 3 profiles, 5 parts packaged and signed, image 16 MB, virtual device boots to the recovery loader.
- Keys-out build, third attempt (after rebake and the numeric version): all steps pass (14 baked binaries + 2 Store apps, firmware, 8 modules, 8 drivers); kernel and a module verify with the user's keys and fail with PURR's; the bootloader verifies the boot package "signed by key 7" and the kernel slot (`slot @0x1e0000: ok`); the virtual device reaches the recovery loader with the screen drawn. The first two keys-out attempts booted but showed no display (baked-in binaries signed by PURR; then `bad-version`).
- Not verified: apps (own projects or the source's `Apps/`) through the whole road (fake-runner tests only, one app is compiled by rebake: store/storesvc); toggle on with a real OTA from PURR; real hardware; Windows.

## For next time
- Start with `sdk/tests/test_source.py` (fake runner) to see the shape; the real run is `scratchpad/realtrust.py` style: create a throwaway boot key and developer key (`purrstrap keys generate --role boot|developer --key-id N --out DIR`, password in the environment of that one command), set `parts_from=source`, `keep_ours`, run `system.build`, then `emulator.test_image()`.
- A full cold run is ~10 minutes of compile plus the emulator's ~100 s.
