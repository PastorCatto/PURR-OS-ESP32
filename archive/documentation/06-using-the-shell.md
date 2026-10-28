# 06. Using the shell

The shell is the only user interface today. It runs on the T-Deck Plus display (a 40 by 26 character
grid, 8x9 pixel cells, white on black) and reads the built-in keyboard. There is no serial shell
(the USB serial port is a log output only).

## Logging in

After boot setup, the screen shows `login:`. See [07](07-user-accounts.md) for first-time setup and
accounts. After a successful login:

```
PURR OS shell
Type help for the commands.

alice@PURR OS>
```

KittenOS shows `KittenOS shell` and a `name@KittenOS>` prompt. In KittenOS, if there is no PURR OS
installed, login is skipped and you are the session user `setup` (an admin).

## Typing

| Key | Does |
|-----|------|
| Enter | run the line |
| Backspace / Delete | delete one character |
| Ctrl-U | clear the whole line |
| any printable key | insert and echo |

The line is limited to 119 characters and 16 words. Longer lines print `line too long`; more than 16
words, or an unclosed quote, print `syntax error: too many words or an open quote`.

Quoting: `'single'` and `"double"` quotes group words. There is no escaping and no difference
between the two. Examples: `write notes.txt "hello there"`, `wifi connect "My Network" "p ass"`.

**What the shell does not have** ([F-04](FINDINGS.md#f-04)): no command history, no tab completion,
no pipes (`|`), no redirection (`>`, `>>`, `<`), no `;`, `&&`, `||`, `&`, no variables (`$X`, `$?`),
no comments, no scripts (`sh file`), no wildcards, no job control, no `$PATH`. The specs describe
all of these ([DESIGNED]); `purr_cli.h` itself says "Pipes come later". Every command's exit code is
returned but nothing displays or uses it.

An unknown word prints `<word>: command not found (try help)`.

## Command table

Commands come from three places. The order in `help` is: built-ins, then `/system` modules
(alphabetical), then `/kernelmods` kernelmods (alphabetical). If a module is missing or was
quarantined, its commands are simply absent. `help` prints `  name     help-text`.

| Source | Commands |
|--------|----------|
| built-in (`commands.c`) | `help`, `plantmodules` |
| `/system/about.cat` | `about` |
| `/system/apps.cat` | `apps` |
| `/system/netinstall.cat` | `netinstall` |
| `/system/wifi.cat` | `wifi`, `net` |
| `/kernelmods/appmgr.cat` | `appinfo`, `appinstall`, `appremove`, `appformat` |
| `/kernelmods/fs.cat` | `ls`, `cat`, `mkdir`, `rm`, `mv`, `write`, `df`, `format` |
| `/kernelmods/login.cat` | `whoami`, `id`, `su`, `passwd`, `useradd`, `userdel`, `usermod`, `logout` |
| `/kernelmods/sysinfo.cat` | `mem`, `uptime`, `version`, `info`, `parts`, `echo`, `clear`, `reboot`, `purrcfg` |

**If `help` shows only `help` and `plantmodules`, the modules are missing.** That happens on a freshly
formatted `root`. Install them with `netinstall modules` (needs Wi-Fi) or `plantmodules` (local), then
`reboot`. See [10](10-updates-network-install-and-recovery.md).

## Reference

### System information

| Command | Output / effect |
|---------|-----------------|
| `version` | `PURR OS 0.1.0 (full profile)`. The number is hard-coded ([F-18](FINDINGS.md#f-18)). |
| `info` | `system`, `board` (`tdeck_plus`), `chip` (`esp32s3 rev 0.2, 2 cores`), `display` (`ST7789 320x240 320x240`), `idf` |
| `mem` | `internal: N free, N largest block` and, on boards with PSRAM, `psram:    N free of N` |
| `uptime` | `up H:MM:SS` |
| `parts` | the partition table: name, type, subtype, offset, size in KB |
| `purrcfg` | `copy`, `seq`, `secure mode`, `flags`, `boot count, fails` ([05](05-boot-process-and-flash-layout.md)) |
| `echo words...` | prints them separated by single spaces |
| `clear` | clears the screen |
| `about` | prints a fixed message proving the `about` module loaded from a file |

### Restarting

| Command | Effect |
|---------|--------|
| `reboot` | plain restart |
| `reboot recovery` | sets `FORCE_RECOVERY`, restarts; the bootloader starts KittenOS once, skipping the menu |
| `reboot loader` | sets `FORCE_LOADER`, restarts; starts the recovery loader once |
| anything else | `usage: reboot [recovery\|loader]` |

These need the **real PURR bootloader** to do anything ([04](04-building-and-flashing.md)). With the
stock bootloader the flag is written and ignored.

### Files ([09](09-filesystem.md))

| Command | Effect |
|---------|--------|
| `ls [path]` | list a directory (default `/`). Directories print as `name/`, files as `name   size`. |
| `cat file` | print a file. Non-printable bytes show as `.`. |
| `mkdir dir` | create one directory (parent must exist) |
| `rm path` | remove a file or an **empty** directory |
| `mv from to` | rename or move |
| `write file text...` | create or **replace** a file with the words joined by spaces plus a newline. Capped at about 118 characters. |
| `df` | `root: NK used of NK (N blocks free)` |
| `format --yes` | **erase everything** on `root` and create an empty filesystem. Without `--yes` it only warns. |

A path without a leading `/` gets one (`etc` means `/etc`). Paths are limited to 79 characters. There
is no current directory and no `cd`.

### Accounts ([07](07-user-accounts.md))

| Command | Effect |
|---------|--------|
| `whoami` | `alice` or `alice (root)` after `su` |
| `id` | `uid=1(alice) role=admin root` |
| `su` | admins only. Asks `password:` (your own) and makes the session root until `logout`. |
| `passwd [user]` | change your own password (asks the current one) or, as an admin, someone else's |
| `useradd name [admin\|standard]` | admins only. Prompts twice for the new password. Default role is standard. |
| `userdel name` | admins only. Cannot delete yourself or the last admin. |
| `usermod name admin\|standard` | admins only. Cannot demote the last admin. |
| `logout` | ends the session and returns to `login:` |

### Network ([08](08-networking.md))

| Command | Effect |
|---------|--------|
| `wifi scan` | list nearby networks: name, dBm, `open` or `secured` (first 16) |
| `wifi connect ssid [password]` | connect (15 second timeout) and save the network |
| `wifi forget ssid` | remove a saved network |
| `wifi list` | list saved networks |
| `wifi status`, `net` | `wifi: <ssid> (<dBm> dBm)` and `ip:`, or `wifi: not connected` |
| `netinstall [component]` | download, verify and install a signed component. **Default is `kernel`.** Components: `kernel`, `loader`, `bootpkg`, `coreos`, `appmanager`, `runtime`, `devbundle`, `modules`. See [10](10-updates-network-install-and-recovery.md). |

### Apps ([11](11-apps.md))

| Command | Effect |
|---------|--------|
| `apps` | list your installed apps: name, version, size, verified/unverified, `(wrong chip)` |
| `appinfo name` | name, version, size, chip match, verified, signer role |
| `appinstall file-or-URL` | install a `.cat` from a file on `root` or an `http(s)://` URL |
| `appremove name` | remove one of your apps |
| `appformat --yes` | remove **all** of your apps (not other users') |

### Temporary

| Command | Effect |
|---------|--------|
| `plantmodules` | writes the eight embedded signed modules to `/system` and `/kernelmods`, then tells you to reboot. A local fallback for when there is no network. It is scaffolding the author intends to remove. |

## Error messages you will meet

| Message | Meaning |
|---------|---------|
| `no filesystem mounted (run: format --yes)` | `root` failed to mount. On PURR OS run `format --yes`; KittenOS formats by itself. |
| `<path>: <reason>` | a LittleFS error, e.g. `no such file or directory`, `file exists`, `directory not empty` |
| `only an admin can do that` | an admin-only account command from a standard user |
| `usage: ...` | wrong arguments |
| `not connected. run: wifi connect <ssid> [password]` | `netinstall` without Wi-Fi |

## Limits worth knowing

All collected in [21](21-reference-formats-abis-limits.md#limits): 16 words, 119 characters, 79-character
paths, 8 modules, 8 kernelmods, 16 accounts, 8 saved networks, 32 apps per user.

Next: [07 User accounts](07-user-accounts.md).
