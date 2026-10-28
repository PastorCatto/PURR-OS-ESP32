# broken-kitten-recovery

Restores a PURR OS device from a computer on the same network, no USB cable. One self-contained executable for Windows, macOS and Linux.

```
broken-kitten-recovery            # fetch, check, serve
broken-kitten-recovery verify     # check what is in ./kitten-recovery, change nothing
broken-kitten-recovery --help
```

It downloads the official `recovery.manifest` and `modules.manifest` and every image they list, and checks each one against the
official keys (SHA-256 against the manifest, the payload hash in the image header, the ECDSA P-256 signature, and that the signing
key's role may sign that kind of image). Then it serves them at `/recovery/<file>` and answers the device's discovery probe
(`PURR-REPO?` on UDP 47800) with `PURR-REPO! <name> <port> relay-<id>`. Everything is checked again from disk just before serving;
nothing that fails is served.

The device does not rely on this tool: the loader checks every image itself, strictly in secure mode.

Build: `cargo build --release` (tests: `cargo test`). The official public keys are in `src/keys.rs`, generated from
`PurrOS/components/coreos/keys/purr_default_keys.c`.

Not done yet: the device side of the relay (attended confirmation, pairing, secure-mode pull), the developer role that pushes
signed drivers, a version floor check, and release binaries. See `plan3.md`.
