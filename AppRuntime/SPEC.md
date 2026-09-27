# AppRuntime spec (draft 0.3)

The part of the system that runs apps. It has two layers: a shared **manager**
that handles multitasking, focus and memory pressure, and **runtime modules**,
each of which knows how to run one kind of app. The first is the basic runtime
for native `.cat` apps. A MicroPython runtime is planned as a second module.

Installing and removing apps is AppManager's job (`../AppManager/SPEC.md`).
Drivers and hardware are the kernel's
(`../PurrOS/components/kernel/SPEC.md`).

## 1. Scope

**In**

- **Phone-style multitasking:** several apps loaded at once, one in the
  foreground, the rest in the background or suspended.
- The **manager**: instances and their states, switching, focus, tracking what
  each app holds, admission, and stopping apps, including when memory runs out.
- The **runtime module interface** that every runtime implements.
- The first runtime: the basic `.cat` runtime (`catrt`).
- The lifecycle callbacks an app implements.
- **Memory-limited app count with an out-of-memory (OOM) killer.**

**Out for now**

- The MicroPython runtime itself. Its own spec comes when the manager and `catrt`
  work. Section 3 only makes room for it.
- Installing, removing and updating apps (AppManager).
- The `ui` catcall design and any graphical shell (separate specs). The
  command-line shell is in AppManager (`../AppManager/SPEC.md` section 8.1).
- Communication between apps, permissions, user accounts, power management.

## 2. Constraints from the rest of the design

- The OS is FreeRTOS in ESP-IDF, so tasks are preemptive and run on both cores.
- Apps use only catcalls (`../AppManager/SPEC.md` section 3). Every service an app
  can use has to be a catcall the manager, a runtime or the kernel provides.
- The first board has no PSRAM. RAM is the scarce resource and decides how many
  apps can be loaded at once.
- **There is no memory protection** on the original ESP32. Tasks share one
  address space. A faulty app can corrupt another app or the OS, and the manager
  can only clean up after it, not contain it.
- The kernel knows hardware only, and CoreOS owns decisions
  (`../PurrOS/SPEC.md` section 2). The OOM policy therefore lives in CoreOS.
- Runtimes are native code, so they are chip-specific modules loaded and updated
  like the other `.kitt` modules (`../PurrOS/SPEC.md` section 6).

## 3. Manager and runtime modules

**The manager** is the common part. It owns the instance table, the states and
transitions (section 4), focus (section 10), admission (section 6), the OOM
killer's registry (section 7), resource tracking and teardown (section 9), and the
scheduling policy (section 11). It does not know how to run any app format.
**It is linked into CoreOS**, not a separate module: it is policy-heavy, has to
be stable, and works closely with CoreOS's memory pressure service.

**A runtime module** knows one app format. It loads an app, runs it, delivers the
lifecycle events, and cleans up what it created. Two are planned:

| Runtime | Runs | Notes |
|---------|------|-------|
| `catrt`, the basic OS runtime | native `.cat` images | First. Chip-specific code, loaded into RAM or run in place. |
| `mpyrt` | MicroPython apps | Later. Bytecode, so an app is independent of the chip. |

Each runtime is a native module (`.kitt` container, role `runtime`). On modular boards it is
a file in `/boot`, and on monolithic boards it is linked into the packed image. CoreOS loads
it through the same loader as AppManager. Adding or updating one is a file operation
(`../PurrOS/SPEC.md` section 6.1), and **the runtimes a device carries are the files
present.**

**Runtime interface**

```
purr_runtime_t {
    abi_version, name,
    kinds[],                     app kinds this runtime runs
    can_run(app_header)          -> ok, or the reason it cannot
    memory_needed(app_header)    -> bytes, for admission
    load(image, instance)        -> ok or error
    start(instance), suspend(instance), resume(instance)
    stop(instance, reason)       tear down what this runtime created
    deliver(instance, event)     run a lifecycle or input event on the app
    stats(instance)              -> memory in use, tasks
}
```

- The manager picks the runtime from the app's header: it names the app kind and
  the minimum runtime version it needs. AppManager checks that the runtime is
  present at install time and refuses the app if not.
- The manager passes each runtime the same host API, so runtimes cannot bypass
  the resource tracking in section 9.
- Every runtime exposes the same catcalls to its apps. `catrt` hands them over as
  `catcall_get`. `mpyrt` would expose them as a Python module generated from the
  same catcall headers, so the two cannot drift apart.

**What differs for MicroPython** (settled in `../MicroPython/SPEC.md`):

- App files are bytecode (`.mpy`), which does not depend on the chip. The runtime
  declares which `.mpy` format version it accepts, and AppManager checks it.
- MicroPython keeps one global interpreter state per process, as far as I know,
  which would rule out several isolated interpreters. Several apps would then
  share one VM and one memory region, without isolation from each other. That
  changes what an "instance" and an OOM victim mean. Needs checking.
- It is large. My rough expectation is several hundred KB of flash and tens to
  over a hundred KB of RAM for the VM's heap. These are estimates, not
  measurements. It probably does not fit the CYD's 4 MB layout next to everything
  else, and suits boards with more flash and PSRAM better.

## 4. App instances and states

Every loaded app is an **instance** with a state:

| State | Meaning | Tasks | RAM |
|-------|---------|-------|-----|
| `starting` | being loaded and created | not running yet | allocated |
| `foreground` | on screen, receives input | running | held |
| `background` | running but not visible | running | held |
| `suspended` | kept in memory, paused | suspended | held |
| `stopped` | gone | deleted | released |

Transitions:

- Launching an app moves the current foreground app to `background` if its header
  allows it to run in the background, otherwise to `suspended`.
- Switching back to a suspended app resumes its tasks and makes it `foreground`.
- Suspended and background apps stay in memory for fast switching, and are the
  first to be stopped when memory runs short (section 7).
- A `stopped` instance is fully released. Nothing survives except what the app
  saved itself.

## 5. App entry and lifecycle callbacks

The `.cat` entry point returns a table the runtime calls. Every call runs on the
app's own event task, so an app is an event loop, like a phone app:

```
purr_app_entry_t {
    abi_version,
    on_create(env, state),   first start: receives purr_app_env_t and the state
                             saved from the last run, or NULL
    on_resume(),             app came to the foreground
    on_pause(),              app left the foreground
    on_save_state(buf, cap), optional: write state, return its length
    on_low_memory(level),    the system is short of memory, free what you can
    on_destroy(reason),      about to be stopped, last chance to save
}
```

- **The reason** for `on_destroy` says why: the user closed it, an update, or
  killed for memory. When killed for memory the app may not get to run it, so
  anything worth keeping is saved earlier, not only at the end.
- **Saved state is opt-in.** An app that supports it sets a flag in its header and
  implements `on_save_state`. The manager calls it whenever the app leaves the
  foreground, and keeps the result (capped at `max_state_size`, default 4 KB) in
  the app's data folder. It is handed back to `on_create` at the next launch. An
  app without the flag has nothing saved and starts fresh. State is saved on
  leaving the foreground, not at kill time, because a kill for memory cannot
  depend on running app code.
- Long work uses more tasks, created through the `sched` catcall (section 8).
  Callbacks must return quickly.
- **Unresponsive apps.** If the foreground app does not handle events for a set
  time, the manager marks it unresponsive and lets the user close it.
- `mpyrt` maps the same five events onto Python callbacks.

## 6. Memory and admission

- **Each instance has a fixed memory block**, sized by the header (data, zeroed
  data, heap, and code too if the app is loaded into RAM). It is allocated when
  the app starts and released when it stops. The block does not grow, so an app
  running out of its own heap only hurts itself.
- Task stacks are counted in the block, and the header declares their sizes.
- **Admission.** Before loading an app, the manager asks its runtime how much
  memory it needs (`memory_needed`) and whether that fits in free memory while
  keeping a reserve for the system. If it does not, it asks CoreOS to free memory
  by stopping the least important apps (section 7), and retries. If it still does
  not fit, the launch fails with that reason and stops nothing in the foreground.
- **Fragmentation.** The block has to be contiguous, so free memory can be enough
  in total and still too fragmented. Blocks are allocated from the largest free
  region, and the reserve is sized to leave room for it. Apps use the shared
  heap, with no dedicated app pool: admission control and a system reserve
  protect the system instead.
- The number of apps loaded at once is not fixed. It is whatever memory allows.
  A static table of instances (`MAX_RUNNING_APPS`, default 8) caps the
  bookkeeping and avoids the heap.

## 7. Out-of-memory killer

**Split of duties.** CoreOS watches memory and decides who is stopped. The manager
does the stopping, through the app's runtime. This keeps decisions in CoreOS and
keeps the dependency direction: the manager registers with CoreOS, and CoreOS
never calls into the manager except through the callbacks it was given.

**CoreOS side (memory pressure service, `../PurrOS/components/coreos/SPEC.md`)**

- Watches free internal memory and the largest free block. Two thresholds with
  hysteresis: `low` and `critical`. It checks when an allocation fails, if the
  platform offers a hook, and on a slow timer.
- At `low` it tells the manager, which sends `on_low_memory` to every app so they
  can drop caches.
- At `critical` it picks a victim and asks the manager to stop it. It repeats
  until free memory is above `critical` plus a margin.
- A minimum interval between kills stops a kill loop.

**Choosing the victim.** The manager keeps a registry that CoreOS can read: for
each instance its importance, size and time of last use. Order of stopping:

1. Suspended apps, then background apps, least recently used first, and among
   equals the largest first.
2. Never: CoreOS, the kernel, AppManager, the manager and runtime modules, other
   system tasks, and any app flagged `persistent`.
3. The foreground app is not stopped to make room for another app. If memory is
   critical with only the foreground app left, that is reported and not resolved
   by killing it.

**Stopping an app** is the teardown in section 9. The reason is recorded as
`killed for memory`, so the UI can tell the user.

**Limits of the killer.** It can only reclaim what apps hold. If the kernel,
drivers or the system itself have taken the memory, it says so in the log and
does not kill innocent apps to compensate.

## 8. Catcalls the manager provides

Apps can only use catcalls, so multitasking needs some of its own. All are to be
designed, each with its own header and doc entry:

- `sched`: create and end tasks, sleep, timers. Everything created through it is
  owned by the calling app. The default cap is 4 threads, and an app can declare more in
  its manifest (`../Catcalls/SPEC.md` section 4).
- `app`: read own info, ask to be closed, post an event to itself.
- `storage`: scoped to the app's own data folder.
- `console`: standard input, output and error for an app (its `io`, bound when the
  shell starts it). By default the foreground instance is connected to the
  console, and a background instance's output goes to the system log with its id
  and name in front. The shell can bind it to a pipe or a file instead. It is the
  first interface apps use for text, before any `ui` exists. Only the instance
  with console focus receives console input.
- `ui`: drawing and input events (separate spec, owned by this layer).

## 9. Resource tracking and teardown

The manager records everything an instance acquires: its tasks, timers, catcall
handles, and its memory block. Stopping an app releases all of it:

1. Mark the instance `stopping` and stop delivering events to it.
2. Wait until it is not inside any catcall. Each instance counts the catcalls it
   is currently in, so a task is never deleted while it holds a system lock or is
   inside a driver. If the count does not reach zero within a timeout, the
   manager logs it and forces the stop.
3. Ask the runtime to `stop` the instance. It suspends, then deletes, the tasks it
   created.
4. Release the catcall handles and timers.
5. Release the memory block.
6. Remove the instance from the registry and report the reason.

Rules that make step 2 workable: catcall implementations must not hold a system
lock across a call back into app code, and use timeouts when they take locks.

## 10. Focus and shared hardware

- **Display and touch:** owned by the foreground app. Input events go to it only.
  A background app's drawing is dropped (the exact rule belongs to the `ui`
  spec).
- **Storage:** shared, with locking in the catcall. Each app sees only its own
  data folder.
- **Keys:** foreground only, except system keys (home, back) which the manager
  handles itself.
- The command-line shell (launcher and task manager commands) lives in AppManager
  (`../AppManager/SPEC.md` section 8.1). It is privileged, not a normal app. The
  manager gives it the instance list and states, and the run, suspend, resume and
  stop calls.
- **Console focus works like Unix job control:** the foreground instance owns
  console input, and `fg` and `bg` change which one. Display and touch focus
  follows the same instance once a `ui` exists. A background instance's console
  output goes to the system log.

## 11. Scheduling

- Priorities map to state: system services highest, then the foreground app, then
  background apps. Suspended apps have no runnable tasks.
- App priorities are always below the system's, so a runaway app cannot starve the
  system, though it can starve lower-priority apps.
- Apps run on the application core by default, so radio and driver work on the
  other core is not disturbed. Configurable.
- Tasks at the same priority share time by the RTOS's normal round robin.

## 12. Relationship to other parts

- **AppManager** supplies the installed image and its header, including which
  runtime it needs. The manager refuses to start an app that is being installed or
  updated, and AppManager asks the manager to stop an app before replacing it.
- **The shell** (command-line launcher and task manager) is part of AppManager
  and calls the manager to list instances, run, switch and stop.
- **Loading** (RAM or in place from flash) is a detail of `catrt`, behind
  `load(image, instance)`. That decision is open in `../AppManager/SPEC.md`
  section 3 and does not change this spec's shape, only the block size and how
  many apps fit.
- **CoreOS** loads the runtime modules, provides the memory pressure service and
  the host API the runtimes use.

## 13. Testing

Host-side, with fake tasks, a fake heap, a fake runtime and fake catcalls:

- The state machine: every transition, and the illegal ones.
- The OOM killer as table-driven scenarios: victim order, protected instances,
  hysteresis, repeated pressure, the minimum interval, launch admission with
  enough and not enough memory.
- Teardown: after stopping, every recorded resource is released, including when
  the app is stuck inside a catcall.
- The runtime interface: a fake second runtime proves the manager does not
  depend on `catrt`, and an app whose runtime is missing is refused.
- Output routing: foreground to the console, background to the log with the
  prefix, and pipes and files bound at launch.
- `on_destroy` reasons.

On the board: a stress app that allocates until it is killed, a switching test
between several test apps, and a measurement of the real free RAM to set the
defaults for the reserve and thresholds.

## 14. Open questions

- **MicroPython on the first board:** does it fit the CYD's flash and RAM at all,
  and can it host several apps in one VM? The MicroPython runtime probably targets
  boards with more flash and PSRAM first.
- **Importance classes:** is the list in section 7 enough, and does an app get to
  declare itself more important?
- **Background rules:** which catcalls a background app may use, and whether it
  needs a permission.
- **Killing a task safely** if catcall counting proves too weak in practice.
- **Sizing** the reserve, thresholds and the default limits from real
  measurements on the board.
- **Saved state details:** whether the default cap of 4 KB is right, whether state
  is also saved on a timer for apps that stay in the background for a long time, and
  what happens to state when an app is updated to a version that does not
  understand it (the app should carry a state version).
- **Shell details** are open in `../AppManager/SPEC.md` (command names, output
  format, console behavior for background apps).
