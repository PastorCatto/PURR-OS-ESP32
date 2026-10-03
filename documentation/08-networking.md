# 08. Networking

Wi-Fi in station mode (the device joins your network; it never hosts one), plus a small HTTPS
download helper that the installer and app installer use. Source:
`PurrOS/components/kernel/src/purr_net.c`, `purr_netrec.c`, `purr_fetch.c`,
`PurrOS/components/coreos/src/purr_wifi.c`, `Modules/network/wifi_module.c`.
Design: `Network/SPEC.md`.

**Status:** **[WORKS]** scan, connect, save, forget, automatic reconnect, HTTPS GET with redirects.
**[DESIGNED]** and absent: `ping`, `nslookup`, `fetch <url>`, `net` details beyond SSID/RSSI/IP, sockets for apps
(`net` catcall), Bluetooth, a hotspot mode, static addresses, captive portals, enterprise Wi-Fi, IPv6,
power saving, time synchronisation.

## Commands

All from the `wifi` module and its `net` alias ([06](06-using-the-shell.md)).

```
wifi scan
wifi connect <ssid> [password]
wifi forget <ssid>
wifi list
wifi status        (same as: net)
```

### Connect to a network

```
alice@PURR OS> wifi scan
scanning...
  HomeNet                           -48 dBm  secured
  Neighbour                         -80 dBm  secured
  CoffeeShop                        -61 dBm  open
alice@PURR OS> wifi connect HomeNet hunter2hunter2
connecting to HomeNet...
connected, ip 192.168.1.57
```

- Quote names or passwords with spaces: `wifi connect "My Home" "my pass phrase"`.
- No password means an **open** network.
- A password makes the device require WPA2-PSK or better. Use passwords of 8 to 63 characters; longer
  ones are truncated to 63, so a raw 64-hex-digit WPA key will not work.
- The **password is typed in the clear** on the screen and ends up in the shell's screen buffer.
- Wait up to 15 seconds. On failure you get one of:
  - `connection timed out. Could not connect. network not found`
  - `connection timed out. Could not connect. likely a wrong password`
  - `connection timed out. Could not connect.` (reason unknown)
  - `could not connect (<esp error name>).`
- Only 2.4 GHz networks are visible to an ESP32-S3.

A successful `wifi connect` does two things:

1. Adds the network to the **saved list** in `/etc/wifi` (plain text, `ssid<TAB>password` per line).
   Maximum **8** saved networks; if the list is full the network connects but is not saved. Names up to
   32 characters, passwords up to 64, neither may contain a tab or newline.
2. Overwrites the **recovery network record** in the `netrec` partition with the same name and password.
   That record is what the recovery loader (which has no filesystem) and KittenOS's stage-2 installer use to
   get online by themselves ([10](10-updates-network-install-and-recovery.md)). It always holds the **most
   recent** network you connected to by hand, not your whole list.

### Forget a network

`wifi forget HomeNet` removes it from `/etc/wifi` and, if it is the one in use, disconnects and stops
automatic reconnecting. It does **not** clear the `netrec` record, so the recovery loader may still try that
network. To clear `netrec`: `python -m esptool --chip esp32s3 --port COM5 erase_region 0x500000 0x1000`.

### Check status

`net` prints `wifi:    HomeNet (-48 dBm)` and `ip:      192.168.1.57`, or `wifi:    not connected`.

## Automatic reconnect

When the Wi-Fi service starts (right after the filesystem and modules, before login), it loads the saved
list and starts a background task that wakes every 3 seconds:

1. If associated, do nothing.
2. If a connection has worked since boot and then dropped, retry the credentials the radio already holds.
3. Otherwise, if there are saved networks, scan and connect to the **strongest saved network in range**.

So after a reboot with saved networks in range, the device connects on its own within about 3 to 10
seconds, with no command. Until `wifi connect` has succeeded once in this boot, only step 3 applies.
The scan in step 3 is blocking and briefly interrupts the radio.

## Downloads (HTTPS)

`purr_fetch_alloc` and `purr_fetch_into` are a blocking HTTPS GET used by `netinstall` and `appinstall <url>`.

- Certificates are checked with ESP-IDF's bundled certificate authority list.
- Redirects (301, 302, 303, 307, 308) are followed by hand, up to 5. This is what makes GitHub release asset URLs
  work, since they redirect to an object store with a very long URL (the header buffer is 8 KB for this reason).
- 20 second timeout. A non-200 answer fails (`HTTP <code>` in the serial log).
- Size limits are set by the caller: manifests 32 KB, system images 2 MB, module files 256 KB, apps 512 KB.
- There is **no resume**. A dropped connection means starting that download again.
- **No clock is set** anywhere, and the project's own `Keys/SPEC.md` design (authenticated time, a certificate time
  floor) is not built. ESP-IDF's mbedtls does not enforce certificate dates by default, so HTTPS works without
  a clock but also does not check expiry. This is an inference from ESP-IDF defaults, not something this project
  tests.
- The download source is **not trusted for integrity**. Everything downloaded is signature- and hash-verified
  again before it is used ([17](17-keys-and-signing.md)).

## Troubleshooting

| Symptom | Likely cause and fix |
|---------|----------------------|
| `wifi scan` shows nothing | radio busy right after boot; wait a few seconds and retry |
| `network not found` | wrong name (case matters), 5 GHz only network, or out of range |
| `likely a wrong password` | check the password; WPA3-only or enterprise networks are not supported |
| Connects, but `netinstall` says `could not fetch the manifest` | no internet, or the manifest URL is wrong or private. See the URLs in [18](18-making-your-own-purr-os.md). Watch the serial log for `HTTP 404` and similar. |
| Reconnects to the wrong network | it picks the strongest **saved** one; `wifi forget` the unwanted one |
| Wi-Fi gone after a `format --yes` | the saved list lived on `root`. The `netrec` record survives, but only the loader and stage 2 use it. |
| `wifi: did not start (...)` at boot | the radio failed to initialise; see the serial log |

## Security notes

- Saved passwords are plain text in `/etc/wifi` and `netrec`, readable by any logged-in user with `cat /etc/wifi`
  ([F-10](FINDINGS.md#f-10), [F-23](FINDINGS.md#f-23)). The specs say real protection needs flash encryption, which
  is a later step.
- Nothing can reach the device over the network; it makes outbound connections only.

Next: [09 Filesystem](09-filesystem.md).
