# PURR OS export — pre-Windows-reinstall dump

Generated 2026-07-11. This folder is a self-contained snapshot meant to
survive being zipped up and moved off this machine before a Windows
reinstall. It captures things that live in this environment but NOT
necessarily in git history or anywhere else durable: local config, the
toolchain setup, in-progress task state, and hard-won technical discoveries
from this session that aren't written down anywhere in the codebase itself.

Since the whole project folder is being zipped (not just this export/
folder), the git history, all tracked files, and currently-untracked files
(including `CatReleases/`) travel with it automatically — nothing there is
at risk. This folder exists for the stuff that's easy to forget or
hard to reconstruct: exact local paths, the toolchain version, and the
mental model built up over this session's debugging.

## Contents

- **01-task-list.md** — exact open/in-progress task state at export time.
- **02-memory.md** — the assistant's persistent cross-session memory for
  this project (user preferences, project facts, deferred decisions).
- **03-session-summary.md** — what was just built/fixed/committed, and
  what's queued next, in narrative form.
- **04-technical-quirks.md** — MiniWin internals and other gotchas
  discovered by reading source this session — not obvious from the code
  alone, easy to re-waste time rediscovering.
- **05-environment-setup.md** — toolchain versions, local paths, device
  config, and anything needed to get a fresh machine building/flashing
  again.

## Fastest path to resuming work on a new machine

1. Unzip the project folder anywhere.
2. Read `05-environment-setup.md` and reinstall the ESP-IDF version /
   Python deps listed there.
3. Read `01-task-list.md` + `03-session-summary.md` to know exactly where
   things were left off.
4. Read `04-technical-quirks.md` before touching MiniWin/WinCE desktop code
   again — it'll save re-deriving things already figured out the hard way.
