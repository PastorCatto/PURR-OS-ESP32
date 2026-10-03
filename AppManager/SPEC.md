# AppManager spec (draft 0.4)

The part of the system that knows which apps are installed and changes that set
safely. A fresh design, built from decisions made for the rewrite and not from
the old runtime manager.

## 1. Scope of the first chunk

**In**

- **Local apps only.** Apps installed on this device, run on this device.
- **Pre-compiled app images (`.cat`).** Every app is a signed, pre-compiled
  image that talks to the system only through catcalls. There are no scripts and
  no interpreted apps.
- **Add and remove** apps, and list and inspect what is installed.
- **Build** apps: purrstrap produces the signed .cat image (section 6).
- **Copy apps over from a PC** through an **MTP session** that AppManager opens
  on request (section 7).
- **Update itself,** as a system file, by the shared procedure (section 9).
- **A command-line shell** with launcher and task manager commands, on the serial
  console first (section 8.1).

**Out for now**

- Sending apps between devices, app stores, or any network transfer.
- Other app formats. MicroPython apps, run by a second runtime module, come
  later. The design leaves room for them (`../AppRuntime/SPEC.md`).
- Launching and running apps (the app runtime does that), and any graphical UI.
  The command-line shell that drives them lives here (section 8.1).
- Permissions, user accounts, app data sync.

## 2. Constraints from the rest of the design

- Everything other than the bootloader is a signed module, updated
  independently (`PurrOS/SPEC.md` section 1). An app is one type of module and
  is verified with the same key system, through CoreOS's image verification.
- Apps use catcalls and nothing else (section 3). The kernel and drivers are
  `.kitt` modules, apps are `.cat` images: different files, different rules.
- KittenOS is recovery only and can add and remove mini-apps, so the install and
  remove code must work in both bootable images.
- No PSRAM on the first board (CYD 2.4C, 4 MB flash).

## 3. The `.cat` app image

An app is a `.cat` file: the module container (`bootloader/SPEC.md` section 3)
with `image_type = 4` (app), holding pre-compiled code.
The exact file format is in `../CatFormat/SPEC.md`. This section gives the outline
and the reasons.

**Catcalls are the whole interface.** An app never calls the OS, ESP-IDF or
hardware. At start it receives one structure, `purr_app_env_t`, whose entry point
is `catcall_get(id, min_major, min_minor)`. Everything the app does goes through
catcalls: drawing its UI, reading touch and keys, storing its own data, time and
logging. Its memory is a fixed block handed over by the loader, sized by the
header.

What follows from that:

- **Compatibility is the catcall version rule.** Major must match and minor must
  be at least what was asked for (`PurrOS/components/kernel/SPEC.md` section 7).
  An app built against `display 2.0` and `ui 1.x` runs on any device that
  provides those, including through the legacy adapters.
- **The capability boundary is the catcall list.** An app cannot touch pins,
  buses, flash or other apps, except through a catcall the device provides. A new
  ability means a new catcall, never a new import.
- **Missing hardware is detected at install.** The image lists the catcalls it
  needs, and the installer refuses one the device cannot provide.
- **Drawing a UI needs a higher-level catcall.** The kernel's `display` catcall
  only pushes pixels, which would make every app carry its own font and drawing
  code. A `ui` catcall (canvas drawing, text, images, input events) has to be
  designed. It belongs to the app runtime layer, and it is a prerequisite for
  useful apps.

**Why the same image can work on another device.** The interface is
device-independent. What can differ is the machine code:

- ESP32 and ESP32-S3 are both Xtensa but not identical cores. The ESP32 has a
  floating-point unit and the S3 does not, and the S3 adds vector instructions.
  An image built for the common base instruction set (no FPU, no vector
  instructions) is expected to run on both. That has to be confirmed on hardware.
- RISC-V chips (ESP32-C3, C6 and so on) cannot run Xtensa code at all.

So a `.cat` can carry **one payload per instruction-set family**, and the
installer picks the one that matches the device or rejects the image with that
reason. For the first boards a single Xtensa-base payload is the common case.

Fields an app image needs beyond the container's common ones (fixed when the
format is written down, in `bootloader/SPEC.md` section 3):

- A payload table: instruction-set family, payload offset and size, SHA-256.
- Per payload: entry offset, RAM needed for `.data` and `.bss`.
- Runtime needs: stack size, heap size, and whether it may run in the background.
- The runtime it needs: the app kind and the minimum runtime version.
- The catcalls it needs, each with the minimum major and minor version.
- Display name and icon, optional.

**Loading model: undecided.** The OS is multitasking (`PurrOS/SPEC.md` section 5),
so several apps can be running at once, and each needs its own code and data.
Running in place needs a separate code window per concurrent app, and loading into
RAM needs code RAM per concurrent app. That makes the number of concurrent apps a
design parameter, not a side effect. Without PSRAM, an app has to run from RAM or in
place from flash. Running from RAM means relocating the code into executable
memory, which limits app size to what the chip has free for code. The limit is
unknown until measured on the board. See section 11.

## 4. Storage and registry

- Installed packages live in a storage filesystem, one folder per app:
  `apps/<name>/` holds the package and, later, its data.
- **The filesystem is the source of truth.** The registry is a cache built by
  scanning `apps/`, so it can be deleted and rebuilt, and can never disagree with
  what is installed for long.
- Registry entry: name, version, package size, which payload matches this chip,
  the runtime it needs, signer role, verified yes or no, and the time it was installed.
- An `incoming/` folder is where packages are dropped by a transport (section 7).

The filesystem is **LittleFS**, on the `apps` partition. It is designed to survive
power loss and supports atomic renames, which the install steps in section 5 rely
on. Confirm the rename guarantee against the LittleFS version that gets used.

**Capacity.** The apps area is about 1 MB, and what limits it is bytes, not a
count. About 15 average-sized apps is only an example: it can hold fewer large
apps or more small ones.

- The registry is a fixed table sized by a build setting (`MAX_APPS`, default
  32), so there is no heap use. If the table or the space runs out, adding fails
  and says which.
- A per-app size cap (`max_app_size`) is fixed once the loading model and the
  code RAM on the board are measured. It may be lower than the free space.
- An install writes a temporary copy first, so the free space has to cover the
  incoming app plus overhead. The installer checks that before it starts.

## 5. Operations

**Add** (from a package file already on the device)

1. Read the container header and check chip, type and name. No payload matches
   this chip: reject with that reason.
2. Verify with `purr_image` (hash and signature against the key bag). A failure
   is rejected unless `secure_mode` is off, the same rule CoreOS uses.
3. Check the catcalls the app needs against what this device provides, and
   that the runtime it needs is present at a high enough version.
4. Copy to `apps/<name>.tmp/`, check the copy's hash again, then rename into
   place. Never write into a live app folder.
5. Add the registry entry.

A package whose name is already installed is an **update** if its version is
higher, and is rejected otherwise. An update keeps the old version until the new
one is fully in place, so a power cut leaves one complete version.

**Remove:** delete the app folder, then the registry entry. If the delete is cut
short, the next scan finds no complete package and cleans up.

**List** and **inspect:** installed apps with status, plus free space and any
package that failed verification.

**Recovery of a cut operation:** at start, delete every `*.tmp` folder and any
`incoming/` file that never finished, then rebuild the registry.

Every operation returns a result code the caller can show (not installed
because: bad signature, wrong chip, no space, needs catcall X, older version,
and so on).

## 6. Building apps (purrstrap)

A new purrstrap subscript, `apps`, does what the C side cannot: compile an app
for each instruction-set family, assemble the .cat image, and sign it.

- Actions: `build` (compile per instruction-set family), `package` (assemble and sign), `verify`
  and `inspect` (same rules as the device), `install` (push over a transport).
- Needs a crypto package for signing, so it declares `requires`.
- The app SDK (the catcall headers an app builds against, a link script and
  example apps) is a separate spec, written before the first app.

## 7. Transports and the MTP session

A **transport** brings a package onto the device. AppManager defines the
interface, the transport only has to put a complete file in `incoming/` and say
when it is done. Adding a transport never changes install logic.

| Transport | Chips | Notes |
|-----------|-------|-------|
| MTP session | ESP32-S2, ESP32-S3 (USB device) | Chunk 1 target. Section below. |
| SD card | any board with an SD slot | Copy the package on a PC, insert the card. Works on the CYD 2.4C. |
| Serial | any | purrstrap pushes the package over the UART. Works on the CYD 2.4C. |
| Another device | any with Wi-Fi | Over a shared network or ESP-NOW, with the receiver confirming (`../Transfer/SPEC.md`). |

**MTP session**

- **The user asks for it.** "Connect to PC" starts an MTP session and closes it
  when the user ends it or the cable is pulled. USB is not left on in normal use.
- **File level, not disk level.** MTP exposes files, not a raw disk, so the
  device keeps ownership of its filesystem. That is the reason to prefer it over
  USB mass storage, which needs the device to unmount first.
- **One storage shown to the PC:** `PURR Apps`, with `incoming/` writable, and
  `apps/` listed. Deleting a folder in `apps/` from the PC is a remove request,
  handled by AppManager (not blindly applied to the filesystem).
- **Install on completion:** when a file finishes copying to `incoming/`,
  AppManager runs the Add steps and reports the result on the device. A rejected
  package is deleted from `incoming/`, and the reason is kept for display.
- **Device stays usable.** While a session is open, installs are queued and
  applied one at a time. Apps are not launched during a session.
- **Limit per package** (default 2 MB, from the storage size) so a bad transfer
  cannot fill the disk.

**Hardware reality.** The CYD 2.4C is an original ESP32, which has no USB device
controller (its USB port is a serial bridge chip). **MTP cannot run on the first
board.** It can be built and tested on an S3 board, for example the T-Deck Plus,
which has native USB. On the CYD the first working transports are SD card and
serial. The MTP design does not depend on the CYD.

**MTP implementation risk.** ESP-IDF's USB stack (TinyUSB) has to provide the
device side. Whether TinyUSB in ESP-IDF 5.3.5 includes an MTP class is not
confirmed. If it does not, an MTP responder has to be written (bulk transfers,
an interrupt endpoint and a small command set: open session, storage info,
object list, get, send, delete). That is a sizeable piece of work and gets its
own spec before code.

## 8. Where it lives

AppManager is **its own module, not part of the kernel or CoreOS.** On modular boards it is a
file in `/boot` of the root filesystem, loaded by CoreOS after the kernel, and changed rarely.
On monolithic boards it is linked into the packed image.

- It is a native module: a `.kitt` container with the module role `appmanager`,
  `image_type = 3`. It is not a `.cat`, because it needs raw storage access, executable memory
  and the USB transport, which a catcall-only app never gets.
- **In PURR OS**, CoreOS loads it through the same loader and call table it uses for the other
  modules (`PurrOS/components/coreos/SPEC.md` section 6).
- **Its core is plain C** with no ESP-IDF dependency (packages, registry, add, remove, recovery
  of a cut operation), so it can be tested on the host. The module wrapper and the transports
  are the only parts that touch hardware.
- **In KittenOS**, recovery carries only the swap routine (`PurrOS/SPEC.md` section 6.1) and
  loads this file from `/boot` if it is there and verifies. So KittenOS gets its app and update
  handling from the same module as PURR OS.

### 8.1 The shell (command line)

**The pipes/redirection/variables/job-control style described below is designed, not built**
([F-04](../documentation/FINDINGS.md#f-04), `PurrOS/components/coreos/SPEC.md` section 3.7). What
runs today is tokenizing, line editing and a command table.

The core system in AppManager is a **command-line shell** with a launcher and a
task manager, in a Unix-like style. There is no graphical UI in this work. A
graphical launcher comes later, on top of the same services (`../UI/SPEC.md`).

**Style**

- Small single-purpose commands, plain-text output that a person or a script can
  read, and an exit code from every command.
- **Pipes and basic shell syntax:** `a | b`, redirection (`>`, `>>`, `<`), `;`,
  `&&` and `||`, `&` for background jobs, simple variables, comments and script
  files. The engine and the exact list are in
  `PurrOS/components/coreos/SPEC.md` section 3.7.
- **Logs:** output from an app that is not in the foreground goes to the system
  log with its id and name in front, and is read with `log`. Output that was
  redirected explicitly goes where it was told.
- A conventional directory hierarchy on storage. Proposed: `/apps` for installed
  apps, `/data` for their data, `/etc` for configuration, `/var/log` for logs and
  `/tmp` for scratch space. Open (section 11).
- Runs on the serial console first, which works on the CYD. An on-screen text
  console can follow without changing the commands.

**Launcher commands** (names are placeholders)

| Command | Does |
|---------|------|
| `apps` | List installed apps |
| `info <app>` | Show one app's header, size, runtime and signer |
| `install <file>` | Add a package (section 5) |
| `remove <app>` | Remove an app |
| `run <app>` | Start an app |

**Task manager commands** (placeholders)

| Command | Does |
|---------|------|
| `ps` | List running instances: id, name, state, memory |
| `top` | Live view of state and memory |
| `kill <id>` | Stop an instance |
| `stop <id>`, `cont <id>` | Suspend and resume |
| `fg <id>`, `bg` | Give the console to an instance, or take it back |
| `log` | Show the system log, including background app output |

**How it is built**

- The shell does not run apps itself. It asks the registry what is installed, and
  the runtime manager (`AppRuntime/SPEC.md`) for instances, states, and the run,
  suspend and stop calls.
- **The command-line engine lives in CoreOS** (`purr_cli`,
  `PurrOS/components/coreos/SPEC.md` section 3.7): line reading, parsing and a
  command registry. KittenOS uses the same engine, so a console exists before
  AppManager loads. AppManager registers its commands into it, and other modules
  can register theirs.
- **Console ownership follows Unix job control.** The foreground instance owns
  console input, `fg` and `bg` change which one, and apps write text through the
  `console` catcall (`AppRuntime/SPEC.md` section 8), which exists before any `ui`
  catcall.
- In KittenOS the same shell runs, because KittenOS loads this module.

## 9. Updating AppManager

AppManager is a system file, so it is updated by the shared procedure in `PurrOS/SPEC.md`
section 6.1. That procedure is not repeated here. In short, AppManager stages and verifies the
new file in `/boot` as `appmanager.new`, records the request and restarts into KittenOS, which
swaps it in and rolls back if needed. AppManager never replaces its own file.

What is specific to AppManager:

- **Its part is the first three steps:** stage, verify, and request, after quiescing.
- **Quiesce** means: stop the app runtime, close any MTP session, and end AppManager's own
  tasks.
- **Apps are untouched.** The `apps/` folders and the registry live in the apps area, not in
  `/boot`, so an AppManager update leaves them alone. The new version has to read the registry
  the old one wrote, so the registry format is versioned, and an unreadable registry is simply
  rebuilt from `apps/`.
- **The module version, call table version and the catcalls it needs** are checked against
  CoreOS when it loads, and by KittenOS before the swap.

## 10. Testing

- **Host, with a fake filesystem:** package parsing, chip matching, the Add
  steps, update rules, and the registry rebuild.
- **Power loss:** the fake filesystem can stop after any write. Every stopping
  point has to leave exactly one complete version, or none, and the next start has
  to clean up. This is tested exhaustively.
- **Verification:** signed sample packages from purrstrap in `test/vectors/`,
  including wrong chip, bad hash, bad signature, wrong role and an older version.
- **Self-update:** AppManager's part (finding the staged image, verifying it,
  quiescing, asking for the move) is tested on the host. The move itself and
  its power-loss behavior are tested where the routine lives, under CoreOS.
- **MTP:** a manual test matrix on real hardware with a Windows PC (copy a
  package in, delete one, disconnect mid-transfer, fill the storage). Automated
  MTP testing is not planned in chunk 1.

## 11. Open questions

- **Loading model for pre-compiled apps.** Run from RAM after relocation, or in
  place from flash? This decides the maximum app size and whether apps can live
  in a filesystem at all. On monolithic boards such as the
  CYD it needs a measurement of the free code RAM. On modular boards the apps can be
  loaded into PSRAM, which likely removes the code RAM cap (`ModuleSpike/SPEC.md`).
- **Instruction-set compatibility.** Does an Xtensa-base build really run on both
  ESP32 and ESP32-S3? Is one payload enough for the boards we care about, or is a
  payload table needed from the start? Should RISC-V ever be supported?
- **The `ui` catcall:** who designs it, and how high-level it is (immediate
  drawing, or a widget tree the system renders).
- **SD card:** whether apps can also be installed to and run from an SD card, in
  addition to the LittleFS partition.
- **The `.cat` image format** is defined here in outline only. The exact header
  goes into `bootloader/SPEC.md` section 3 once the loading model is decided.
- **Signing:** who may sign an app, and does an unsigned app install when
  `secure_mode` is off?
- **First MTP board:** T-Deck Plus, or another S2 or S3 board? And is it fine
  that the first chunk's MTP piece is only testable off the CYD?
- **App data:** where it lives, whether it survives an update, and whether
  removing an app deletes it.
- **MTP responder:** use TinyUSB's class if it exists, or write one.
- **Shell design:** command names and options, the output format, and the exact
  directory hierarchy. Whether the log is also kept on flash is open in the
  CoreOS spec.
