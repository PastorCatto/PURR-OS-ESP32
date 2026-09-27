# .cat format spec (draft 0.1)

The file format of a PURR OS app. It is read by AppManager (install), the runtime
(load), and the tools (build, verify, inspect). The shared container it sits in is
in `../bootloader/SPEC.md` section 3. How apps are built is in `../AppSDK/SPEC.md`.

Python apps are not `.cat` files. They are a plain `.mpy` beside a signed JSON config
(`../MicroPython/SPEC.md` section 2).

## 1. Decisions so far

- A `.cat` is a signed container with `image_type = 4`.
- **One file carries a payload for every platform.** The installer picks the one
  that matches the device.
- **The device keeps the whole file.** It does not trim to its own payload. Cost:
  more flash per app on small boards.
- **A fixed binary header plus a text manifest block** (small JSON). The loader reads
  only the binary parts. The signature covers both.
- **Each payload is a flat image with a relocation list,** built by linking the app
  twice and comparing the results. The loader is plain C with no ELF parser.
- **A format field per payload** leaves room for a richer payload type later.
- The full ELF and map of each build stay on the developer's PC as debug artifacts.
  They are never put on the device.

## 2. File layout

```
[ fixed binary header ]
[ payload table ]
[ manifest block, JSON ]
[ payload 0 ] [ payload 1 ] ...
[ signature ]
```

All integers are little-endian. Every offset is from the start of the file and is
bounds-checked before use, because the file may be hostile.

## 3. Fixed binary header

Loader-critical fields only (exact sizes fixed when the header is written down):

- magic and format version, and the header size
- class (unprivileged or privileged) and kind (daemon, command-line, UI)
- runtime kind and the minimum runtime version it needs
- flags: may run in the background, saves state
- stack size and heap size
- SDK version it was built with
- offset and size of the payload table, of the manifest block, and of the signature
- payload count

The container's common header (`bootloader/SPEC.md` section 3) also applies. For
`image_type = 4` its `chip_id` is set to "any", and its payload fields describe the
payload table, not one payload. That rule is to be added there.

## 4. Payload table

One entry per platform:

- **family:** the instruction-set family the code was built for (for example a
  base Xtensa family, or RISC-V)
- **format:** 1 = flat image with relocations
- offset, size and SHA-256 of the payload
- entry offset, and the sizes of code, read-only data, initialised data and
  zero-initialised data
- relocation list: offset, count

Because every payload's hash is in the signed table, each payload can be checked on
its own.

## 5. Manifest block

A small UTF-8 JSON object with the readable parts: name, version, description,
class, kind, the catcalls it needs with minimum versions, the permissions it
requests, and an optional display name and icon.

- Size-capped (proposed 2 KB).
- Parsed by AppManager and the tools with a tiny reader. **The loader never parses
  it.** Any field that affects loading also lives in the binary header, and the
  verifier checks that the two agree.
- Unknown fields are ignored, so the manifest can grow.

## 6. Flat image format (format 1)

- The image is laid out as code (with its literals), read-only data, then
  initialised data. Zero-initialised data is only a size, not stored.
- **Relocation list:** each entry is an offset of a 32-bit word to patch and a
  **segment tag** saying which segment's load address to add. Segment tags keep
  both loading models possible (one RAM block, or separate code and data
  windows), because the loading model is still open.
- The app's entry is a function that returns the lifecycle table
  (`../AppRuntime/SPEC.md` section 5).

## 7. Loading (plain C)

1. Choose the payload whose family matches the device. None: reject with that reason.
2. Check every offset and size against the file and against the limits.
3. Allocate memory for each segment (or one block).
4. Copy the payload in, using aligned 32-bit accesses where the memory requires it.
5. Apply the relocations: add the load address of the tagged segment to each word.
6. Clear the zero-initialised data, and return the entry.

Relocation is a pure function on a byte buffer, so it is tested on the PC.

## 8. Building (purrstrap)

- Compile and link the app at two different base addresses per segment, flatten both
  results, and compare word by word.
- A word that differs by exactly the base difference is a relocation. Any other
  difference is a build error, which catches constructs that cannot be relocated.
- Check that the app imports nothing but `catcall_get`.
- Build a payload for each family, write the table and manifest, sign, and save the
  ELF and map next to the output.

## 9. Limits

Caps on the number of payloads, the size of the manifest, the number of
relocations, and the maximum app size (`max_app_size`, set after measuring code RAM
on a board). A file over any limit is rejected before anything is allocated.

## 10. Testing

- Parse tests with valid files and malformed ones: truncated, offsets out of range,
  overlapping regions, wrong hash, and a header and manifest that disagree.
- Relocation tests on the PC with a known test blob and several base addresses.
- Every malformed case must be rejected without reading outside the file.
- Sample files from purrstrap in `test/vectors/`, checked both ways (the C loader
  accepts what purrstrap makes, and purrstrap parses what the C code accepts).

## 11. Open questions

- **RAM or in place from flash.** Decides the segment layout and the size cap. Needs
  the measurement on the board (`../AppManager/SPEC.md` section 3).
- **Family names.** Whether ESP32 and ESP32-S3 can share one Xtensa family. A build
  check on hardware.
- **Exact field sizes** in the header and table.
- **Relocation encoding.** A plain list of offsets, or delta-compressed to save flash.
- **Payload compression,** given that the device keeps the whole file.
- **Manifest format.** JSON is proposed here. It has to agree with the manifest format
  decision in the SDK spec.
