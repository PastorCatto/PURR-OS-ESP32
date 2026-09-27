# Drivers spec (draft 0.1)

How drivers are packaged, distributed and pinned. How drivers work inside the kernel (matching, the pin
registry, the catcalls) is in `../PurrOS/components/kernel/SPEC.md`. Finding hardware and pulling drivers
for an unknown board is in `../Install/SPEC.md`. Keys are in `../Keys/SPEC.md`.

## 1. Decisions so far

- **Two forms of driver distribution.**
  - An **individual driver** is a signed file that can be pulled on its own and updated without touching
    anything else. Discovery uses these for add-ons and unknown boards.
  - A **driver pack** is for a pre-supported device, and sets up a whole board.
- **A pack is a signed list, not one big file.** It holds the **board description** and, for each driver,
  its name, its **pinned version and hash,** and its **pin assignments.** It is signed with a release key.
- **The pinout adapts to the board.** Drivers never hard-code pins. The pack gives each driver the pins it
  uses on that board, so the same driver file serves different boards with different wiring.
- **Core drivers stay locked.** Because the signed list pins each driver's exact hash, a swapped or
  modified driver is caught.
- **Drivers are files in `/boot/drivers`.** A driver several boards share is stored once and can be updated
  on its own.
- **Data-description drivers and code drivers.** A driver is a data description when it is one device on one
  bus plus its own pins, and needs real code otherwise (`../PurrOS/components/kernel/SPEC.md` section 14).
- **Users can add drivers** from an SD card, through the individual path, under the same rules.

## 2. The pack

A signed manifest, made by purrstrap. It contains:

- the board's codename and its hardware description (buses, devices, which driver serves each)
- for each driver: name, version, **SHA-256 of the driver file,** and its **pins** for this board
- the minimum kernel version it needs

The device downloads each driver file separately, checks it against the pinned hash, and keeps it in
`/boot/drivers`. If the manifest's signature is wrong, or a file's hash does not match, the driver is not
used.

## 3. Individual drivers

- A signed file with its own header: name, version, the `compatible` strings it serves, the buses it needs,
  and the catcall it provides.
- **Who signs it** decides where it may be used: the system role for core drivers, a vendor certificate for a
  vendor's driver (for example a mesh radio), the developer role for a user's code driver, and no signature
  needed for a data description that only uses pins no locked driver owns (`../Keys/SPEC.md`).
- An index in the repo lists each driver with its version, hash, signer, size and requirements, so the
  device can look one up when discovery finds a matching device.

## 4. How it fits the kernel

- The kernel matches each device in the board description to a driver by its `compatible` string and hands
  it the pack's pins. A device with no driver ends in the `no_driver` state, and the installer fetches one.
- The pin registry still refuses any conflicting claim, including one from a pack entry
  (`../PurrOS/components/kernel/SPEC.md` section 6).
- **Code drivers need loadable native modules,** which depend on the module loading spike
  (`../ModuleSpike/SPEC.md`). Until it passes, code drivers are built into the kernel and only data-description
  drivers are separate files.

## 5. Testing

- Pack checks: a wrong signature, a driver file whose hash does not match, a swapped driver, and a missing
  driver.
- Adaptable pinout: the same driver file on two boards with different pin maps.
- A driver shared by two packs is stored once and updated once.
- Pulling an individual driver on demand, and using the cached one when offline.
- A pack entry that asks for a pin a locked driver owns is refused.

## 6. Open questions

- **The file format for code drivers,** and how they are built per chip. The old system had a driver build
  tool, and purrstrap would get a subscript for it.
- **Where the drivers live** in the repo, and whether they have their own repo.
- **The pack's encoding.**
- **Dependencies between drivers,** and what happens when two drivers match one device.
- **Rolling back a bad driver,** using the same `.bak` swap as other system files.
- **Whether a user can override the pins** in a pack on their own board.
