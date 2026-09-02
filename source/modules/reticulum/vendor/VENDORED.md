# Vendored from microReticulum

Source: https://github.com/attermann/microReticulum
Commit: `40fa628809d57140180c1c833559ab96fec992c1` (master branch)
License: Apache-2.0 (see LICENSE in that repo)

Everything under this directory (`vendor/`) is `src/microReticulum/` from
that commit, **byte-identical to upstream — no edits**. Same discipline
[../../meshcore/vendor/VENDORED.md](../../meshcore/vendor/VENDORED.md)
established for MeshCore: re-vendoring a future microReticulum update is a
straight file copy, no merge required.

Upstream's dependency on `attermann/Crypto` (a fork of `rweather/Crypto` —
software AES/SHA/Ed25519/X25519, no hardware acceleration, not vendored
here at all) is satisfied by [../compat/](../compat/) instead: drop-in
class-compatible shims for the exact method surface `Cryptography/*.h`
actually calls, backed by:

- **AES / SHA-256 / SHA-512** — ESP-IDF's mbedtls (`mbedtls_aes`,
  `mbedtls_md`), same approach and same rationale as MeshCore's own
  `compat/AES.h`/`compat/SHA256.h`.
- **X25519 (Curve25519 ECDH)** — ESP-IDF's mbedtls (`mbedtls_ecdh_*`,
  `MBEDTLS_ECP_DP_CURVE25519`) — the same primitive `pairing_module.c`'s
  own Phase A handshake already uses in this codebase
  (`mbedtls_ecdh_compute_shared`).
- **Ed25519 (EdDSA signing)** — deliberately NOT mbedtls: this exact
  ESP-IDF/mbedtls build has no classic (non-PSA) Ed25519 sign/verify API,
  and PSA crypto with twisted-Edwards support isn't enabled in this
  project's mbedtls config. Backed by
  [`source/lib/lib_ed25519`](../../../lib/lib_ed25519/) instead — the same
  vendored `orlp/ed25519` implementation MeshCore's own `Identity.cpp`
  already uses in this tree, so this is a second consumer of an already-
  proven dependency, not a new one.
- **RNG** — ESP-IDF's `esp_fill_random()`, the same primitive used
  throughout this codebase (e.g. `pairing_module.c`).

The four other upstream dependencies (`ArduinoJson`, `MsgPack`,
`ArxContainer`, `ArxTypeTraits`, `DebugLog` — header-only, genuinely
framework-agnostic despite their Arduino-ecosystem origins) ARE vendored,
under [../thirdparty/](../thirdparty/), each pinned to the exact tag
`microReticulum`'s own `CMakeLists.txt` fetches:

| Library        | Tag     | Commit                                    |
|----------------|---------|--------------------------------------------|
| ArduinoJson    | v7.4.2  | `733bc4ee82630c88c0a619a883cd3a206efae977` |
| MsgPack        | v0.4.2  | `1f552c31b940d6e9063ee17a4b3fa10c47b27169` |
| ArxContainer   | v0.7.0  | `d6affcd0bc83219b863c20abf7c269214db8db2a` |
| ArxTypeTraits  | v0.3.2  | `702de9cc59c7e047cdc169ae3547718b289d2c02` |
| DebugLog       | v0.8.4  | `b581f7dde6c276c5df684e2328f406d9754d2f46` |

`RNS_USE_FS`/`RNS_PERSIST_*`/`RNS_USE_PROVISIONING` are left OFF for now
(see this module's own CMakeLists.txt) — microStore's filesystem
persistence layer isn't vendored yet; it needs an adapter onto this
device's actual VFS conventions (`mount_app_config_vfs()`/claw_loader's
flash-fallback personal space) rather than assuming its own layout. See
the plan doc's staged build-out for when that lands.

The radio interface (`RNS::InterfaceImpl` subclass talking to the actual
SX1262 hardware) is implemented separately in
[`../rns_radio_adapter.cpp`](../rns_radio_adapter.cpp)/`.h` — microReticulum's
own `examples/common/lora_interface/LoRaInterface.*` was not vendored: it's
`#ifdef ARDUINO`-gated and assumes Arduino's RadioLib API, incompatible
with the RadioLib instance construction already established in
[`source/drivers/radio/sx1262_rl/sx1262_rl.cpp`](../../../drivers/radio/sx1262_rl/sx1262_rl.cpp)
(same `EspHal`/`Module`/`SX1262` types, non-Arduino ESP-IDF-native HAL).
