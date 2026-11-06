# Showtime Plan 1: the SDK ready for showtime, the whole repository documented, and a human front page

> **A manually approved addition to the plan series (owner, 2026-11-05).** plan11 was already an override of the "stop at plan10" rule; this is a second one, named by the owner ("Showtime Plan 1"). It is written down and **waiting for the owner's keyword**; nothing in it has been started.

## Status

| Part | What | State |
|---|---|---|
| A | The SDK up to spec (kit 1.1.0), with the emulator built into it | **started 2026-11-05 (the keyword was given)** |
| B | Document the whole repository (not `archive/`): at least 100 pages, with hyperlinks to everything | not started; follows Part A |
| C | The front README, opening with a quote from the owner | a first version is written (uncommitted); the quote slot is open |
| D | Custom fonts loaded from TTF files, and fonts as Store assets | designed here; follows Part A |
| E | The Windows 10 Mobile shell at feature parity with the Android shell (Lumia and Windows Phone 8 design language), with a Windows 10 style user screen and the list at the side | written down; parity table first |

Order: A, then B, then C. C needs only the owner's words, so it can be done at any point once they are given.

## What the owner said (2026-11-05, kept close to the words)

- "We need to get the SDK up to spec. The emulator is kind of built, and we have a standalone version, so check the conversations with the other Claude. Also, maybe if you need to talk to it, you can. They're both online, I should say, unless it's not. If it's not online, just read the conversations. But definitely integrated into the SDK. Get the SDK ready for showtime."
- "Then I need you to go through the entire repository in great detail, minus the archive, and document everything. I'm looking for a minimum of like 100 pages worth of ... great details and everything, with hyperlinks to everything."
- "And then the front README ... put a little quote on the front of it from me, just kind of introducing it, so that way it's actually more human instead of AI slop. But I'll give you that part later."

## Rules that apply to all three parts

- `archive/` is out. So are build output (`out/`, build dirs), `managed_components/`, and vendored third-party code (it is listed with its licence in `THIRD_PARTY.md`, not documented line by line).
- **Nothing private goes into a document:** no passphrase, no private key material, no device addresses or hardware identifiers, no private hardware detail. `signing_keys/` is described by role and by what it is for, never by content. The `Partner-*` folders stay out until the owner says they are public.
- A claim in the documentation is checked against the code or a test, not remembered. What could not be checked is marked "not verified on a device" in the text, the way the plans do it.
- If a plan and the code disagree, one of them is fixed in the same step (CLAUDE.md). The documentation never papers over a difference: it says so and links the plan.
- The emulator is only run with the owner's say-so, and a live emulator window is never killed (`pgrep` first). Whether the keyword covers running it for Part A is the first question to ask at the keyword.
- Commits are split into feature chunks. The signing passphrase is scanned for before every commit.

## Part A: the SDK up to spec, with the emulator inside it

### Where things stand (read 2026-11-05)

- `sdk/` holds the library `purrsdk`, the `purr.py` command line, the `studio/` web app, `boards/`, `docs/`, `tests/`, `tools/` and `build-kit.py`. The design is `sdk/DESIGN.md`; the plan is `plan5.md` (status table and session log). The built kit has been `purr-sdk-0.1.0-macos-x86_64`.
- Verified in the past: the C host tests, 74 SDK tests, the bundled Python. Not verified on a device: GPS, LoRa, modem, Wi-Fi AT, HCI and trackball drivers (the GPS driver is pulled from the dev server as of today, see plan9), the `gps`, `radio` and `modem` shell commands, branding in the shell.
- The emulator hook in the SDK is only a hook (`purr run` / `PURR_EMULATOR`). The emulator itself lives in `~/Projects/ESP32-Emulator`: a Rust application around Espressif's QEMU fork (ESP32-S3 only), the display, touch, keyboard and SD card as Rust devices over the `vpb` socket protocol, boards as TOML files (`t-deck-plus.toml`, `t-deck-plus-vm.toml`, the CYD boards, `generic-esp32s3.toml`, `pocketbyte.toml`), a W5500 Ethernet model for the VM board (guest networking works through QEMU's user-mode network), a standalone version, and docs (`architecture.md`, `board-files.md`, `custom-hardware.md`, `vm-hardware.md`). Its own hand-off note is `OS handoff.md`. Four session transcripts are under `~/.claude/projects/-Users-pastorcatto-Projects-ESP32-Emulator/`.
- Sessions reachable now: `esp32-emulator-59` (the emulator's Claude, online and idle) and `purr-os-esp32-4e` (another session of this project, online and idle). The rule: read the transcripts and docs first; message the emulator session only for a specific question that the transcripts do not answer (an interface to agree, a build step that fails), and say in the plan what was asked and answered.

### Steps

1. **Inventory against the spec.** List what plan5 and `sdk/DESIGN.md` promise, what exists, what is tested, what is only claimed. Output: a table in plan5 (this replaces the status table's guesses with checked facts).
2. **Read the emulator side.** The four transcripts, `README.md`, `docs/*.md`, `OS handoff.md`, the board files. Output: a short "what the emulator offers today and what it does not" section in `sdk/docs/` and a corrected "the emulator as the second device" section in plan11 (partly done: the networking facts are in).
3. **Agree the interface** with the emulator session if the transcripts leave it open: how the SDK finds and starts the emulator, which board file a project selects, how a built app gets into the VM (the app loader, a flash image, the dev server), where the screen and serial output go, how a test run is scripted (headless key presses and a screenshot already exist).
4. **Bring the standalone emulator into the kit.** Build or fetch it into `out/sdk-cache` like the other pieces, put it in the kit, record its licences (QEMU is GPL; the fork's source offer; the Rust crates) in the kit's notices and in `THIRD_PARTY.md`. The kit stays self-contained: no step that needs a compiler or a download on the developer's machine.
5. **`purr run`, end to end.** Create an app from a template, build it, sign it with the developer key, start it in the emulator on the chosen board, show the result (a screenshot and the log), and stop the emulator cleanly. A test drives this headless.
6. **The rest of the spec** that step 1 finds missing or untested: the commands, the Studio app's pages, the board and brand files, the templates, the error messages (friendly, per the owner's earlier brief).
7. **Docs and examples** inside `sdk/docs/`: a first-hour guide (install the kit, write an app, run it in the emulator, publish it to the dev server, install it on a real device), a reference for the commands and the catcalls (ui v3 widgets, `files`, `system` v2), and the Photos, Clock and Files apps as annotated examples.
8. **Build and check the kit** on a clean path, run every SDK test (with the IDF Python for the cryptography ones), and walk the first-hour guide as a stranger would.
9. **Update plan5, plan11 and `THIRD_PARTY.md`** in the same pass.

**Done when:** on a machine with nothing installed, the kit alone takes someone from "new project" to "my app is running in the emulator" and on to "installed on a T-Deck Plus through the dev server", every step in the guide has been run, and the tests pass.

## Part B: the repository documented, at least 100 pages, everything linked

### What "100 pages" is taken to mean

About 500 words a page, so **at least 50,000 words** of real content, plus tables that are not counted in that figure. The aim is 60,000 or more. A small script counts the words per chapter and reports the total, so the measure is a number and not a feeling.

### Layout

A `docs/` tree in Markdown (it renders on GitHub and in an editor, and can be turned into one PDF later if the owner wants one):

- `docs/README.md`: the index and a map of the repository. Every chapter is linked from here.
- One chapter per area, in this order: the boot chain (bootloader, boot package, KittenOS, recovery loader, kernel), partitions and the flash map, the trust model and the key roles, the kernel and its services, the catcall ABI, the app runtime and the sandbox, the driver bus and the drivers, the UI stack and the three shells, the picture pack, networking and Wi-Fi, the Store and repositories, power, updates and release channels, the build and signing tool (`purrstrap`), the host tests, the apps (one page each), the SDK, the emulator and the VM, the boards, the tools and scripts, and a glossary.
- Each chapter uses the same shape: what it is for; the files that make it up (a table, each file a link); how the data and control flow; the interfaces other code depends on; how to build and test it; what is known to be wrong or unverified (each linked to the plan that tracks it); where to read next.
- **Generated parts stay true:** the per-folder file tables and the catcall/driver-bus interface listings are produced by a script from the source, so they cannot drift. The narrative is written by hand from reading the code.

### Links

- Every file, function and plan mentioned is a relative link. A function links to its definition with a line anchor only when a script wrote the link (so it can be regenerated); hand-written links go to files and sections.
- `tools/docs/check_links.py` (to write) fails on any broken relative link or missing anchor. It runs at the end of every chapter and at the end of the job.

### Method

Area by area in the order above. For each: read the plan, read the code, build the file table, write the chapter, check every statement that is not obvious, run the link check, commit that chapter. The job is far bigger than one sitting: progress is recorded in this file after each chapter (a table of chapter, words, links checked), so a new session can pick it up. At most two helpers write in parallel (three only if there is no other way), as set out below.

**Done when:** every area has its chapter, the word count is at least 50,000, the link checker is clean, nothing outside the rules above is in the text, and the index reaches every chapter.

## Part C: the front README

- The top of `README.md` opens with a short quote in the owner's own words, a few sentences introducing the project in a human voice. **The owner writes or dictates it; it is not drafted for them.** Until it arrives, the place is held open and the README is not published with a made-up quote.
- Under it, in plain language and without the usual boilerplate: what PURR OS is, who it is for, what works today and what does not (honestly), a short way to start (the SDK first-hour guide), and the map into `docs/`.
- No marketing adjectives; short sentences; the same tone as the plans.

## Part D: custom fonts, loaded from files (added by the owner, 2026-11-05)

**What the owner asked for:** run a font straight from a TTF file on the file system, so that fonts are not compiled in, the emoji can be updated by swapping a file, and people can add custom fonts through the Store.

**What was checked (not built yet):**
- LVGL 9.2.2, the version in use, has Tiny TTF in `managed_components/lvgl__lvgl/src/libs/tiny_ttf/`. `lv_tiny_ttf_create_file(path, size)` reads a file (it needs an LVGL file-system driver and `LV_TINY_TTF_FILE_SUPPORT`); `lv_tiny_ttf_create_data(data, len, size)` takes bytes already in memory. The second one suits PURR OS: the OS reads the file through its own file system into PSRAM and hands over the bytes, so no extra driver is needed. `lv_tiny_ttf_set_size()` resizes a font; a glyph cache (`LV_TINY_TTF_CACHE_GLYPH_CNT`) sets the RAM it uses.
- It is **off in our build** (`CONFIG_LV_USE_TINY_TTF` is not set). Switching it on adds code to the kernel; the kernel image has about 90 KB left in its slot (1,876,112 of 1,966,080 bytes at 1.0.26), so the cost has to be measured first.
- Tiny TTF is built on `stb_truetype`, which draws **outline glyphs only**. It cannot draw colour emoji fonts (the bitmap and colour-layer tables such as CBDT and COLR). So a TTF gives text fonts and **monochrome emoji** (tinted by the text colour). Colour emoji would come from LVGL's image font (`LV_USE_IMGFONT`, also off) with small pictures in the picture pack. Both can sit behind the same fallback chain.

**Design to settle at the keyword:**
1. **Where fonts live.** A read-only system folder for the fonts that ship, and a per-user folder for installed ones. Fonts are a kind of asset in the repository format (`Repositories/SPEC.md` already lists fonts among the "assets and extras" the Store's companion installs to flash or to an SD card), so the Store gets a Fonts category with no new mechanism: signed index, size and SHA-256, the licence shown before install.
2. **A fallback chain.** The font a theme asks for, then the emoji font, then the built-in Montserrat, so a missing glyph never shows as a blank. Settings picks the system font; an app can ask for a font by name through a new version of the `ui` catcall.
3. **Untrusted files.** A font is parsed by C code from a file somebody else made. Mitigations to build: a size cap, a header and table check before the file is ever handed to the renderer, a limit on the glyph cache, and the font only ever loaded by the UI service (never by an app). A font that fails the check is refused with a message.
4. **Licences.** Each font entry carries its licence name; the Store shows it. Fonts that ship with the system are listed in `THIRD_PARTY.md`.
5. **Tools and docs.** `purr font check` and `purr font add` in the SDK (validate a TTF, report the glyph coverage and the licence, package it as a repository asset), a Fonts chapter in `docs/`, and the first-hour guide gets a "change the system font" step.

**Done when:** a TTF placed on the device (or installed from the Store) can be chosen in Settings and is used by the shell and by apps; replacing the emoji font file changes the emoji with no rebuild; a damaged font file is refused and the shell keeps working; the kernel still fits its slot with the option on; the measured cost is written in the plan.

## Part E: the Windows 10 Mobile shell brought to feature parity with the Android shell, in the Lumia and Windows Phone 8 design language (added by the owner, 2026-11-05)

**What the owner asked for:** the Windows 10 Mobile shell should have the same features as the Android shell, designed in the language Microsoft used on Lumia and Windows Phone 8 (flat tiles, big light type, the accent colour, tilt and slide motion), and "a user screen like Windows 10 that has the list on the side". The first line of the request said "version 4" (the `ui` catcall version 4, added with the Messages work). The standing art rule applies: the design language is recreated, with original art; no Microsoft imagery or icons are added.

**Where the two shells differ today (to be checked and completed in the first step, from the code):** the Android shell has notifications on the lock screen and a notification shade with a list, Messages and Contacts looks drawn for it, the Froyo icon set, and its sign-in screen; the Windows 10 Mobile shell has tiles, the sign-in screen of `purr_w10m.c`, its own Settings pages and the app list.

1. **A parity table** of every feature of the Android shell against the Windows 10 Mobile shell, built by reading `purr_android*.c` and `purr_w10m*.c`: lock screen, notifications (lock screen and shade), sign-in and user switching, the app list and pinning, the task switcher, the status area, the `ui` widgets of versions 1 to 4, permission prompts, and the Settings pages. Each row says present, missing or different.
2. **Close the gaps in the Lumia style:** notifications on the lock screen (a short list under the clock, as Windows Phone 8 shows them), the version 4 `bubble` drawn flat in the accent colour with the theme's light or dark setting, and whatever else the table shows missing.
3. **The user screen.** Windows 10's sign-in screen shows the chosen account in the middle and the **list of accounts down the side**, so another account is one tap away. The Windows 10 Mobile shell gets that layout, keeping its rules (an account list, a password, lock-out after repeated failures, the default account note from the MEOW change). The Android shell keeps its own sign-in.
4. **Each piece is checked as the rest of the UI is:** a render first (the host LVGL renderer in `tools/lvgl_host/`), shown to the owner, then built. The VM runs the Windows 10 Mobile shell, so it is tested there headless, with screenshots.

**Done when:** the parity table has no "missing" row that the owner wants kept, the lock-screen notifications and the version 4 widgets look right in the Lumia style, and the side-list user screen has been shown as a render, built, and seen working in the VM with at least two accounts.

## The apps repository (the owner is creating it)

The owner will make a **new repository for the apps** and then give this session access to it (and to the other Claude) so the work can be finished there. It is the "separate repository that holds only apps" that `Repositories/SPEC.md` describes: pre-compiled packages, each linking to its source, with a signed `repo.json` and `index.json`. Until it exists nothing is moved. When access is given, the order is: list what belongs in it (the apps in `Apps/`, the new Messages and Contacts apps once written), build and sign each with the developer key, publish the packages and the index, check every file by size and hash from a fresh download, point the Store's default repository at it, and update plan8 and the documentation. Keys and passphrases are never written into that repository either.

## The owner's answers (2026-11-05) and what they change

1. **Emulator and the other session: yes.** The SDK has a run button, so Part A runs the emulator and may message the emulator session (`esp32-emulator-59`) for specific questions. A live emulator window of the owner's is still never killed (`pgrep` first).
2. **Helpers: two at a time, three only if there is no other way.** Credits are the limit. Rules that follow: the generated parts (file tables, interface listings) come from scripts, not from helpers; helpers get one chapter each with the chapter template and the rules above; the cheaper model is used for helpers where the work is tables and file reading, and the main session writes and reviews the chapters that need judgement (boot chain, trust model, runtime); each helper's result is read and link-checked before it is kept.
3. **Markdown only.** No PDF.
4. **The kit is version 1.1.0.** 1.1 is the general release version, the one that counts as stable; the numbers below it (1.0.x) are the patch notes on the way there. The owner does not mind the exact number. The kit's version string, `build-kit.py` and the kit's folder name change to 1.1.0 in Part A.
5. **The `Partner-*` folders: out of the documentation for now** (owner, 2026-11-05), until the owner says they are public.

### How the documentation is written (owner's brief)

The aim: if AI ever disappears, the owner can still carry on developing from the documents alone. Easy to read, including for people with ADHD, and clear and to the point.

- **Each chapter starts with a "short version"**: four or five lines or bullets saying what the thing is, where its files are, and the one command or fact someone most needs. Someone who reads only that still gets the point.
- **Short sections, one idea each**, with plain headings, so a reader can find their place again. Tables for lists of files, settings, states and commands. Numbered steps for anything done in order. Bold only for the thing that must not be missed.
- **Commands are written out in full and can be copied**, with what they print when they work and what it means when they do not.
- **The same shape in every chapter**, so the reader always knows where to look: short version, what it is for, the files (linked), how it works, how to build and test it, what goes wrong and how to fix it, what is not finished (linked to the plan).
- **"Why" is written down next to "what"**, because decisions are the part nobody can rediscover. Where a decision came from the owner, say so with the date.
- **A runbook chapter that moves working knowledge out of people's heads and chat notes and into the repository:** the build directories and what each is for, the order for building and signing a release (with the passphrase supplied in the environment of one command, never written), publishing to the dev server and verifying it, flashing and debugging a T-Deck, the traps that have already cost time (stale `sdkconfig` in an existing build directory, zsh word splitting, opening the serial port resets the board, apps cannot run on PSRAM stacks, baked-in drivers are planted every start). Much of this lives today only in the assistant's memory notes; Part B copies the substance into `docs/` so it survives.
- **A "start here" page** with three paths: "I want to build and flash it", "I want to write an app", "I want to understand how it boots".
- **A glossary** of every PURR term (catcall, kittenos, bootpkg, uiassets, JumpingJaguar, withdrawn, and so on) with a link to where it is used.
- **Honest status:** what works, what has only been compiled, what has run on a device. No marketing words.

## Progress log

- 2026-11-05: written; the two online sessions were identified; nothing started. The owner's keyword and the README quote are still to come.
- 2026-11-05 (later): the owner answered the questions (above): emulator and messaging covered, two helpers, Markdown only, kit 1.1.0, documentation written for easy reading and for carrying on without AI. Still waiting for the keyword.
- 2026-11-05 (later still): the owner confirmed the `Partner-*` folders stay out of the documentation. All five questions are answered; only the keyword is missing.
- 2026-11-05 (evening): the owner added Part D (custom fonts from files, fonts in the Store) and said the apps will move to a new repository that Claude will be given access to. A first README was written (Part C) and is waiting for review; the quote slot is empty on purpose.
- 2026-11-05 (night): the owner gave the keyword. **Showtime starts before the key rotation**; the rotation (plan3, due 7 November) is done after it, at the owner's choice. Part A is started.
- 2026-11-05 (night): the owner added Part E (the Windows 10 Mobile shell at ui version 4, and a Windows 10 style user screen with the account list at the side). The Messages work (not part of this plan) ran first at the owner's request.

