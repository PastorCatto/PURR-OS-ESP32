# PURR OS documentation

## Short version

- Twenty chapters, one per area, plus a start-here page, a runbook and a glossary. Every chapter opens with a short version.
- New here? Read [start-here.md](start-here.md). Carrying on the work? Read the [runbook](runbook.md). A word you do not know? [glossary.md](glossary.md).
- The plans in the repository root (`plan1.md` to `plan10.md`, and `plan11.md` and `showtime1.md` by the owner's override) say what is done and what is not. When a chapter and a plan disagree, the plan is the source of truth and the chapter is the bug.
- Every chapter says what was **not verified on a device**. Nothing here is claimed to work unless the code or a test shows it.

## Where to start

| You want to | Read |
|-------------|------|
| build and flash the system | [start-here.md](start-here.md), then the [runbook](runbook.md) |
| write an app | [start-here.md](start-here.md), then [17-sdk.md](17-sdk.md) and [sdk/docs/13-first-hour.md](../sdk/docs/13-first-hour.md) |
| understand how it boots | [01-boot-chain.md](01-boot-chain.md) |
| find a word | [glossary.md](glossary.md) |

## Chapters

| # | Chapter | What it covers | Words |
|---|---------|----------------|-------|
| 01 | [The boot chain](01-boot-chain.md) | bootloader, boot package, KittenOS, loader, kernel, in the order they run | 1,246 |
| 02 | [The flash map](02-flash-map-and-partitions.md) | where every component sits in flash, and the partition table | 1,253 |
| 03 | [Trust, keys and signing](03-trust-and-keys.md) | key roles, the trust block, who may sign what | 1,353 |
| 04 | [The kernel and its services](04-kernel-and-services.md) | what runs after the boot menu | 1,514 |
| 05 | [Catcalls and the app ABI](05-catcall-abi.md) | the calls between apps and the system | 1,461 |
| 06 | [The app runtime](06-app-runtime.md) | loading, limits, permissions, the sandbox | 1,407 |
| 07 | [Drivers and the driver bus](07-drivers.md) | modules that talk to hardware | 1,327 |
| 08 | [Updates, releases and channels](08-updates-and-release-channels.md) | manifests, the official releases, rollback rules, withdrawing a release | 1,228 |
| 09 | [purrstrap](09-purrstrap.md) | the build and signing tool | 1,351 |
| 10 | [Host tests](10-host-tests.md) | the tests that run without a device | 1,246 |
| 11 | [The UI stack and the shells](11-ui-stack-and-shells.md) | LVGL, the UI service, the three shells | 1,326 |
| 12 | [The picture pack and fonts](12-picture-pack-and-fonts.md) | the images the shells draw; fonts as planned work | 1,134 |
| 13 | [Networking](13-networking.md) | Wi-Fi, the VM's wired network, the dev server | 1,123 |
| 14 | [The Store and repositories](14-store-and-repositories.md) | signed indexes, the Store, the companion | 1,137 |
| 15 | [Power](15-power.md) | screen timeout, sleep, battery | 1,029 |
| 16 | [The apps](16-apps.md) | one section per app | 1,312 |
| 17 | [The SDK](17-sdk.md) | the developer kit | 1,138 |
| 18 | [The emulator and the VM](18-emulator-and-vm.md) | running the system without a device | 1,154 |
| 19 | [Boards](19-boards.md) | board files and device profiles | 1,166 |
| 20 | [Tools and scripts](20-tools-and-scripts.md) | what each script in the repository does | 999 |

Also here: [start-here.md](start-here.md), [runbook.md](runbook.md), [glossary.md](glossary.md).

## A map of the repository

| Folder | What is in it | Chapter |
|--------|---------------|---------|
| [PurrOS/](../PurrOS/) | the firmware: kernel, core services, shells, app runtime | [04](04-kernel-and-services.md), [11](11-ui-stack-and-shells.md) |
| [bootloader/](../bootloader/), [bootpkg/](../bootpkg/) | the second-stage bootloader and the boot package | [01](01-boot-chain.md), [02](02-flash-map-and-partitions.md) |
| [purrstrap/](../purrstrap/) | the build, packaging and signing tool and the dev server | [09](09-purrstrap.md) |
| [Apps/](../Apps/), [Modules/](../Modules/) | the sources of apps, system modules and drivers | [16](16-apps.md), [07](07-drivers.md) |
| [sdk/](../sdk/) | the developer kit | [17](17-sdk.md) |
| [UI/](../UI/) | the interface specification and artwork | [11](11-ui-stack-and-shells.md), [12](12-picture-pack-and-fonts.md) |
| [tools/](../tools/) | release, hardware, VM and documentation scripts | [20](20-tools-and-scripts.md) |
| [Boards/](../Boards/) | board descriptions | [19](19-boards.md) |
| [Keys/](../Keys/), [Permissions/](../Permissions/), [Users/](../Users/) | specifications for keys, permissions and accounts | [03](03-trust-and-keys.md), [06](06-app-runtime.md) |
| [OTA/](../OTA/), [Install/](../Install/), [Repositories/](../Repositories/) | updates, installing, repositories | [08](08-updates-and-release-channels.md), [14](14-store-and-repositories.md) |
| `signing_keys/` | the signing keys (described by role only, never by content) | [03](03-trust-and-keys.md) |
| `out/` | build output (not in git) | [runbook](runbook.md) |

## How these pages are kept true

- Link check: `python3 tools/docs/check_links.py docs` fails on any broken relative link or anchor.
- Word count: `python3 tools/docs/count_words.py docs` (tables and code are not counted).
- Where a plan or specification and the code disagreed while these pages were written, each chapter says so in its "what is not finished" section, and the list is in [showtime1.md](../showtime1.md).
- Nothing private is written here: no passphrases, key material, device addresses or hardware identifiers.
