# MicroPython spec (draft 0.1)

The second runtime, `mpyrt`, which runs Python apps beside the native `.cat` apps. It is a
runtime module in the sense of `../AppRuntime/SPEC.md` section 3. The catcalls it exposes are
in `../Catcalls/SPEC.md`, and permissions are in `../Permissions/SPEC.md`.

## 1. Decisions so far

- **Modular boards only** (at least 4 MB flash, at least 2 MB PSRAM, and MTP). It is too large
  for the 4 MB tier (`../PurrOS/SPEC.md` section 1.1).
- **Two uses:** Python **apps**, and a **`python` command in the shell** for quick scripts and
  experiments, like on a Unix machine. Both use the same VM.
- **One Python app at a time.** MicroPython keeps one global interpreter state per process, as far
  as I know, so several isolated VMs are not an option. Starting another Python app stops the
  running one, which can restore from its saved state. Native `.cat` apps still multitask around
  it as usual. Stopping a Python app means restarting the VM, which keeps teardown and the
  out-of-memory killer simple.
- **A Python app is a plain `.mpy` file plus a signed config beside it.** A `.mpy` in the
  MicroPython world is raw bytecode with no signature, so we keep it as it is and put the app's
  details in a separate config (section 2). Standard tools still work on the `.mpy`, and a plain
  `.mpy` can be dropped in while developing.
- **The config is JSON,** the same style as the `.cat` manifest. Hand-edited files, such as themes,
  use the `key = value` style instead.
- **The runtime has its own settings file,** `/etc/mpy.conf`, hand-editable in the `key = value`
  style (section 5).
- **The `python` shell command** warns when a Python app is running, and does not just stop it,
  because it might be an important service (section 4).
- **Apps reach the system through a Python module** generated from the catcall headers, so the
  catcalls and the Python module cannot drift apart.

## 2. The app config

A JSON file beside the `.mpy`, produced by purrstrap. It holds:

- name, version and description
- class and kind, as for `.cat` apps
- the permissions it requests, and the catcalls it needs with minimum versions
- the runtime it needs and the minimum runtime version
- the `.mpy` bytecode format version it was built for
- heap size, and whether it saves state
- the **SHA-256 of the `.mpy`**
- the **signature**, over the whole config including that hash, made with a key of the right role
  (`../Keys/SPEC.md`)

Because the signed config contains the hash, changing either file is caught. Installing, updating,
removing and the app index all treat the pair as one app. AppManager checks that both files are
present, that the hash matches, and that the bytecode version matches the runtime, and installs
both or neither.

## 3. How it fits the runtime interface

- `can_run` checks the runtime version and the bytecode format version.
- `memory_needed` is the heap size from the config, or the default from `/etc/mpy.conf`.
- **The heap lives in PSRAM,** sized at VM start and fixed. The whole VM counts as one unit for
  the out-of-memory killer.
- The lifecycle events (`../AppRuntime/SPEC.md` section 5) map to Python functions the app defines.
- **Only the catcall module is exposed to apps.** MicroPython's own modules for direct hardware,
  files and networking (such as `machine`, `os` and `network`) are not built in for ordinary apps,
  because they would bypass the catcall rule and the permissions. Privileged apps and the shell's
  `python` command are decided in section 7.

## 4. The `python` command

- With no Python app running, `python` opens a prompt (a REPL), and `python <file>` runs a script.
- **If a Python app is running,** `python` prints a warning that names the app and how to stop it
  with `kill`, and does nothing else.
- **Typing `python` again straight afterwards** (within a short window, 30 seconds proposed) stops
  the app and opens a fresh session.
- **`python --force`** does that in one step, for scripts.

## 5. The runtime settings file

`/etc/mpy.conf`, `key = value` lines with comments. Proposed settings:

- the default heap size
- a script to run when the VM starts
- which built-in modules are available
- the interpreter's recursion and stack limits

## 6. Building

purrstrap gets a Python path in its `apps` subscript. It compiles source to `.mpy` with
`mpy-cross` at the version the SDK pins, writes the config with the hash, and signs it. `mpy-cross`
is an external package, so the subscript declares it as a requirement.

## 7. Size and risks

- MicroPython is large. My rough expectation is several hundred KB of flash and tens to over a
  hundred KB of RAM, on top of the heap in PSRAM. These are estimates, not measurements.
- Python is much slower than native code. It suits apps and scripts, not timing-critical work.
- There is no isolation between the Python code and the VM, but the app can only reach the system
  through the catcall module and the permission checks.

## 8. Testing

- Config parsing and the checks: hash mismatch, wrong bytecode version, missing half of the pair.
- The pair installs atomically, so a power cut leaves both files or neither.
- The one-at-a-time rule and the `python` command flow: the warning, the second call inside and
  outside the window, and `--force`.
- Stopping restarts the VM and releases everything the app held.
- Permissions: a Python app gets exactly what a native app with the same permissions gets.

## 9. Open questions

- **The MicroPython version** to pin.
- **Which standard modules** are exposed, and whether asyncio is included.
- **Threads inside an app:** whether `_thread` is allowed, and how it maps to the `sched` cap.
- **Frozen modules** compiled into the runtime.
- **Whether the `python` prompt may use the direct-hardware modules,** since it is the root user's
  tool, and whether privileged Python apps may.
- **Running `.py` source** compiled on the device, or `.mpy` only for apps.
- **Moving the pair between devices** as one unit, for example a small archive, so MTP and the app
  index do not have to treat them as two files.
- **Performance from PSRAM,** and whether the VM's hot parts need to stay in internal memory.
