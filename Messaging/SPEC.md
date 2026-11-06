# Messaging spec (draft 0.1, 2026-11-05)

The Messages and Contacts apps and the way messaging services plug into them. **Nothing here is built.** It records the owner's decisions of 2026-11-05 and the questions that are still open. The
screens are drafted in the artifact shown to the owner (Froyo style, 320 by 240); the plan is in `../plan9.md` ("Messages and Contacts apps").

Related: permissions (`../Permissions/SPEC.md`), the app manifest and runtime (`../AppSDK/SPEC.md`, `../AppRuntime/SPEC.md`), the catcalls (`../Catcalls/SPEC.md`), the Store and repositories
(`../Repositories/SPEC.md`), fonts (`../showtime1.md`, Part D).

## 1. The pieces

| Piece | What it is |
|---|---|
| **Messages** | The front end. Lists threads, shows a thread, composes, attaches pictures, picks emoji. Keeps the message store. Written for the Froyo shell (the layout follows the Android 2.2 Messaging app). |
| **Contacts** | A separate app. The address book: a person, with one address per messaging service. Follows the Android 2.2 Contacts layouts (letter headers, rows with a quick-action icon, a card above data rows, Done and Discard in the editor). |
| **Service apps** | Meshtastic, Reticulum and MeshCore, each its own app, each declaring itself as a messaging service in its manifest (section 2). The owner plans to release the Reticulum and Meshtastic ones himself. |
| **The radio lease** | The system's arbiter for the one LoRa radio (section 4). |
| **Companion** | An optional driver or system service that a service app ships for work an app cannot do (section 5). |

If the user keeps a service's own app as the front end, Messages leaves that service alone. If the user hands the service to Messages, the service's app is hidden and its settings are drawn inside
Messages (section 3).

## 2. The manifest section

An app declares itself as a messaging service with a `[provides.messaging]` section in its `manifest.toml`. The values below are examples; the real limits come from each service.

```toml
[provides.messaging]
version    = "1.0"
name       = "Meshtastic"
tag        = "MTS"                # up to four letters, shown on rows
address    = "node id"            # what identifies a person on this service
text_max   = 200                  # bytes in one message
images     = "preview"            # none, preview or full
image_max  = 2048                 # bytes of picture it can carry
needs      = ["radio"]
radio      = "lease"              # holds the radio only while it owns it, and yields when asked
companion  = "none"               # or "driver:<name>" or "service:<name>"

[[provides.messaging.setting]]    # Messages draws these when it takes the service over
key     = "region"
label   = "Region"
kind    = "choice"                # choice, text, number or toggle
options = ["EU 868", "US 915", "AU 915"]
```

- The settings are a list and not a screen, so Messages can draw them in its own window and they look like the rest of the system. The list is part of the app's signed package.
- Declaring the section needs no permission. It is shown to the user at install, like the permission list: "This app can act as a messaging service."
- An app that declares it must also handle the radio lease if it has `needs = ["radio"]` (section 4).

## 3. Handing a service to Messages

1. When a new messaging app is installed (or Messages starts and finds one it has not seen), Messages asks once: "Show its messages in Messages and move its settings here? Its own app will be hidden."
2. **Use Messages:** the service app is hidden from the launcher (not uninstalled), is started by Messages in the background, and its settings page appears inside Messages. A row in those settings
   brings the app back.
3. **Keep its app:** nothing changes; Messages does not touch that service and does not list its threads.
4. The choice is per user and can be changed from Messages' services screen.

## 4. The radio lease

The three mesh services use the one LoRa radio with incompatible settings, so one app holds it at a time.

1. An app that needs the radio asks the system for a lease, with the settings it wants. It is granted if nobody holds it.
2. To change networks, Messages asks the current holder to yield. The holder is told, and has a short time (a few seconds, to be fixed in the build) to finish its transmission, save its state and release.
   If it does not, the system takes the radio back and tells the app that it lost it.
3. An app that is stopped, or sent to the background and put to sleep, releases the lease.
4. Switching takes a few seconds and messages that arrive on the other network meanwhile are missed; the services screen says so.
5. A message goes out on the service of its thread. It is never moved to another network on its own.

Holding or asking for a lease needs the `radio` permission (`../Permissions/SPEC.md`). How an app reaches the radio itself (the driver exists, an app-facing call may not) is to be checked.

## 5. Companion drivers and system services

A service app may ship a companion for work an app cannot do: a signed **driver** for timing-critical radio work, or a **system service** that keeps running when the app itself is stopped (the way the
Store's companion works). The manifest names it.

- **Approval, for now:** anyone with a signature may publish one. A list of trusted publishers is planned, and apps from it will be approved without a prompt; the owner expects the first Reticulum and
  Meshtastic services to come from himself.
- A system service has more reach than an app, so the install screen says so plainly, and a companion never gets more permissions than its app declares.

## 6. Threads, contacts and channels

- **One thread per service.** A person who is on two services has two threads. Contacts joins them: a contact has one address per service, and its card lists them with an action that opens the thread.
- **Channels are not private messages.** Messages opens on the list of private threads. The **Menu key** (or the options button) shows a menu with **Channels** (LongFast, General and any others the
  service offers), New message, Contacts, Services and Settings, following the shell's navigation. Channels are a separate list, because everyone on the channel sees every message.
  LongFast and General are the public ones.
- **Failed sends.** A failed message stays "Not sent" and can be tapped to retry. If the person has an address on another service, the app offers a one-tap resend there. Automatic fallback is an
  option, off by default, because the radio has to switch networks and a slow first attempt can still arrive later as a duplicate.

## 7. Storage

- Messages live in a folder on the flash file system, under the signed-in user's folder, until together they have used **1 MB**.
- When a micro SD card is inserted after that, everything stored so far moves to the card, and from then on fresh messages go on flash again.
- Pictures count toward the 1 MB like any other message data.
- **Open:** what happens at 1 MB with no card (the proposal is to warn once and then delete the oldest messages first), and whether the user can choose a different limit.

## 8. Images and emoji

- **Images:** attached from Photos or the user's files. Each service declares what it can carry (`images` and `image_max`). A picture bigger than that is shrunk to a preview; a service that carries
  no pictures refuses the attachment with a message. Previews show in the thread and open in Photos. The sizes in the draft screens are examples: what each protocol can carry has not been checked.
- **Emoji:** drawn from an emoji font loaded from a TrueType file, so the emoji can be updated by replacing the file. **Monochrome** (outline glyphs, tinted by the text colour) is the decision for the
  first version; no colour. The font falls back text font, emoji font, then the built-in font. The picker is a grid of buttons (`ui` version 3), and a short code such as `:smile:` also works.
  Loading fonts from files is Part D of `../showtime1.md` (LVGL's Tiny TTF, off in the build today).

## 9. Permissions

| Permission | Covers | Notes |
|---|---|---|
| `messaging` | Reading and sending messages through the shared message layer, and listing the messaging services. Messages needs it. | Dangerous: asked at first use. |
| `contacts` | Reading and changing the address book. Messages and Contacts need it. | Dangerous: asked at first use. |
| `radio` | Asking for the radio lease. Already in the permissions list ("LoRa and similar radios"). | Dangerous: asked at first use. |

## 10. How Messages talks to a service app

- **Today:** the `event` catcall. An app publishes events named `custom.<app>.<name>` and can subscribe by prefix, up to four subscriptions per app. That is enough for a first version with three services:
  Messages subscribes to each service's prefix, sends by publishing a request, and gets incoming messages, receipts and status back as events. Events carry short text only.
- **Later:** a `messaging` catcall with send, list, status and change notifications and room for attachments. Pictures need it, or a file handed over by path.

## 11. To check before building

1. The size and memory of each mesh stack inside an app (apps are freestanding: no C library, a small stack), and whether an embeddable version exists for the ESP32-S3.
2. The licences of the three stacks.
3. What each protocol can carry for pictures and text.
4. The cost of Tiny TTF in code size, memory and the speed of the first draw of a glyph.
5. Where each network's keys live and how they are backed up.
6. How a Meshtastic channel or group appears in Contacts.
7. The radio lease: the yield time, and what an app sees when it loses the radio.
