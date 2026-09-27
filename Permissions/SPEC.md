# Permissions spec (draft 0.1)

How apps get access to hardware and system features. The model is Android-style
allow and deny on top of a Unix-style core. **Where a question has an obvious
Android answer, Android's behavior is the default.** Items that are assumed from
that rule and not explicitly discussed are marked *(assumed)*.

Related: apps and their manifest (`../AppSDK/SPEC.md`, `../CatFormat/SPEC.md`),
installing and the shell (`../AppManager/SPEC.md`), running apps
(`../AppRuntime/SPEC.md`).

## 1. Decisions so far

- An app lists the permissions it wants in its manifest.
- **Grants are per user.** Each user has their own record of what they allowed
  (`../Users/SPEC.md`).
- **At install,** the system shows that list, for information. It decides nothing.
- **At first use,** the system asks the user, at the moment the app first reaches
  for the feature. An app that never touches Wi-Fi is never asked about Wi-Fi.
- **Normal permissions** (the basic catcalls) are granted automatically at install.
  **Dangerous permissions** are asked at first use *(assumed)*.
- **Prompt choices:** allow always, allow this once, or deny. After repeated denials
  the user can choose "don't ask again" *(assumed)*.
- **Denial:** the catcall returns a "permission denied" error and the app has to
  handle it *(assumed)*.
- **Revocation:** the user can change or remove any grant later. The next attempt
  asks again, unless "don't ask again" is set *(assumed)*.
- **Prompts are text on the console for now,** since the shell comes first. A dialog
  can replace them later.

## 2. The permissions

**Normal (granted at install):** the basic catcalls: `console`, `sched`, `app`, the
app's own storage folder, `system` (read-only battery, time, memory), audio output, and
`internet` (sockets). Following Android, none of these is asked.

**Dangerous (asked at first use):**

| Permission | Covers |
|------------|--------|
| `wifi` | Scanning and connecting |
| `bluetooth` | Bluetooth |
| `radio` | LoRa and similar radios |
| `gps` | Location, as parsed fixes |
| `gps-raw` | The raw GPS stream (`gps_raw`) |
| `shared-storage` | Files and the SD card outside the app's own folder |
| `input-monitor` | Reading keys while the app is not focused |
| `audio-input` | The microphone (a later catcall) |
| `system-settings` | Brightness, time and similar settings |
| `hardware-raw` | Direct GPIO, I2C and SPI (`gpio`, `i2c`, `spi`), never pins or buses that drivers own |
| `device-admin` | Adding, removing and exporting other installed apps (`appmgr` catcall) |

**Special:**

| Permission | Rule |
|------------|------|
| `superuser` | Only an app signed with the **owner key** can hold it. It never appears in a prompt, so nobody can be talked into granting it. |

## 3. Privileged apps

- The privileged class has **every permission on, and they cannot be turned off**
  except by deleting the app.
- The system shows a large warning at **install** and again at **first launch**:
  this is a system-level app, be careful. The shell marks it `privileged` in `apps`
  and `ps` at all times.
- **Official system-signed apps skip the warning,** as Android's platform-signed
  apps get their permissions without prompts. Any other privileged app, including
  one signed with a test key, still shows it.
- A privileged app has `device-admin` automatically. An unprivileged app must be
  granted it, with a separate strong warning.

## 4. The owner key and test builds

- `superuser` is gated by the owner key, not by a prompt.
- **Developer-preview builds** (DP 10) trust a **published test owner key**. The
  private key is public on purpose, so in those builds `superuser` is open to anyone
  who downloads it. The boot report shows that a test key is trusted, so a preview
  device is never mistaken for a protected one.
- **Official releases carry no test key.** They carry only the release keys, which
  sign drivers, system apps and every module.
- The key roles this needs (owner, system, developer) are defined in the signing
  chunk.

## 5. Enforcement

- **Where.** When an app asks `catcall_get` for a catcall, the runtime manager
  (`../AppRuntime/SPEC.md`) checks the app's class and grants. Sensitive calls can
  be checked again on each use. *(Proposed.)*
- **Who owns the grants.** AppManager stores them, one record per app, and gives
  the runtime manager the current table when the app starts and whenever a grant
  changes. *(Proposed.)*
- **Fail closed.** If the table is missing or unreadable, everything dangerous is
  denied.
- **Limits.** On the original ESP32 there is no memory protection, so enforcement is
  by the SDK's rules, by what `catcall_get` hands out, and by signing. It is not a
  hardware wall. On the ESP32-S3, what the hardware permission blocks can add is
  unverified (`../AppSDK/SPEC.md` section 4).

## 6. Prompts

- One prompt at a time. Others wait in a queue.
- **Background apps and daemons cannot prompt.** Their request is denied unless the
  permission is already granted, as on Android *(assumed)*.
- A prompt names the app, the permission and what it covers. On the console it looks
  like: `clock wants to use Wi-Fi. Allow always / once / deny?`

## 7. Storage, updates and removal

- Grants are stored per app and survive reboots.
- **Update:** grants are kept. A permission the new version asks for that the old one
  did not is treated as a new request at first use *(assumed)*.
- An update that raises the class from unprivileged to privileged shows the
  privileged warning again, as if it were a new install *(assumed)*.
- **Removal:** deleting an app deletes its grants.

## 8. Shell commands (placeholders)

- `perms <app>`: list an app's requested and granted permissions
- `grant <app> <permission>` and `revoke <app> <permission>`
- `reset <app>`: forget all decisions, so every permission asks again

Changing another app's permissions needs the shell's own authority (root).

## 9. Testing

- A table-driven test of every combination of class, permission, grant state and
  caller, checking allow, deny or prompt.
- Grants survive a reboot. Revocation takes effect on the next call.
- A missing or corrupted table fails closed.
- A background app that needs a prompt is denied and logged.
- The update scenarios in section 7.
- `superuser` is refused unless the signing key has the owner role.

## 10. Open questions

- Which catcall belongs to which permission is in `../Catcalls/SPEC.md` section 1.
- **"Allow only while the app is in use"** for things like location, as on Android.
- **Auto-reset of permissions for apps that are not used for a long time,** as
  Android does.
- How long a "this once" grant lasts, and whether prompts time out.
- How the user sees all their permissions in one place, beyond the shell commands.
- The exact key roles (signing chunk).
