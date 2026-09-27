# Networking spec (draft 0.1)

How the device connects to networks. The `net` and `wifi` catcalls apps use are in
`../Catcalls/SPEC.md` sections 1 and 5. Updates over the network are in `../OTA/SPEC.md`.
Bluetooth is deferred to its own chunk.

## 1. Decisions so far

- **A system service remembers Wi-Fi networks and reconnects on its own,** like a phone, at
  boot and whenever the connection drops. Apps use sockets and never manage the connection.
  Only an app with the `wifi` permission can scan, add or remove networks.
- **Saved passwords are kept in a root-only file** (`/etc/wifi`). Apps never see them, and can
  only ask the service to connect to a saved network. Real protection against someone reading
  the flash needs flash encryption, which is a later step.
- **Time.** Plain NTP sets the clock but never the certificate floor. Only **authenticated
  time** (the `Date` header of an HTTPS response) may raise `latest_time`, so nobody on the
  network can expire certificates by feeding a far-future time (`../Keys/SPEC.md` section 5).
- **Shell commands:** `wifi` (scan, connect, forget, list saved networks), `net` (connection,
  IP address, signal strength), `ping`, `fetch <url>` (HTTP and HTTPS download, like `wget`),
  and `nslookup`.
- **KittenOS and the network**, by board tier:
  - **Modular boards:** KittenOS carries a Wi-Fi stack with HTTPS, so recovery can download a
    missing or broken system file itself.
  - **4 MB boards (monolithic):** KittenOS has **no network stack**. The running system, which
    keeps its Wi-Fi, downloads an update to the SD card, and KittenOS applies it from there.
    Such a board needs an SD card for a Wi-Fi update.
- **Bluetooth is deferred.**

## 2. Who does what

Following the layers (`../PurrOS/SPEC.md` section 2):

- **The kernel** owns the hardware side: the Wi-Fi radio driver and the IP stack.
- **CoreOS** hosts the connection service: the list of saved networks, reconnecting, and the
  policy. *(Proposed.)*
- **The catcalls** `net` and `wifi` sit on top. They exist only on boards with Wi-Fi hardware.

## 3. First-time setup

A board with no keyboard, such as the CYD, cannot type a password on screen yet, because that
needs UI. Until then, networks are added from the serial console, or by a `wifi.conf` file on
an SD card that the system reads and then removes the password from.

## 4. Size

Wi-Fi with HTTPS, including the certificate bundle, is large, probably several hundred KB, and
not measured yet. That is why the 4 MB boards leave it out of KittenOS. On modular boards
KittenOS's partition can be made larger if it needs to be.

## 5. Testing

- The connection service against a fake Wi-Fi driver: saved networks, reconnect after a drop,
  wrong password, network not found, several saved networks in range.
- The password file: root-only access, and that an app cannot read it through any catcall.
- Time: NTP never raises the floor, and an HTTPS `Date` header does.
- The shell commands against fake networks.

## 6. Open questions

- **HTTPS for apps.** Whether apps get a TLS client through a catcall, or bring their own
  (`../Catcalls/SPEC.md` section 12).
- **The certificate bundle:** how it is kept up to date, since the authorities it holds age out,
  and whether KittenOS has its own copy.
- **Provisioning without a keyboard:** the serial and SD card routes here are stopgaps.
- **Captive portals,** enterprise Wi-Fi, static addresses and the hostname.
- **Power saving** for the radio.
- **A hotspot mode** on the device, for setup.
- **IPv6,** proxies, and time zones.
