# AppSDK spec (draft 0.1)

**Partly built, 2026-10-18.** `Apps/hello/hello_app.c` (AppRuntime/SPEC.md's chunk C/D/E) is
still bare C against `purr_app_abi.h` directly, built by hand with `modules build --kind app`
-- it predates this slice and stays as the original loader smoke test. What's now real: a
`purrstrap apps` subscript (`new`, `build`), one starter template
(`AppSDK/templates/unprivileged/`), and the TOML manifest format (section 5), reusing
`modules.py`'s own compile/relocate/pack pipeline rather than a second implementation.
Confirmed end to end on the host toolchain: `apps new` scaffolds a project, `apps build` reads
its manifest and produces a real, correctly relocated `.cat` (not yet signed or run on
hardware). Still not built: the mini-library (section 2), privilege enforcement, the other
four templates, and the `package`/`verify`/`inspect`/`install` actions (AppManager/SPEC.md
section 6).

What a developer needs to write, build and package a `.cat` app. The format of
the `.cat` file itself is a separate chunk (`../AppManager/SPEC.md` section 3
holds the outline). Running apps is the runtime's job (`../AppRuntime/SPEC.md`).

## 1. Languages

- **C and C++.** C++ is first-class from the start.
- **C++ settings:** exceptions and run-time type information are off by default,
  because both add a lot of code and need support we do not have. Classes,
  templates, inheritance and virtual functions all work.
- **No system libraries.** Apps import nothing by name, so the SDK provides its own
  `new` and `delete` on the app's heap, and the startup code that runs global
  constructors and stubs for what the C++ runtime expects (static-initialisation
  guards, `atexit`).
- **Standard library:** the language, a small header-only subset (`std::array`,
  `std::string_view`, `std::optional`, `std::span` and similar), and heap
  containers (`std::string`, `std::vector`) running on the app's own heap.
- **Allocation failure stops the app.** With exceptions off, the standard
  containers cannot report an out-of-memory error. The app aborts, the runtime
  stops it, records "out of memory", and other apps and the system are unaffected.

## 2. The mini-library

The app's own small standard library, since apps cannot call the OS:

- memory and string functions (`memcpy`, `strlen`, `strcmp` and similar)
- printf-style formatting, writing to the `console` catcall
- `std::string`, `std::vector` and the header-only types above
- math functions
- time and sleep helpers, wrapping the `sched` catcall
- random numbers, wrapping a catcall

Everything in it is built on catcalls, so it is device-independent.

## 3. Building an app

- Through purrstrap's `apps` subscript (`../AppManager/SPEC.md` section 6): `build`,
  `package`, `verify`, `inspect` and `install`, run inside a starter template
  project. Nobody has to run the compiler by hand.
- The project pins the SDK version it builds with, so an app can be rebuilt with a
  known-good older SDK. purrstrap keeps several SDK versions side by side.
- The build produces the flat image and relocation data, checks that the app
  imports nothing but `catcall_get`, and reports its size.

Proposed project layout:

```
myapp/
  <manifest file>        name, version, class, permissions ... (section 5)
  src/                   the app's code
```

## 4. Privilege classes

- **Unprivileged.** Basic catcalls only (such as `console`, `sched`, scoped
  `storage`, `app`). Anything else needs a permission that the user grants.
- **Privileged (root).** Full access to everything: the root console and terminal,
  Wi-Fi, hardware, and direct access to the kernel and CoreOS through their host
  API tables. All permissions are on and cannot be turned off, except by deleting
  the app. The system shows a large warning at install and again at first launch:
  this is a system-level app, be careful. The shell marks it `privileged` in `apps`
  and `ps` at all times.
- **Device administrator.** A permission that lets an app add and remove other
  installed apps, through an `appmgr` catcall that gives controlled access to
  AppManager's install and remove. Both classes can use it. An unprivileged app
  must be granted it, with a separate strong warning. A privileged app has it
  automatically.
- **Signing gate.** A privileged app can only be installed if it is signed with a
  trusted key. Which keys count is decided in the Permissions and signing chunks.

**What enforces this.** On the original ESP32 there is no memory protection, so the
unprivileged limit is enforced by the SDK's rules (an app imports only
`catcall_get`), by which catcalls `catcall_get` hands out to a class, and by
signing. It is not a hardware wall: a hand-built app that ignores the SDK could
still touch memory. On the ESP32-S3 (the T-Deck Plus, the primary board) there are
hardware permission blocks that ESP-IDF uses for system-wide protections. Whether
they can isolate one app from another is not verified and needs research before
anything relies on it.

The permission model itself (the prompts, stored grants, revocation, and which
permission covers which catcall) is its own chunk, Permissions.

## 5. The manifest

The description file in each app project, `manifest.toml` at the project root. **Decided**
2026-10-18 (section 10 used to leave the format open): **TOML**, since purrstrap now requires
Python 3.11+ (`tomllib`, stdlib, read-only parsing is all the build side needs). This also
matches the Hyprland-style `key = value` config philosophy already proven for `/ui/cardshell.conf`.

```toml
name = "myapp"
version = "0.1.0"
description = "A short description"
class = "unprivileged"        # or "privileged"
kind = "command-line"         # "daemon" | "command-line" | "ui"
sdk_version = "0.1.0"
entry = "myapp_entry"         # the C symbol the core calls to enter the app

[catcalls]
console = "1.0"

[permissions]
requests = []                 # e.g. ["device_admin"]

[runtime]
stack_bytes = 8192
heap_bytes = 16384
threads = 1
background = false
saves_state = false

[display]
name = "My App"
icon = ""
```

**What actually reaches the signed `.cat` today, and what doesn't yet.** `purr_abi.h`'s
`purr_image_header_t` (`lib/image.py`'s `HEADER_FORMAT`) only has room for `name[32]` and
`version[12]` — it predates this spec and is shared by every image kind (OS, recovery, module,
app), not just apps. `purrstrap apps build` reads the whole manifest and uses `name`/`version`
to build the image exactly as `modules.py build --kind app` already does, and validates the
rest (class, permissions, catcalls, runtime limits) at build time, but **does not yet embed
them in the binary** — there is nothing on the install/run side to enforce them yet either
(Permissions chunk, AppManager's install-time checks). Embedding the full manifest is a
follow-up once that enforcement exists to actually consume it, not a v1 requirement. This
keeps today's slice inside the existing container format instead of also taking on a
CatFormat/`purr_abi.h` change.

## 6. Starter templates

Five examples ship with the SDK:

| Template | Kind | Class | Shows |
|----------|------|-------|-------|
| System daemon | daemon | set in the manifest | A background service: persistent, logs through the system log, no console focus |
| Command-line app | command-line | set in the manifest | Reads and writes through `console`, works in pipes |
| UI app | UI | set in the manifest | Uses the `ui` catcall. Written now, but cannot run until that catcall exists |
| Unprivileged app | any | unprivileged | Basic catcalls only, and requesting one extra permission |
| Privileged app | any | privileged | Root-level access, with the install and first-launch warnings |

## 7. Versioning

The SDK has its own version number, separate from the catcall versions. Whether an
app runs on a device is decided by the catcall versions it asks for. The SDK version
is recorded in the app header for diagnostics, and lets a project roll back to a
known-good SDK.

## 8. Layout in the repository

```
AppSDK/
  SPEC.md
  include/        the catcall headers apps build against, and the mini-library headers
  lib/            the mini-library
  link/           the link script and startup code
  templates/      the five starter projects
```

The catcall headers are owned by the kernel and the runtime. The SDK packages a
versioned copy, so there is one source of truth.

## 9. Testing

- The mini-library is compiled natively on the PC and tested there.
- Every template must build with the pinned SDK, and its size is reported.
- Each built template must pass the import check: nothing but `catcall_get`.
- Templates run on a board once the runtime exists.

## 10. Open questions

- **Toolchain.** Does a developer need the ESP-IDF Xtensa toolchain installed, or
  does the SDK provide one? Where do the SDK versions come from?
- ~~**Manifest format.**~~ **Decided** 2026-10-18: TOML (section 5). purrstrap's minimum
  Python version is raised to 3.11 for `tomllib` (`purrstrap/SPEC.md` section 1).
- **Standard containers.** Whether the toolchain's own C++ headers work with SDK
  supplied error handlers, or a small embedded container library is needed. A build
  check, not a decision.
- **Daemons:** whether they start at boot, and how that is configured.
- **Which class the daemon, command-line and UI templates use** by default.
- **Hardware isolation on the ESP32-S3:** what the permission blocks can do for
  per-app protection.
- **Catcalls that count as basic** and which need a permission (Permissions chunk).
- **Which keys may sign a privileged app** (signing chunk).
- **Documentation:** how the SDK's reference is written and published.
