# 21. Reference: formats, ABIs and limits

Look-up tables. The C headers are the authority for layout (`PurrOS/components/coreos/include/purr_abi.h` and friends);
this page restates them and was cross-checked against `purrstrap/lib/image.py` and the host tests. All integers are
little-endian. Packed structs have no padding.

- [Image container](#image-container)
- [Constants](#constants)
- [Module payload](#module-payload)
- [purrcfg](#purrcfg)
- [Handoff and boot package](#handoff-and-boot-package)
- [Call tables](#call-tables)
- [Versions](#versions)
- [Manifests](#manifests)
- [Account files](#account-files)
- [Result codes](#result-codes)
- [Limits](#limits)

## Image container

173 bytes, then the payload. `purr_image_header_t`; Python format string `<IBHHBBH32s12s12sII32s64s`.

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 4 | `magic` | `0x50555252`, the bytes `52 52 55 50` (ASCII `RRUP`, the word `PURR` little-endian) |
| 4 | 1 | `header_version` | 1 |
| 5 | 2 | `header_size` | 173 today. Lets the header grow. |
| 7 | 2 | `chip_id` | 0 ESP32, 9 ESP32-S3, `0xFFFF` any (apps only) |
| 9 | 1 | `image_type` | 1 PURR OS, 2 KittenOS, 3 system module, 4 app |
| 10 | 1 | `key_id` | which key in the key bag signed it (0 to 31) |
| 11 | 2 | `flags` | low byte = module subtype when `image_type` is 3 |
| 13 | 32 | `name` | NUL padded text |
| 45 | 12 | `version` | `major.minor.patch`, optional `-suffix` |
| 57 | 12 | `min_boot_version` | lowest bootloader version allowed (empty = none; the bootloader's version is 0) |
| 69 | 4 | `payload_offset` | from the start of the image; at least `header_size` |
| 73 | 4 | `payload_size` | bytes |
| 77 | 32 | `payload_sha256` | SHA-256 of the payload |
| 109 | 64 | `signature` | ECDSA P-256 `r || s` over SHA-256 of **bytes 0 to 108** (everything before this field) |

Verification order is in [17](17-keys-and-signing.md#how-a-device-verifies-a-file).

## Constants

| Group | Values |
|-------|--------|
| chip ids | `PURR_CHIP_ESP32` 0, `PURR_CHIP_ESP32S3` 9, `PURR_CHIP_ANY` `0xFFFF` |
| image types | `PURR_IMG_OS` 1, `PURR_IMG_RECOVERY` 2, `PURR_IMG_MODULE` 3, `PURR_IMG_APP` 4 |
| module subtypes | `PURR_MOD_KERNEL` 1, `COREOS` 2, `APPMANAGER` 3, `RUNTIME` 4, `DRIVER` 5, `BOOTPKG` 6, `DEVBUNDLE` 7, `LOADER` 8 |
| roles | `NONE` 0, `BOOT` 1, `SYSTEM` 2, `OWNER` 3, `DEVELOPER` 4, `VENDOR` 5 |
| secure mode | `OFF` 0, `WARN` 1, `ENFORCE` 2 |
| boot targets (unused) | `NORMAL` 0, `KITTENOS` 1, `LOADER` 2 |
| update states | `NONE` 0, `STAGED` 1, `REQUESTED` 2, `MOVING` 3, `UNCONFIRMED` 4, `CONFIRMED` 5, `FAILED` 6 |
| component ids (floors, update target) | `KITTENOS` 1, `KERNEL` 2, `COREOS` 3, `APPMANAGER` 4, `RUNTIME` 5, `DEVBUNDLE` 6, `BOOTPKG` 7, `LOADER` 8 |
| name lengths | name 32, version 12, SHA-256 32, public key 64, signature 64 |

## Module payload

The payload of a module (`image_type 3`, a command module or kernelmod) is a small prefix and a code blob. Built by `purrstrap modules build`
(`pack_payload` in `modules.py`), read by `purr_module_parse_layout`:

| Offset | Field |
|-------:|-------|
| 0 | `entry_offset` u32, into the **code blob** |
| 4 | `data_reloc_count` u32 |
| 8 | `data_reloc_offsets[]` u32 each, offsets into the code blob; relocate against the **writable** base |
| next | `code_reloc_count` u32 |
| next | `code_reloc_offsets[]` u32 each; relocate against the **executable** base |
| next | the code blob: the module linked at base 0, one merged segment (literals, text, rodata, data), `.bss` not stored |

- Every offset is a multiple of 4 and must fit the blob. `PURR_RELOC_MAX` is 65536 per list.
- Applying a relocation is `word += base`, because the module was linked at 0 so the stored word is already the offset.
- A word goes in the **code** list when its value equals the address of a `FUNC` symbol (from `readelf -s`); everything else is **data**.
- The build links twice at `0x0` and `0x100000`; a word that differs by exactly `0x100000` is a relocation, any other difference is an error.
- Compile flags: `-O2 -std=gnu11 -ffreestanding -fno-builtin -mlongcalls -mtext-section-literals -ffunction-sections -fdata-sections -Wall -Wextra -Werror`;
  link: `-nostdlib -Wl,--gc-sections -T <generated.ld> ... -lgcc`.
- Load rule: one 64 KB-aligned PSRAM block per module and kernelmod (the loader function takes a page budget; both callers pass 1).

## purrcfg

`purr_cfg_t`, 752 bytes, in each of two 4 KB sectors. CRC-32 (reflected polynomial `0xEDB88320`, init and final XOR `0xFFFFFFFF`, the same as zlib's `crc32`) over
bytes 0 to 747.

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 4 | `magic` `0x47464350` ("PCFG") |
| 4 | 2 | `version` 1 |
| 6 | 2 | `size` 752 |
| 8 | 4 | `seq` increments on every store |
| 12 | 1 | `secure_mode` |
| 13 | 1 | `boot_target` (unused) |
| 14 | 2 | reserved |
| 16 | 4 | `flags` (bits below) |
| 20 | 4 | `revoked_keys`, one bit per key id |
| 24 | 4 | `boot_seq` (nothing writes it) |
| 28 | 4 | `boot_fail_count` |
| 32 | 4 | `latest_time` (nothing writes it) |
| 36 | 8 | `update`: `target` u8, `state` u8, `attempts` u8, reserved u8, `version` u32 |
| 44 | 64 | `floors[8]`: each `component` u8, 3 reserved, `version` u32 |
| 108 | 544 | `keys[8]`: each `key_id` u8, `role` u8, `valid` u8, reserved u8, `pub` 64 bytes |
| 652 | 32 | `efuse_arm` (unused) |
| 684 | 64 | reserved |
| 748 | 4 | `crc32` |

Flag bits: `IGNORE_ONCE` 0, `UPDATE_KEY` 1, `SECURE_OFF_ONCE` 2, `FORCE_RECOVERY` 3, `FORCE_LOADER` 4, `AUTO_REINSTALL` 5, `CONTINUE_INSTALL` 6. Meaning and status in
[05](05-boot-process-and-flash-layout.md#the-flags). A ready-to-use builder is in [examples/tools/mkcfg.py](examples/tools/mkcfg.py).

## Handoff and boot package

`purr_handoff_t` (16 bytes; **defined but not written or read by anything**, [F-05](FINDINGS.md#f-05)): `magic` u32 (`0x4F464E48`), `version` u8 (1),
`boot_state` u8, `key_id_used` u8 (`0xFF` none), `flags_used` u8, `bootloader_version` u32, `crc32` u32. Boot states: `NONE` 0, `VERIFIED` 1, `FAILED_WARNED` 2,
`FORCED_RECOVERY` 3, `CONFIG_DEFAULT` 4. (The spec also lists `auto_reinstall`; not in the header.)

**Boot package** payload (`purr_pkg_preamble_t`, 16 bytes): `text_size` u32, `data_size` u32, `bss_size` u32, `entry` u32, then `text_size` bytes of code, then
`data_size` bytes of data. Sizes are padded to a multiple of 4. ESP32-S3 windows: code written at `0x3FC88000` and run at `0x40378000` (32 KB max), data and bss at `0x3FC90000`
(32 KB max). Entry:

```c
int purr_pkg_entry(const purr_boot_services_t *svc, const purr_boot_part_t *parts, int nparts, int preferred);
// returns the index of the slot to boot, PURR_PKG_CHOICE_NORMAL (-1) or PURR_PKG_CHOICE_INTERNET_RECOVERY (-2)
```

`purr_boot_services_t` (version 1): `version`, `log(line)`, `delay_us(us)`, `gpio_setup(pin, mode)` (`PURR_GPIO_OUT` 0, `PURR_GPIO_OPEN_DRAIN` 1).
`purr_boot_part_t`: `name[16]`, `bootable` u8, 3 pad. At most 8 parts.

## Call tables

### Core table (modules), `PURR_MODULE_ABI_VERSION` **4**, `purr_module_abi.h`

| Field | Signature |
|-------|-----------|
| `puts` | `void (*)(purr_cli_t *cli, const char *s)` |
| `printf` | `void (*)(purr_cli_t *cli, const char *fmt, ...)` (160-byte buffer) |
| `apps_scan` | `void (*)(purr_app_registry_t *out)` |
| `net_scan` | `int (*)(purr_module_net_ap_t *out, int max, int *out_n, char *err, size_t err_cap)` |
| `net_connect` | `int (*)(const char *ssid, const char *pass, char *err, size_t err_cap)` |
| `net_forget` | `void (*)(const char *ssid)` |
| `net_saved` | `int (*)(purr_module_net_ap_t *out, int max)` |
| `net_status` | `void (*)(purr_module_net_status_t *out)` |
| `net_install` | `int (*)(purr_cli_t *cli, const char *component)` |

A module returns `purr_module_table_t { uint32_t abi_version; const purr_cmd_t *cmds; uint32_t cmd_count; }` from its entry
`const purr_module_table_t *entry(const purr_core_table_t *core)`.

### Kernel table (kernelmods), `PURR_KERNEL_TABLE_ABI_VERSION` **6**, `purr_kernel_table.h`

| Group | Fields |
|-------|--------|
| output | `puts`, `printf` |
| heap | `heap_alloc`, `heap_free`, `heap_free_internal`, `heap_largest_free_internal`, `heap_free_psram`, `heap_total_psram` |
| time | `uptime_us` |
| filesystem | `fs_list`, `fs_read`, `fs_write`, `fs_mkdir`, `fs_remove`, `fs_rename`, `fs_usage`, `fs_mounted`, `fs_strerror`, `fs_format` |
| console | `console_flush`, `console_clear`, `read_key` |
| accounts | `login_whoami`, `login_logout`, `login_su`, `login_passwd`, `login_useradd`, `login_userdel`, `login_usermod`, `login_run`, `login_skip`, `login_take_logout` |
| apps | `app_info`, `app_install`, `app_remove`, `app_format` |
| system | `print_version`, `print_info`, `print_parts`, `print_purrcfg`, `reboot_system` |

Entry: `const purr_kernel_module_table_t *entry(const purr_kernel_table_t *kernel)`. The table's field **order is the ABI**; the version number is bumped on every change.
The standalone `Kernel/` binary fills only: `puts`, `printf`, the heap and uptime entries, `fs_list/read/write/mkdir/remove/rename/usage/mounted/strerror`, `console_flush`,
`console_clear`. Everything else is `NULL` there.

### Shell command type

```c
typedef int (*purr_cmd_fn)(purr_cli_t *cli, int argc, char **argv);
typedef struct { const char *name; const char *help; purr_cmd_fn fn; } purr_cmd_t;
```

### Display

`purr_display_v2_t`, major 2, minor 0: `struct_size`, `major`, `minor`, `features` (bit 0 brightness, bit 1 power), `get_info`, `blit`, `fill`, `set_brightness`, `set_power`.
Pixels are host-endian RGB565. Detail in [13](13-writing-drivers.md).

## Versions

`major.minor.patch`, each 0 to 255, missing parts are 0, an optional suffix starting `-` or `+` is **ignored for ordering**. Packed as `major<<24 | minor<<16 | patch`.
So `1.0.0-dp2` and `1.0.0-dp10` compare **equal**, and an app update from one to the other is refused as `an equal or newer version is already installed`. Anything else
(`v1.0`, `1.x`) is `bad-version`. Used for app updates, the version floor and `min_boot_version`.

## Manifests

Format, keys and limits: [10](10-updates-network-install-and-recovery.md#the-recovery-manifest). The module index uses the same format with `type=module` or `type=kernelmod`.

## Account files

`/etc/passwd` and `/etc/shadow` formats: [07](07-user-accounts.md#where-it-is-stored). Password hash: PBKDF2-HMAC-SHA256, 10,000 iterations, 16-byte salt, 32-byte output.

## Result codes

| Source | Values |
|--------|--------|
| verify | `ok`, `bad-magic`, `bad-version`, `bad-layout`, `bad-chip`, `bad-type`, `too-old-bootloader`, `below-version-floor`, `no-key`, `revoked`, `role-mismatch`, `bad-hash`, `bad-signature`, `io-error` |
| module layout | `ok`, `too short`, `too many` relocations, `bad size` (prefix does not fit), `bad entry` |
| relocation | `ok`, `too many`, `misaligned`, `bad offset` |
| app manager | `ok`, `not a PURR image`, `not an app image`, `no payload for this chip`, `failed verification`, `name too long`, `an equal or newer version is already installed`, `too many apps installed`, `filesystem error` |
| filesystem | `I/O error`, `no valid filesystem (blank or damaged)`, `no such file or directory`, `already exists`, `not a directory`, `is a directory`, `directory not empty`, `bad file`, `file too large`, `invalid, or the filesystem is not mounted`, `no space left`, `out of memory`, `no such attribute`, `name too long` |
| swap actions | nothing, do swap, resume move, retry, rollback |
| module load | loaded, transient (retry next boot), untrusted (quarantine), incompatible (quarantine) |

## Limits

| Thing | Limit | Where |
|-------|-------|-------|
| shell line | 119 characters | `PURR_CLI_LINE` 120 |
| shell words | 16 | `PURR_CLI_MAX_ARGS` |
| `write` text | about 118 characters | `fs_module.c` |
| path length (shell file commands) | 79 characters | 80-byte buffers |
| `printf` in modules | 160 bytes per call | `core_table_printf`, `purr_cli_printf` |
| console grid | 64 columns x 40 rows | `purr_term.h` |
| loaded modules / kernelmods | 8 / 8 | `PURR_MODULE_MAX`, `PURR_KERNELMOD_MAX` |
| entries seen per module folder | 8 (including subfolders) | `module_listing_t` |
| module size | one 64 KB page of code; file read cap 128 KB | `commands.c` |
| relocations per list | 65,536 | `PURR_RELOC_MAX` |
| accounts | 16 | `PURR_USER_MAX` |
| user name | 31 characters | `PURR_USER_NAME_LEN` |
| password | 63 characters (typed), PBKDF2 10,000 iterations | `login.c`, `purr_users.h` |
| uid | 1 to 255 | |
| saved Wi-Fi networks | 8 (SSID 32, password 64, the radio uses 63) | `PURR_WIFI_MAX_SAVED` |
| apps per user | 32; package 512 KB; name 31 characters | `PURR_APP_MAX`, `APP_SCRATCH_CAP` |
| key bag | 16 keys, ids 0 to 31, 8 override slots in `purrcfg` | `PURR_KEYBAG_MAX`, `PURR_KEY_SLOTS` |
| floors | 8 slots | `PURR_FLOOR_SLOTS` |
| manifest | 16 entries kept; manifest 32 KB; system image 2 MB; module file 256 KB | `purr_manifest.h`, `commands.c` |
| manifest fields | component 31, version 15, chip 15, board 23, file 63 characters | `purr_manifest.h` |
| HTTPS | 20 s timeout, 5 redirects, 8 KB header buffer | `purr_fetch.c` |
| Wi-Fi connect | 15 s; reconnect poll every 3 s | `purr_net.c`, `commands.c` |
| boot menu | 3 s countdown; 2 s before "Internet recovery"; 8 entries | `purr_menu.h` |
| boot package | 32 KB code, 32 KB data and bss | `purr_abi.h` |
| boot ladder | kernel at failures 1-3, KittenOS 4-6, loader 7+ | `purr_boot.c` |
| staged update retries | 3 attempts before rollback | `purr_swap_decide(..., 3)` |
| filesystem | 4096-byte blocks, one open file at a time | `purr_fs.h` |
| flash writes | `purrcfg` two 4 KB sectors; `netrec` one record | |

Next: [22 purrstrap reference](22-purrstrap-reference.md).
