# Glossary

Terms as this repository uses them. Status tags as in the [README](README.md).

| Term | Meaning |
|------|---------|
| **ABI version** | A number in each call table. A module built for a different number is refused. Core table 4, kernel table 6 ([21](21-reference-formats-abis-limits.md#call-tables)). |
| **admin** | An account role that may add, remove and change accounts and use `su`. The first account is always an admin ([07](07-user-accounts.md)). |
| **app (`.cat`)** | A signed package, `image_type` 4, installed under `/home/<user>/apps`. Cannot run yet ([11](11-apps.md)). |
| **AppManager** | The storage and registry code for apps (`purr_appmgr.c`). In the specs also a separate system module; today it is linked in. |
| **auto mode** | The recovery loader's unattended mode, used when the failure ladder reaches the loader ([10](10-updates-network-install-and-recovery.md)). |
| **board** | A concrete device (`tdeck_plus`): chip, pins, partition table. Chosen by `PURR_BOARD` ([16](16-adding-a-board.md)). |
| **boot package (bootpkg)** | A small signed program the bootloader loads into SRAM to draw the boot menu ([05](05-boot-process-and-flash-layout.md)). |
| **bootloader** | The PURR second-stage bootloader in `bootloader/`, flashed at `0x0`. Not the same as the stock bootloader `PurrOS/` builds ([04](04-building-and-flashing.md)). |
| **catcall** | A versioned interface apps use to reach the system. Only the display one exists as a struct. |
| **component** | A named updatable piece: `kernel`, `kittenos`, `loader`, `bootpkg`, `coreos`, `appmanager`, `runtime`, `devbundle`. |
| **container** | The 173-byte PURR image header plus payload ([21](21-reference-formats-abis-limits.md#image-container)). |
| **CoreOS** | The shared platform code in `PurrOS/components/coreos`: verification, key bag, accounts, shell engine, app manager core. |
| **developer key** | A signing role for third-party apps and command modules. Key id 2 in this repo. |
| **devices bundle (devbundle)** | The planned signed bundle of board description and drivers. **[DESIGNED]** |
| **eFuse** | One-time-programmable chip fuses. The project reads the Secure Boot fuse and never burns anything. |
| **file-staged component** | CoreOS, AppManager, runtimes, devices bundle: updated by staging a file and swapping it in KittenOS ([10](10-updates-network-install-and-recovery.md)). |
| **floor / version floor** | The lowest version of a component the device accepts; rises when an update is confirmed. |
| **flag (purrcfg)** | A bit in `purrcfg.flags`. One-shot flags are cleared before they are acted on ([05](05-boot-process-and-flash-layout.md)). |
| **full / recovery / minimal** | The three build profiles: PURR OS, KittenOS, the recovery loader. |
| **handoff** | A struct for passing boot state from the bootloader to the OS. Defined, unused ([F-05](FINDINGS.md#f-05)). |
| **kernel** | The hardware and filesystem layer. A partition in the design; today the same code is linked into the monolith. Also the name of the partition and of the `Kernel/` project. |
| **kernelmod** | A signed relocatable file in `/kernelmods` that adds commands, calling the kernel table ([12](12-writing-modules.md)). |
| **kernel table** | `purr_kernel_table_t`, the function-pointer table kernelmods call. |
| **KittenOS** | The recovery system: the `recovery` profile. It applies staged updates and formats a blank filesystem. |
| **ladder** | The boot-failure escalation: kernel, then KittenOS, then the recovery loader ([05](05-boot-process-and-flash-layout.md)). |
| **loader (recovery loader, PURR Loader)** | The `minimal` profile in the `loader` partition: Wi-Fi, TLS, key bag, flash writer. |
| **LittleFS** | The power-loss-safe filesystem on `root` ([09](09-filesystem.md)). |
| **manifest** | A flat text list of components with sizes and hashes. The recovery manifest and the module index ([10](10-updates-network-install-and-recovery.md)). |
| **modular board** | A board with enough flash and PSRAM to load code from files. The only supported tier. |
| **module** | A signed relocatable file in `/system` that adds commands, calling the core table ([12](12-writing-modules.md)). |
| **monolith** | The one image that has the kernel, CoreOS, shell and module loader linked together. What actually runs. |
| **monolithic board** | A board without PSRAM, planned to run one packed image. **[DESIGNED]**, no code. |
| **netrec** | The raw partition holding the last Wi-Fi network for the recovery loader ([08](08-networking.md)). |
| **owner key** | The signing role reserved for the project owner (`superuser` apps). **[DESIGNED]** |
| **payload** | The part of a container after the header. For installed partitions only the payload is written. |
| **PSRAM** | External RAM on the T-Deck Plus (8 MB). Modules and downloads live there. |
| **purrcfg** | The A/B boot configuration sectors ([05](05-boot-process-and-flash-layout.md)). |
| **purrstrap** | The build and packaging tool ([22](22-purrstrap-reference.md)). |
| **quarantine** | Moving a rejected module to `/system/.rejected/` so it is not retried ([12](12-writing-modules.md)). |
| **relocation** | Adding a load address to a stored word so a module can run wherever it lands ([21](21-reference-formats-abis-limits.md#module-payload)). |
| **role** | The kind of file a key may sign: boot, system, owner, developer, vendor ([17](17-keys-and-signing.md)). |
| **root (filesystem)** | The LittleFS partition `root`. Not to be confused with **root (account)**, the session `su` gives an admin. |
| **secure mode** | `purrcfg.secure_mode`: off, warn, enforce ([17](17-keys-and-signing.md)). |
| **slot** | One of the three app partitions the bootloader can start: `kernel`, `kittenos`, `loader`. Also a display instance in [15](15-adding-a-second-display.md). |
| **stage 2** | KittenOS installing `kernel` and the modules after the loader restored it ([10](10-updates-network-install-and-recovery.md)). |
| **staged / swap** | Putting a new file in `/boot` as `.new`, then having KittenOS rename it into place ([10](10-updates-network-install-and-recovery.md)). |
| **system key** | The signing role for AppManager, runtimes and devices bundle. None exists in the key bag yet. |
| **tier** | Modular or monolithic. |
| **tee display** | A display object that forwards to two panels ([15](15-adding-a-second-display.md)). |
| **vendor key / certificate** | The planned way to trust a third party's modules. **[DESIGNED]** |
