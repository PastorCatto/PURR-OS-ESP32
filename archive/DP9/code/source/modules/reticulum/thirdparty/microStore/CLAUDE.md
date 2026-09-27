# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
pio run -e native                    # Build native (desktop) target
pio run -e heltec_wifi_lora_32_V3   # Build for ESP32 LoRa
pio run -e wiscore_rak4631          # Build for nRF52 LoRa
pio test -e native                  # Run tests on native target
pio test -e native -f test_iterator # Run a specific test suite
```

The native environment produces `.pio/build/native/program`. All environments use `-std=gnu++14`.

## Architecture

microStore is a persistent key-value store for embedded systems, inspired by Bitcask. All logic lives in the `include/microStore/` directory — there are no `.cpp` files.

### Core Design

- **Append-only segmented log**: Records are appended to segment files (up to 64KB each, 8 segments max). Writes go through a 4KB in-memory buffer before flushing.
- **Robin-hood hash index**: In-memory index (`std::unordered_map`) mapping keys → `(segment_id, offset)`. Rebuilt from a persistent index file at boot — no full log scan needed.
- **Tombstone deletes**: Logical deletes write a delete-marker record; compaction reclaims space.
- **Crash-safe compaction**: A journal file tracks compaction state (`COMPACTING` → `COMMIT`). On boot, `recover_if_needed()` checks the journal and either completes a partial compaction or discards it.
- **Index-based compaction walk**: Compaction builds per-segment sorted lists of live offsets from the in-memory index, then seeks directly to live records instead of a full scan.

### Key Structures

| Structure | Magic | Purpose |
|-----------|-------|---------|
| `RecordHeader` | `0xC0DEC0DE` | Per-record metadata: key/value lengths, timestamp, CRC-32 |
| `RecordCommit` | `0xFACEB00C` | Commit marker appended after each successful write |
| `Journal` | `0x4B564A4E` | Crash recovery state for compaction |

### Header Layout

| File | Purpose |
|------|---------|
| `include/microStore/FileStore.h` | KV store engine — the main library logic |
| `include/microStore/HeapStore.h` | In-memory KV store with TTL/eviction; same API shape as `FileStore` |
| `include/microStore/TypedStore.h` | Typed wrapper over any store (`FileStore` or `HeapStore`) |
| `include/microStore/Codec.h` | Encode/decode specializations for `TypedStore` keys and values |
| `include/microStore/FileSystem.h` | OOP filesystem abstraction (`FileSystem` / `FileSystemImpl`) |
| `include/microStore/File.h` | OOP file abstraction (`File` / `FileImpl`) with integrated CRC-32 |
| `include/microStore/Crc.h` | CRC-32 utility |
| `include/microStore/Adapters/` | Platform-specific `FileSystem` backends |

### Filesystem Abstraction

`FileStore.h` uses `FileSystem` and `File` directly (the OOP layer from `FileSystem.h` / `File.h`). `FileSystem` wraps a `shared_ptr<FileSystemImpl>`; `Store::init()` takes a `FileSystem` by value. Platform backends implement `FileSystemImpl` and are selected by build flag:

| Build Flag | Backend | Platform |
|-----------|---------|---------|
| `USTORE_USE_POSIXFS` | `PosixFileSystem` | native (Linux/macOS) |
| `USTORE_USE_LITTLEFS` | `LittleFSFileSystem` | ESP32 |
| `USTORE_USE_SPIFFS` | `SPIFFSFileSystem` | ESP32 |
| `USTORE_USE_INTERNALFS` | `InternalFSFileSystem` | nRF52 |
| `USTORE_USE_FLASHFS` | `FlashFSFileSystem` | nRF52 + SPI flash |
| `USTORE_USE_UNIVERSALFS` | `UniversalFileSystem` | auto-selects nRF52 or POSIX |
| `USTORE_USE_NOOPFS` | `NoopFileSystem` | stub (all ops return false/0) |

`File` accumulates a running CRC-32 on every read/write transparently; call `file.crc()` to retrieve it. To add a new platform, subclass `FileSystemImpl` and `FileImpl`.

### Platform Detection

- `PLATFORM_NATIVE`: uses `std::chrono` for timestamps
- `PLATFORM_ESP32` / `PLATFORM_NRF52`: uses Arduino's `millis()`

### Configuration Macros (top of FileStore.h)

| Macro | Default | Meaning |
|-------|---------|---------|
| `USTORE_MAX_VALUE_LEN` | 1024 | Max value size in bytes |
| `USTORE_DEFAULT_SEGMENT_SIZE` | 65536 | Per-segment file size limit |
| `USTORE_DEFAULT_SEGMENT_COUNT` | 8 | Max number of segments |
| `USTORE_WRITE_BUFFER_SIZE` | 4096 | Write buffer size |
| `USTORE_MAX_KEY_LEN` | 64 | Max key length in bytes |
| `USTORE_COMPACT_RETRY_MS` | 60000 | Compaction cooldown (ms) |

### Store Variants

Three store types share a common API shape and are interchangeable as the backing store for `TypedStore`:

**`microStore::FileStore`** (`FileStore.h`) — persistent, crash-safe, append-only log store.
- `init(FileSystem fs, const char* prefix)` — initialize with a `FileSystem` and file name prefix
- `put(key, ts, data, len)` — write key-value (overloads for `uint8_t*`, `char*`, `std::vector`)
- `get(key, out, len)` — read value
- `remove(key)` — logical delete (tombstone); compaction reclaims space
- `clear()` — wipe all data
- `dumpInfo(detailed)` — print storage statistics
- Range-based for loop via `begin()`/`end()` over `Entry` (key, value, timestamp)

**`microStore::HeapStore`** (`HeapStore.h`) — in-memory store backed by `std::map`. Same API as `FileStore` plus:
- `exists(key)` — check if key is present (and not expired)
- `size()` — number of live records
- `set_ttl_secs(secs)` — global TTL; entries older than this are lazily evicted
- `set_max_recs(n)` — evict oldest entry (by map order) when count exceeds `n`
- `put(key, data, len, ttl, ts)` — per-entry TTL overrides global policy
- `BasicHeapStore<Allocator>` template allows custom allocators; `HeapStore` is `BasicHeapStore<>`

**`microStore::TypedStore<Key, Value, Store>`** (`TypedStore.h`) — typed adapter over any store.
- Wraps a store reference; does not own it
- Encodes/decodes via `Codec<Key>` and `Codec<Value>` specializations
- Built-in codecs: `std::string`, `std::vector<uint8_t>`, `char*`
- Custom types require a `Codec<T>` specialization with `encode(T) → vector<uint8_t>` and `decode(vector<uint8_t>, T&) → bool`
- Range-based for loop yields `TypedStore::Entry{key, value}`
