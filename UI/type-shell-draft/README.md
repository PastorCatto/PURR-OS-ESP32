# The Type shell's design draft (kept here so the shell can be compared with it)

These are the boards of the final design canvas, as published on 2026-11-12 (claude.ai artifact `C7nAzMt4EqurmGMRYuPY7K`, "Playtime launcher, final design"): one `.dc.html`
per screen, and `canvas.json`, the index with each board's position and the notes beside them. They are design-canvas pages (they need that canvas's runtime to be
drawn), so read them as a specification: the sizes, the colours (paper `#f4f1ea`, ink `#14120f`, the accent `#ff5a1f`), the type (Space Grotesk Bold, Space Mono Bold)
and the layout are all in their inline styles. **Every board is the real 320 x 240 screen at 2x**: halve a pixel size to get the device's.

| Board | Screen | Built in |
|---|---|---|
| `HomeV2` (and `HomeV2Playing`) | home: the clock, a notice, the app list tile and three shortcuts | `purr_tp_home.c` |
| `Apps` (and `AppsPlaying`) | the app list with the clock panel | `purr_tp_home.c` (the generic list) |
| `Lock`, `LockPlaying` | the lock screen | `purr_tp_lock.c` |
| `Pin` | the optional PIN | `purr_tp_lock.c` |
| `SignIn`, `Password` | sign in | `purr_tp_lock.c` |
| `Controls` | the button map (A, B, X, Y, FUNC) | reference |
| `Music`, `Main`, `HomePlaying`, `Motion` | the music screen, the first home (four pinned apps) and the motion study | not built (no music app yet); `Main` was replaced by `HomeV2` |

What differs from the drafts on purpose or for now is listed in plan7.md, "The Type shell". The fonts are in UI/fonts/ (licences there).
