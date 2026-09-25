# Vendored from MeshCore

Source: https://github.com/meshcore-dev/MeshCore
Commit: `bbb58cceb852a9190b46d5f6984e8e5140e6991e` (main branch)
License: MIT (see https://github.com/meshcore-dev/MeshCore/blob/main/license.txt)

Files in this directory (`Packet.{cpp,h}`, `Dispatcher.{cpp,h}`, `Mesh.{cpp,h}`,
`Identity.{cpp,h}`, `Utils.{cpp,h}`, `MeshCore.h`) are **byte-identical to
upstream** — no edits. Upstream's dependency on `rweather/Crypto`'s `AES128`/
`SHA256` classes (used only in `Utils.cpp` and `Packet.cpp`) is satisfied by
`../compat/AES.h` and `../compat/SHA256.h`, a drop-in class-compatible shim
backed by ESP-IDF's mbedtls, placed on the include path instead of the real
`rweather/Crypto` library (which is never fetched). This keeps re-vendoring a
future MeshCore update a straight file copy — no merge required.

The radio/board/clock/RNG interfaces these files require (`mesh::Radio`,
`mesh::MainBoard`, `mesh::RTCClock`, `mesh::MillisecondClock`, `mesh::RNG`)
are implemented separately in `../mc_radio_adapter.cpp`/`.h`, against
PurrOS's own `catcall_radio_t` — MeshCore's own `RadioLibWrapper`/board glue
was not vendored (it assumes Arduino's `SPIClass` and constructs its own
RadioLib instance, incompatible with the radio instance already owned by
`source/drivers/radio/sx1262_rl/`).

`../../lib/lib_ed25519/` is vendored from the same commit's `lib/ed25519/`
(zlib license, Copyright (c) 2015 Orson Peters).
