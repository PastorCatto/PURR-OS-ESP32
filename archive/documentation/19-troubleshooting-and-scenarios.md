# 19. Troubleshooting and scenarios

Organised by what you see. Messages are quoted from the source. The **serial log** (115200 baud, USB) is your best instrument: almost every failure
below prints a line there even when the screen shows nothing. Items that depend on the unbuilt parts say so.

- [First: which bootloader is running?](#first-which-bootloader-is-running)
- [Boot problems](#boot-problems)
- [Login and accounts](#login-and-accounts)
- [Shell and modules](#shell-and-modules)
- [Storage](#storage)
- [Wi-Fi and installs](#wi-fi-and-installs)
- [Keys and signatures](#keys-and-signatures)
- [Build and flash problems](#build-and-flash-problems)
- [Recovery scenarios](#recovery-scenarios)

## First: which bootloader is running?

The serial log must contain `purr_boot: PURR OS bootloader` early. If it does not, you are running the **stock** ESP-IDF bootloader, which came from flashing a `PurrOS` build with
`idf.py flash` or its `flash_args`. Then there is no boot menu, `reboot recovery` and `reboot loader` do nothing, and the failure ladder does not exist. Fix: build `bootloader/` and flash only
`bootloader.bin` at `0x0` ([04](04-building-and-flashing.md)). This is the most common cause of "recovery is broken".

## Boot problems

| Symptom | Cause | Fix |
|---------|-------|-----|
| No `PURR OS bootloader` line | stock bootloader | see above |
| `failed to load partition table` | the table at `0x8000` is missing or damaged | flash `partition-table.bin` at `0x8000` |
| `no bootable app found`, then it resets forever | none of `kernel`, `kittenos`, `loader` starts with `0xE9` | flash an app into one of them by wire ([04](04-building-and-flashing.md)). There is no on-screen message ([F-28](FINDINGS.md#f-28)). |
| Boots, no boot menu, screen goes straight to the system | boot package rejected or missing | read the `bootpkg:` serial line, below |
| `bootpkg: no "bootpkg" partition` | partition table lacks it | check the CSV |
| `bootpkg: no PURR image (partition empty?)` | nothing flashed at `0x12000`, or it was installed as a bare payload ([F-13](FINDINGS.md#f-13)) | flash the signed `bootpkg.bin` at `0x12000` |
| `bootpkg: rejected (<reason>), secure mode is warn` | unsigned or wrongly signed package. `no-key`: wrong `--key-id`. `bad-signature`: signed with another key, or the bootloader embeds old keys. | sign with `--key-id 1` using the key whose public half is in `purr_default_keys.c`; rebuild the bootloader after any key change |
| `bootpkg: not a boot package` | a valid image of another subtype is in the partition | reflash |
| `bootpkg: does not fit its RAM window` / `entry outside the code` / `payload smaller than its own preamble` | broken package build | rebuild with `purrstrap bootpkg build` |
| `bootpkg: no RAM window defined for this chip yet` | an ESP32 (not S3) bootloader | the boot package is S3 only ([F-21](FINDINGS.md#f-21)) |
| Menu shows `keyboard not answering` | keyboard I2C not responding; power rail off, wrong pins | check the board profile and `PIN_KBD_*` |
| Menu countdown boots something you did not want | the preferred slot is the first bootable in the ladder order | hold any key during the countdown, W and S to move, D or Enter to choose |
| A blank screen but the log shows the system running | backlight, power rail or driver | [14](14-adding-a-display.md) bring-up checklist |
| `display did not come up: <error>` (then the log repeats every 2 s) | the kernel could not start the panel | check profile pins and the SPI bus; the system will not continue without a display |
| `ST7789 ... up` but garbage | `madctl`, `bgr`, `invert`, offsets, or SPI speed | [14](14-adding-a-display.md) |
| `purr_boot: boot_fail_count N: preferring KittenOS` (or the recovery loader) | the ladder is escalating because boots never reached healthy | find what keeps failing (the serial log of the failing boot); it resets after one healthy boot |
| Boots into an idle screen printing `PURR kernel / hardware up / root fs mounted` and a directory listing | the standalone `Kernel/` binary is in `kernel` | hold a key at the menu and pick KittenOS, or `esptool erase_region 0x1a0000 0x1e0000` |
| Boots straight into KittenOS every time | a failure counter above 3, or the `kernel` slot is blank (the normal dev layout) | check `purrcfg`, `parts`, the layout in [04](04-building-and-flashing.md) |
| Reboot loop with a crash backtrace | see the backtrace. `StoreProhibited` right after mounting root in a new project: the stack-size setting `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` is missing (ECDSA needs real stack). | copy it into your sdkconfig defaults |

Quick state check from a shell: `parts`, `purrcfg` (look at `fails`), `info`, `version`.

## Login and accounts

| Symptom | Fix |
|---------|-----|
| `login incorrect` for a correct password | a name is case-sensitive; `root` is always refused |
| `waiting N second(s)...` | the growing delay after wrong passwords (1, 2, 4 ... 60 s). Wait. It resets on success. |
| Locked out, only account is called `root` | create nothing; wipe `root` ([07](07-user-accounts.md)). Avoid the name. ([F-11](FINDINGS.md#f-11)) |
| Forgot the only admin password | [07](07-user-accounts.md#forgotten-password): erase `root`; everything on it is lost |
| `only an admin can do that` | the command is admin only; `su` or ask an admin |
| `useradd: too many accounts` | the limit is 16 |
| `could not save` | `root` is full or damaged; `df` |

## Shell and modules

| Symptom | Fix |
|---------|-----|
| `help` shows only `help` and `plantmodules` | `/system` and `/kernelmods` are empty or all rejected. `netinstall modules` (needs Wi-Fi) or `plantmodules`, then `reboot`. |
| `<cmd>: command not found (try help)` | the module that provides it did not load; check the boot screen and serial log (`modules`, `kernelmods` tags) |
| `system: /system/x.cat rejected (failed verification) -- quarantined` | the file does not verify with this build's keys. It is now in `/system/.rejected/`. Rebuild and re-sign it with the right key id ([12](12-writing-modules.md)). |
| `... incompatible (abi mismatch) -- quarantined` | the table version changed. Rebuild and re-sign. After an ABI bump, **flashing the app does not replace what is on `root`**: reinstall all modules. |
| A kernelmod is missing and there is no message on screen | kernelmods are skipped silently; read the serial log |
| `help` lists the same command twice | a name clash; the first wins |
| A module loads but crashes when run | it called an unset table entry (the standalone `Kernel/` leaves most `NULL`), or it is stack-heavy (it runs on the shell's stack) |
| Module changes do not show up | you replaced the file but did not `reboot`; modules load once at boot |

## Storage

| Symptom | Fix |
|---------|-----|
| `no filesystem mounted (run: format --yes)` | PURR OS: `format --yes` (erases everything on root). KittenOS does it by itself. |
| `root: not mounted (no valid filesystem (blank or damaged))` | a fresh or erased `root`. `format --yes`. |
| `no space left` | `df`; remove files or apps; a full disk also blocks `passwd` and `useradd` |
| `no such file or directory` creating a nested folder | `mkdir` makes one level; create the parent first |
| `directory not empty` | `rm` needs an empty folder; delete the files first |
| Everything vanished after a reboot of KittenOS | **it formats `root` when it fails to mount** ([F-37](FINDINGS.md#f-37)); a transient error is enough. Keep a copy of anything important off the device (there is no export yet). |

## Wi-Fi and installs

Wi-Fi: see the table in [08](08-networking.md). Install messages, from `netinstall`:

| Message | Meaning |
|---------|---------|
| `not connected. run: wifi connect <ssid> [password]` | connect first |
| `could not fetch the manifest` | wrong URL, no internet, or HTTPS failure. Open the URL from a PC. The serial log has `fetch: <url>: HTTP <code>` or an esp error. |
| `no <component> entry for this board in the manifest` | the manifest has no stanza for this component, `esp32s3` and your board name; check the stanza fields |
| `download failed` | network, or the file is bigger than the buffer (2 MB images, 256 KB modules) |
| `download does not match the manifest (size or hash)` | the manifest's `size` or `sha256` is not for the file you uploaded. Regenerate it ([18](18-making-your-own-purr-os.md)). |
| `not a <component> image` | the container's type or subtype is wrong; use `coreos package-component` for `kernel` and `loader` |
| `verification failed: <name>` | see [17](17-keys-and-signing.md) result names |
| `the image is bigger than the <label> partition` | shrink the image or enlarge the slot |
| `write failed` / `read-back hash mismatch: the write did not take` | flash problem; try again; **that partition may now be broken**, and the bootloader falls back to the next slot |
| `<name> <ver> installed. Run: reboot` | success for a partition component |
| `<name> <ver> staged. Run: reboot` | success for a file-staged component; KittenOS applies it on the next boot |
| `modules: N installed, N skipped (chip/board), N failed` | the summary; failures list above it |

The loader's own messages: `no saved network: cannot recover unattended` (auto mode with an empty `netrec`), `could not connect to the saved network`, `automatic recovery failed`, `no kittenos entry for
this board in the manifest`, `not a KittenOS image`. Each ends in `halted. Power-cycle to try again, or use the boot menu.`

## Keys and signatures

| Message | Cause |
|---------|-------|
| `no-key` | the header's key id is not in the device's key bag: wrong `--key-id`, or the device was built with other keys |
| `role-mismatch` | the key's role may not sign this kind of file ([17](17-keys-and-signing.md)) |
| `bad-chip` | built for the other chip |
| `bad-hash` | the file was modified after signing, or truncated in transfer |
| `bad-signature` | signed by a different key than the id claims |
| `below-version-floor` | enforce mode and an older version than the confirmed floor |
| `could not load <key>: ...` (tool) | wrong password |
| `no PURRSTRAP_KEY_PASSWORD and no terminal to ask on` | set the variable when running non-interactively |

## Build and flash problems

| Symptom | Fix |
|---------|-----|
| Build "succeeds" in Git Bash but there is no output | the MSYS trap. Use PowerShell, or purrstrap (it strips the variables). |
| `ESP-IDF not found` | install ESP-IDF 5.3.5; run `coreos check` ([03](03-development-environment.md)) |
| `fatal error: opening dependency file ...: No such file or directory` | the build path is too long on Windows. Move the repo near a drive root ([F-29](FINDINGS.md#f-29)). |
| `xtensa-esp32s3-elf tools not found` | activate ESP-IDF or install its tools |
| A URL or option change had no effect | the old `sdkconfig` in `build/<board>-<profile>/` was reused. Rebuild with `--clean`. |
| `internal compiler error: Segmentation fault` in an ESP-IDF file | a transient toolchain crash (seen once, building `esp_lcd_panel_rgb.c`); run the same build again, it continues where it stopped |
| `cryptography` import error from `keys` | `pip install cryptography` |
| `compile failed` in a module | any warning is an error ([12](12-writing-modules.md)) |
| `idf.py flash` overwrote the bootloader | use `esptool write_flash` on the app only |
| esptool cannot connect | put the board in download mode; close any serial monitor on the port; try another cable |
| A failed flash left the board dead | hold the download-mode button and reflash all five images ([04](04-building-and-flashing.md)); the ROM bootloader cannot be damaged |

## Recovery scenarios

Which state are you in, and what is the shortest way back?

| You have | Do |
|----------|-----|
| A working shell, a broken module | `rm /system/x.cat` (or fix it), `reboot` |
| A working shell, no modules | `netinstall modules` or `plantmodules`, `reboot` |
| PURR OS will not boot, KittenOS boots | KittenOS: `wifi connect ...`, `netinstall kernel`, `reboot`. Or flash a build by wire. |
| Neither boots, the loader exists | it starts by itself at 7 failed boots, or on `Internet recovery` in the menu when nothing is bootable. It recovers KittenOS; press **F** for the full restore. |
| Only the bootloader and partition table | flash the loader by wire; it installs KittenOS over Wi-Fi; stage 2 adds `kernel` and modules ([10](10-updates-network-install-and-recovery.md)) |
| Nothing at all works (no serial output) | hold the download button, `erase_flash`, then [04](04-building-and-flashing.md) from the top |
| The boot menu is gone | flash a signed `bootpkg.bin` at `0x12000` |
| Wrong keys everywhere after a rotation | rebuild the bootloader and PurrOS with the new keys, re-sign everything, reflash ([17](17-keys-and-signing.md)) |
| Settings corrupted | `esptool erase_region 0x1e000 0x2000` resets `purrcfg` to defaults |
| Wi-Fi record stuck on the wrong network | `esptool erase_region 0x500000 0x1000` clears `netrec` |

## When you must report a problem

Collect: the serial log from power-on through the failure, the output of `parts`, `purrcfg`, `info` and `version`, the exact commands you ran, whether the serial log shows
`PURR OS bootloader`, and the commit you built from (`git log -1`).

Next: [20 Testing](20-testing.md).
