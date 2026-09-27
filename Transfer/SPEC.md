# Moving apps between devices spec (draft 0.1)

How an app gets from a repo, a PC or another device onto a device, and the app that does it.
Installing is AppManager's job (`../AppManager/SPEC.md`). The app repo and its index are in
`../OTA/SPEC.md` sections 1 and 6. The network is in `../Network/SPEC.md`.

## 1. Decisions so far

- **The app storage is an app,** a sibling of the OTA app, and it sits among the other apps. It
  browses the app repo, installs what the user picks, and sends and receives apps.
- **It is an ordinary, unprivileged `.cat`.** It asks for the `internet` permission and
  `device-admin`. The OTA app is privileged because it needs the `update` catcall, which stages
  system files. The app storage only installs apps, so it cannot touch the system.
- **Direct device-to-device transfer is in,** beside the other ways an app arrives (the app repo,
  an SD card, MTP from a PC).
- **Two links, one transfer protocol.** Over a **shared Wi-Fi network** when both devices are on
  one, which is fast. Over **ESP-NOW** when there is no shared network, which works anywhere at
  short range and is slow. The protocol on top is the same either way.
- **The receiver always asks its user first.** "Device X wants to send you the app Y" with its
  size, signer and the permissions it asks for. Nothing is stored until the user accepts.
- **Whatever arrives is verified and installed the normal way.** The signature is checked, and
  AppManager's install steps run (`../AppManager/SPEC.md` section 5). A sender cannot push anything
  unsigned or unwanted.
- **Receive mode.** A device is only discoverable while the user has turned receive mode on.
  - The default is **timed**: on for a short time (a couple of minutes), then quiet again.
  - The user can switch it on **for the whole session.** A reboot puts it back to the timed
    default, so it never stays on by surprise.

## 2. The ways an app can arrive

| Way | Notes |
|-----|-------|
| The app repo | The app storage reads the index and downloads the file (`../OTA/SPEC.md`) |
| An SD card | Copied on a PC, then installed |
| MTP from a PC | Modular boards only (`../AppManager/SPEC.md` section 7) |
| Another device | This spec |

## 3. Direct transfer

**Sending.** The user picks an installed app in the app storage and a nearby device that is in
receive mode. To read another app's file the app storage uses the `appmgr` catcall, under
`device-admin`, which gains an export call. *(Proposed.)*

**Receiving.**

1. The receiver is in receive mode and visible to nearby devices.
2. A sender offers an app. The offer carries its name, version, size, signer and requested
   permissions.
3. The receiver's user accepts or declines. On the console this is a text prompt for now, and a
   dialog later.
4. On accept, the file is sent in chunks, each checked, with the whole file's hash checked at the end
   and resumable if the link drops.
5. The receiver verifies the signature and installs through AppManager. A rejected file is deleted
   and the reason is shown to both sides.

**Limits.** A maximum size per transfer, a rate limit on offers so a device cannot flood another,
and a timeout on an unanswered offer.

## 4. Prior art

Your old system had Milkbar and a remote app protocol over local radio, with list, launch, stop and
download calls and chunked downloads. Only the download idea carries over. Remote launch and control
are not part of this.

## 5. Which boards

Any board with a Wi-Fi radio can take part in the main system. KittenOS does not take part, and
the 4 MB boards have Wi-Fi in the main system only (`../Network/SPEC.md` section 1).

## 6. Testing

- The protocol against a fake link: chunking, a dropped link and resume, a corrupted chunk, and a wrong
  final hash.
- The receive rules: nothing stored before accept, a decline, an unanswered offer, and a rejected
  signature.
- Receive mode: timed expiry, the session setting, and that a reboot returns to the timed default.
- Offers from a flooding sender are limited.
- Both links carry the same protocol and give the same result.

## 7. Open questions

- **The protocol details:** framing, the chunk size on each link, and the resume scheme.
- **ESP-NOW specifics:** its range and rate, whether to use its encryption, and how it shares the
  radio when the device is connected to a Wi-Fi network on a different channel.
- **Discovery:** how a device in receive mode announces itself on each link, and how devices on a shared
  network find each other.
- **A Python app is two files** (`../MicroPython/SPEC.md` section 2). Whether they travel as one unit.
- **Pairing and remembered devices,** which could come later. The confirmation would stay.
- **Progress display,** which is text on the console for now.
- **Whether an app can be sent to several devices at once.**
- **A maximum transfer size,** given the small apps areas.
