# 13. Networking

## Short version

- The kernel has **two network backends**, chosen at build time: **Wi-Fi** (the real T-Deck Plus) and **wired W5500 Ethernet** (the VM flavor, which has no radio). Both sit behind `purr_net.h`.
- Saved Wi-Fi networks are kept in `/etc/wifi` and the device **reconnects by itself**. The recovery loader, which has no file system, reads a small raw record instead.
- All downloads go through `purr_fetch` (HTTPS GET). Fetches checked against a size and a SHA-256 are retried three times.
- The **dev server** (`purrstrap dev serve`, port 47801) is found by a UDP broadcast on port 47800. In the VM it is at `http://10.0.2.2:47801`.
- Spec: [Network/SPEC.md](../Network/SPEC.md). Several things it lists (`fetch`, `ping`, `nslookup`, a `net` catcall) are **not in the code**; see the end.

## What it is for

The network does four jobs: it lets the recovery loader download the OS, lets the running system install updates, lets the Developer Portal and Store install apps, and sets the clock. Apps are meant to use sockets and never manage the connection ([Network/SPEC.md](../Network/SPEC.md) section 1).

## The files

| File | What it is |
|---|---|
| [Network/SPEC.md](../Network/SPEC.md) | The networking design (draft 0.1) with the wired section 5b |
| [PurrOS/components/kernel/include/purr_net.h](../PurrOS/components/kernel/include/purr_net.h) | The one API for both backends |
| [purr_net_wifi.c](../PurrOS/components/kernel/src/purr_net_wifi.c) | Wi-Fi station mode, saved networks, reconnect |
| [purr_net_eth.c](../PurrOS/components/kernel/src/purr_net_eth.c) | W5500 over SPI, polled every 10 ms |
| [purr_netrec.h](../PurrOS/components/kernel/include/purr_netrec.h), [purr_netrec.c](../PurrOS/components/kernel/src/purr_netrec.c) | The recovery network record and the saved recovery source |
| [purr_fetch.h](../PurrOS/components/kernel/include/purr_fetch.h), [purr_fetch.c](../PurrOS/components/kernel/src/purr_fetch.c) | Blocking HTTPS GET |
| [purr_wifi.h](../PurrOS/components/coreos/include/purr_wifi.h), [purr_wifi.c](../PurrOS/components/coreos/src/purr_wifi.c) | The saved-network list logic (host-tested) |
| [purr_lan_discover.c](../PurrOS/main/purr_lan_discover.c) | Finds SDK servers by UDP |
| [purr_recovery_src.c](../PurrOS/main/purr_recovery_src.c) | Where recovery pulls images: official or a chosen dev server |
| [purr_repo.h](../PurrOS/components/coreos/include/purr_repo.h) | The rules for which addresses may be contacted |
| [purr_repo_dev.c](../PurrOS/main/purr_repo_dev.c) | The device's client for a dev server's app index |
| [purr_netsettle.c](../PurrOS/main/purr_netsettle.c) | Lets deferred devices (the LoRa radio) bind after Wi-Fi has an address |
| [purr_clock.c](../PurrOS/main/purr_clock.c) | SNTP and the saved time |
| [Modules/network/wifi_module.c](../Modules/network/wifi_module.c) | The `wifi` and `net` shell commands, as a loadable module |
| [Modules/netinstall/netinstall_module.c](../Modules/netinstall/netinstall_module.c) | `netinstall <component>` |
| [sdkconfig.flavor.vm](../PurrOS/sdkconfig.flavor.vm) | Selects the wired backend |
| [tools/broken-kitten-recovery/](../tools/broken-kitten-recovery/README.md) | A computer-side relay that serves recovery files |
| [purrstrap/scripts/dev.py](../purrstrap/scripts/dev.py) | The dev server and its announce loop |

## How it works

### The two backends

| | Wi-Fi | Wired (VM flavor) |
|---|---|---|
| Kconfig | `PURR_NET_WIFI` (default) | `PURR_NET_ETH_W5500` |
| Selected by | the default | `sdkconfig.flavor.<name>` passed by purrstrap |
| Credentials | SSID and password | none; a link and a DHCP address |
| Scan | real | finds nothing |
| Status SSID | the network | `ethernet` |

`purr_net_wired()` tells the rest of the OS which one it has, so the Wi-Fi page of the recovery loader is skipped and `wifi` commands report "no radio" on a wired build. Everything above the link (DHCP, DNS, HTTPS, the dev server) is the same code, so what is tested over the wire runs the same over Wi-Fi.

### The VM's network

The VM flavor drives a W5500 on its own SPI bus through ESP-IDF's Ethernet driver in MACRAW mode, with no interrupt line, polled every 10 ms. The emulator uses QEMU's user-mode network, so the guest is 10.0.2.15 and the host is 10.0.2.2. Discovery maps the loopback address a server reports to the VM host. Details of the VM are in [chapter 18](18-emulator-and-vm.md).

### Wi-Fi, saved networks and reconnect

`purr_net_init(fs)` starts the radio in station mode and loads saved networks from `/etc/wifi`. If the file system is missing, connecting still works for the session. The service reconnects when the link drops. A network that worked is also written to the **recovery record** (`purr_netrec_save`), a small raw record in its own partition, because the loader has no file system. Both store the password as plain text. The spec says real protection needs flash encryption, which is a later step. Do not copy these files off a device.

### Fetching

- `purr_fetch_alloc(url, &buf, &len, max)` GETs into PSRAM (where the board has any). The caller frees it.
- `purr_fetch_alloc_checked` is used for files an index described: up to three attempts, waiting 2 s then 4 s, each checked against the expected size and SHA-256.
- A response whose stated length ended short is a failed attempt everywhere.
- `purr_fetch_local_into` is for local repositories: a short timeout, and a redirect is a **failure**, so a local server cannot send the device to a public host.
- `purr_fetch_last_error()` says which file failed and why.

### Which addresses may be contacted

[purr_repo.h](../PurrOS/components/coreos/include/purr_repo.h) sets a policy per repository kind:

| Kind | Where | Packages | Index |
|---|---|---|---|
| SDK | private, loopback or link-local addresses only; http or https | signed or unsigned (unsigned only in a user's devapps folder, developer mode on) | not signed |
| Store | https only, any public host | signed only | signed (`index.json.sig`) |

A typed address is resolved and checked, then used as a numeric address, so the address that was checked is the one that is used. See [chapter 14](14-store-and-repositories.md).

### Finding the dev server

1. The server (`purrstrap dev serve`) listens on UDP 47800 and answers `PURR-REPO?` with `PURR-REPO! <name> <port> <id>`. It also announces that line every few seconds to the limited broadcast and each interface's broadcast address.
2. The device (`purr_lan_discover`) sends a probe to the limited broadcast, the subnet broadcast and the VM host, and also listens for announcements. Two ways at once, so it works behind firewalls that drop probes and inside the VM.
3. Only a local address may answer.
4. The recovery setup page lets the owner pick a server, saved with `purr_netrec_save_source` so the next boot pulls from the same place.

A dev server is used for recovery only if the bootloader's `DEV_RECOVERY` flag is set **and** developer mode is on. Otherwise recovery uses the official server, whatever was saved. This is a known trap: after a loader install, a device in the safe-mode state (developer mode off) pulled from the official release ([plan3.md](../plan3.md)). Either way every image is checked against the boot key before it is written.

`tools/broken-kitten-recovery` is a program on a computer that fetches, checks and serves the recovery files, announcing itself as `PURR-REPO! <name> <port> relay-<id>`.

### Time

NTP sets the clock (`pool.ntp.org` by default, settings `time.ntp` and `time.ntp_server`). By design plain NTP never raises the certificate floor; only authenticated time (an HTTPS `Date` header) may ([Network/SPEC.md](../Network/SPEC.md) section 1, [Keys/SPEC.md](../Keys/SPEC.md)).

## How to build and test it

1. Host tests (no device): `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py wifi repo`. These cover the saved-network list and the repository policy.
2. Start the dev server: `python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801`.
3. Find it from the computer: `python3 purrstrap/purrstrap.py dev find`.
4. In the VM the server is reached at `http://10.0.2.2:47801`. The VM itself is [chapter 18](18-emulator-and-vm.md).
5. On a device with a server set, `netinstall <component>` downloads and installs one component (for example `netinstall kernel`; the picture packs are the components `ui_w10m` and `ui_android` from 1.0.29, which replace `netinstall uiassets` and are written as files in `root`).
6. After publishing, fetch every file a manifest names and compare size and SHA-256 (CLAUDE.md).

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| Stage 2 uses the official server, not the dev server | Developer mode is off after a restart, so `purr_recovery_dev_allowed()` is false | Turn developer mode on; see [plan3.md](../plan3.md) |
| Stage 2 never fetches modules or the pack | A discovered dev server is not remembered across the restart (unconfirmed) | Use the full image or `netinstall` with the server set ([plan7.md](../plan7.md)) |
| "not enough free memory" on a big download | The largest PSRAM block is too small | Close apps or restart |
| `fetch` fails with a size or CRC error | The bytes arrived but were not the file | Retried three times; check the server's files |
| LoRa and Wi-Fi start together and run out of fast RAM | Both need internal RAM at boot | `purr_netsettle` delays the LoRa radio until Wi-Fi has an address (or a fixed time) |
| A VM touch panic in the I2C interrupt on core 0 while the W5500 polls | Open, likely a firmware bus shutdown; the W5500 task only shares the core | [plan7.md](../plan7.md) |

## What is not finished

- **Plan and code disagree:** Network/SPEC.md names shell commands `fetch <url>`, `ping`, and `nslookup`. I found no such commands in `PurrOS/main/commands.c` or the modules; the commands that exist are `wifi`, `net` and `netinstall`.
- **The `net` and `wifi` catcalls** are in [Catcalls/SPEC.md](../Catcalls/SPEC.md) but `purr_catcalls.h` has no table for them, so apps have no sockets yet. Whether apps get a TLS client is an open question in the spec.
- Open in the spec: certificate bundle updates, provisioning without a keyboard, captive portals, enterprise Wi-Fi, static addresses, a hotspot mode, power saving, IPv6.
- Bluetooth is deferred.
- Wired mode was verified in the VM on 2026-10-27 (link, DHCP, HTTPS downloads, a full internet recovery). A real W5500 module and Wi-Fi roaming or power saving were **not verified on a device**.
- The 4 MB-board design (KittenOS without a network stack, updates via SD card) is a spec idea; no such board builds today ([chapter 19](19-boards.md)).
- The relay and the discovery handshake are described in the code and the README; the relay path was not exercised for this chapter.
- The official repository address is a placeholder until JumpingJaguar3 is published ([plan8.md](../plan8.md)).

Next: [14 The Store and repositories](14-store-and-repositories.md).
