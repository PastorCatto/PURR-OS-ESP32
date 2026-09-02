# microStore — User Guide

microStore is a header-only, embedded-friendly key-value store written in C++14. It targets MCU-class platforms (ESP32, nRF52840) but builds and runs identically on a POSIX desktop, which makes it trivial to develop and unit-test against. All persistent state is held in a small set of files behind a pluggable filesystem abstraction; in-memory variants are also provided for purely volatile use cases.

This guide walks through the storage model, how the engine treats flash, the Store variants the library exposes, and how to drop microStore into an application.

---

## 1. Storage Model

### 1.1 Log-structured key-value store

microStore is a *log-structured* KV store. Every mutation — insert, update, or delete — is appended to the end of an active log file as a self-contained record. Reads are served by consulting an **in-memory hash index** that maps each key to the `(segment, offset)` of its most recent record on disk.

This is the same architecture pioneered by Riak's **Bitcask** engine, and it has the same operational characteristics:

- Writes are O(1) sequential appends — no in-place updates, no B-tree rebalancing.
- Reads are a single hash-index lookup followed by a single disk seek + read.
- The hash index is rebuilt at boot, optionally from a persisted snapshot for fast warm-up.
- Stale records (overwrites and tombstones) are reclaimed by background **compaction**.

### 1.2 How microStore is like Bitcask

| Bitcask | microStore |
|---------|------------|
| Append-only log with periodic compaction | Same — append-only segment files compacted in-place |
| In-memory hash index keyed by key bytes | Same — `std::unordered_map<vector<uint8_t>, IndexValue>` |
| Each index entry stores `(file_id, value_offset)` | Each entry stores `(segment_id, offset, timestamp, ttl)` |
| Per-record CRC for corruption detection | Same — CRC-32 over header + key + value |
| Tombstone deletes | Same — a record with `FLAG_DELETE` set |
| Index rebuild by scanning the log on startup | Same — `rebuild_index_from_segments()` is the fallback when the persisted index is missing |

### 1.3 How microStore differs from Bitcask

Bitcask was designed for server-class systems with hundreds of gigabytes of disk and gigabytes of RAM. microStore re-shapes the same ideas to suit embedded flash:

| Concern | Bitcask | microStore |
|---------|---------|------------|
| Active log | Single grow-forever file rotated by size | Fixed-count, fixed-size **segments** (default 8 × 64 KB) |
| Compaction | Walks the entire data set, builds new files | **Incremental** — copies one segment's live records at a time into `compact.tmp`, then deletes the source segment and journals the progress |
| Worst-case extra free space | ~live-data size (write a full hint+data file) | ~1 segment (≈ `S` bytes), regardless of live-set size |
| Crash safety | Hint-file replay | Per-record CRC + `commit` magic, plus a small **journal file** with state machine (`COMPACTING` → `COMMIT`) recovered on the next boot |
| Index persistence | "Hint" file written during merges | Index file is written incrementally on every `put` / `remove`, then rewritten in bulk after compaction |
| Memory footprint | Designed for large heaps | Designed for tens-of-KB heaps; allocator is a template parameter (`BasicFileStore<Allocator>` / `BasicHeapStore<Allocator>`) |
| Filesystem layer | Direct POSIX I/O | OOP abstraction (`FileSystem` / `File`) with adapters for LittleFS, SPIFFS, Adafruit InternalFS, SPI flash, SD, POSIX |
| Per-record metadata | Timestamp only | Timestamp **and** per-record TTL (in seconds) |
| Eviction | None | Global TTL **and** "keep newest N records" policy, applied at put/get and during compaction |

The net effect: the same fundamentals as Bitcask, but with bounded flash overhead, bounded RAM, and crash-safe operation on hostile filesystems.

---

## 2. Flash-Friendly Behaviour

Embedded flash has three properties that punish careless filesystem usage: writes can only flip `1`-bits to `0`-bits, erases happen at coarse block granularity, and every program/erase cycle wears the cell. microStore is engineered around all three.

### 2.1 Append-only writes

Records are only ever written sequentially to the end of the active segment. There is no in-place mutation of an existing record — even an update is a new append plus an index swap. This:

- Hits the same flash page once per write, in order, with no read-modify-write inside the underlying filesystem.
- Avoids the random-write pattern that worst-case-ages flash, especially on SPIFFS and SPI NOR.

### 2.2 Segmented log

The log is split into a small, **fixed** number of segments (`USTORE_DEFAULT_SEGMENT_COUNT`, default 8) of a fixed size (`USTORE_DEFAULT_SEGMENT_SIZE`, default 64 KB). Bounding the segment count and size keeps total disk usage predictable and bounds the work each compaction must perform. The 64 KB default lines up with a common erase-block size; for tight devices you can drop it.

### 2.3 Write batching

Writes are buffered through a small in-memory ring (`USTORE_WRITE_BUFFER_SIZE`, default 512 B) and flushed only when full or when an operation explicitly demands durability (e.g. before persisting an index entry). The buffer is *streamed* — a record bigger than the buffer simply flushes mid-record and continues — so the buffer size does not need to be sized to the largest record. Match it to your platform's flash program granularity (256–512 B on most embedded flash, 4 KB on POSIX) to avoid filesystem-level write amplification.

### 2.4 Incremental, crash-safe compaction

When all segments fill, microStore compacts. The algorithm is the bounded-overhead variant described in `incremental_compaction.md`:

1. Open `compact.tmp` and write a journal record (`COMPACTING`, `next_seg=0`).
2. For each source segment `s = 0..N-1`:
   1. Stream `s`'s live records (those still referenced by the in-memory index) into `compact.tmp`.
   2. Flush `compact.tmp`.
   3. Update the journal with `next_seg = s+1` and the durably-valid byte count of `compact.tmp`.
   4. Delete source segment `s`.
3. Write `JOURNAL_COMMIT`.
4. Rename `compact.tmp` → `segment_0.dat`, rebuild the in-memory index from the new segment, and rewrite the persistent index in bulk.

Peak overhead during compaction is roughly **one extra segment** — not "double the live data". A crash at *any* point either rolls forward (rename `compact.tmp` to `segment_0` and rebuild) or rolls back (discard `compact.tmp`); no data is lost.

### 2.5 Persistent index (fast boot)

Every mutating operation appends a small entry to `<prefix>_index.dat`. At boot the engine simply replays the index file into the hash map — no full log scan. If the index file is missing or truncated, `rebuild_index_from_segments()` reconstructs it by scanning the segments, then rewrites the index in bulk so the next boot is fast again.

### 2.6 Integrity checking

Every record carries:

- A 32-bit magic (`0xC0DEC0DE`) for framing.
- A CRC-32 over header + key + value.
- A trailing commit marker (`0xFACEB00C`).

Torn writes — the classic flash failure mode — are detected during the segment scan and silently truncated at the first invalid frame. The `File` abstraction also accumulates a running CRC-32 on every read/write for callers that need stream-level integrity.

### 2.7 Eviction policies

Two policies are available on every Store; both reduce flash churn by keeping the live set small:

- **Global TTL** (`set_ttl_secs`): records older than the TTL are lazily evicted on read and removed during compaction.
- **Max records** (`set_max_recs`): on every `put`, the oldest record is evicted if the count exceeds the limit. At init, any over-cap entries are pruned and the index is rewritten so evicted keys do not reappear after reboot.

`FileStore::put` also accepts a per-record TTL that overrides the global setting.

---

## 3. Store Variants

The library ships three Stores that share the same API shape and can be composed.

### 3.1 `microStore::FileStore`

`include/microStore/FileStore.h`. Persistent, crash-safe, append-only log store. This is the main engine and the focus of this guide. `FileStore` is an alias for `BasicFileStore<std::allocator<uint8_t>>`; use the `BasicFileStore<Allocator>` template directly if you need a custom allocator (e.g. a fixed-size pool).

Use it when:

- You need data to survive reboots.
- You're running against any of the supported embedded filesystems.

### 3.2 `microStore::HeapStore`

`include/microStore/HeapStore.h`. RAM-only KV store backed by `std::map`. Same public API as `FileStore` plus the same TTL / max-records policies. There is no `FileSystem` to wire up; `init()` takes no arguments.

Use it when:

- You want the same call shape as `FileStore` but without persistence (caches, scratchpads, test doubles).
- You want a uniform `TypedStore` over either a persistent or a volatile backing store.

`HeapStore` is an alias for `BasicHeapStore<std::allocator<uint8_t>>`. Pass a custom allocator to the `BasicHeapStore<Allocator>` template if you need control over heap usage.

### 3.3 `microStore::TypedStore`

`include/microStore/TypedStore.h`. A thin, non-owning typed wrapper over any underlying Store:

```cpp
template<typename Key, typename Value, typename Store,
         typename KeyCodec = Codec<Key>,
         typename ValueCodec = Codec<Value>>
class TypedStore;
```

It encodes and decodes keys and values via `Codec<T>` specializations, then forwards to the underlying store. Built-in codecs are provided for `std::string`, `std::vector<uint8_t>`, and `char*`. For custom types, specialise `Codec<T>` with `encode(T) → std::vector<uint8_t>` and `decode(const std::vector<uint8_t>&, T&) → bool`.

Use it when you want type-safe `put(key, value)` / `get(key, value)` calls and forget about byte buffers at the call site.

---

## 4. Filesystem Adapters

`FileStore` does not talk to flash directly. It talks to a `microStore::FileSystem`, which is a thin `shared_ptr` wrapper over a `FileSystemImpl`. Adapters live in `include/microStore/Adapters/` and are activated by build flag:

| Build flag | Backend | Platform |
|------------|---------|----------|
| `USTORE_USE_POSIXFS` | `PosixFileSystem` | native (Linux/macOS) |
| `USTORE_USE_LITTLEFS` | `LittleFSFileSystem` | ESP32 |
| `USTORE_USE_SPIFFS` | `SPIFFSFileSystem` | ESP32 |
| `USTORE_USE_INTERNALFS` | `InternalFSFileSystem` | nRF52 internal flash |
| `USTORE_USE_FLASHFS` | `FlashFSFileSystem` | nRF52 + external SPI flash (Adafruit SPIFlash) |
| `USTORE_USE_SD` | `SDFileSystem` | SD card |
| `USTORE_USE_STDIOFS` | `StdioFileSystem` | native, stdio-based |
| `USTORE_USE_UNIVERSALFS` | `UniversalFileSystem` | auto-selects InternalFS on nRF52, POSIX elsewhere |
| `USTORE_USE_NOOPFS` | `NoopFileSystem` | stub (all ops return false/0); useful for compilation tests |

Adding a new backend means subclassing `FileSystemImpl` and `FileImpl`. The `File` class accumulates a running CRC-32 on every read/write transparently — call `file.crc()` to retrieve it.

---

## 5. Quick Start

microStore is **header-only**. Add the repository as a PlatformIO `lib_dep` (or copy `include/microStore/` into your project's include path), pick a filesystem adapter via a build flag, and you're ready.

```ini
; platformio.ini — POSIX desktop
[env:native]
platform = native
build_flags =
    -std=gnu++14
    -DPLATFORM_NATIVE
    -DUSTORE_USE_POSIXFS
```

```ini
; platformio.ini — ESP32 + LittleFS
[env:esp32]
framework = arduino
platform = espressif32
board = heltec_wifi_lora_32_V3
board_build.filesystem = littlefs
build_flags =
    -std=gnu++14
    -DPLATFORM_ESP32
    -DUSTORE_USE_LITTLEFS
```

---

## 6. Using the Library

### 6.1 Initialising a FileStore

`init()` takes a `FileSystem` instance and a *prefix*. The prefix is prepended to every file the engine creates (segments, index, journal, compact tmp), so picking distinct prefixes lets you host multiple independent stores on the same filesystem.

```cpp
#include <microStore/FileStore.h>
#include <microStore/Adapters/UniversalFileSystem.h>

microStore::FileSystem fs{ microStore::Adapters::UniversalFileSystem() };
fs.init();

microStore::FileStore store;
if (!store.init(fs, "/cfg")) {
    // fs is invalid — most likely the platform filesystem failed to mount
}
```

Optional `init()` arguments:

- `clearOnInit` — wipe everything before opening (useful for tests).
- `segment_size`, `segment_count` — override the defaults at runtime (still bounded by the compile-time `USTORE_MAX_*` macros).

Always call `store.close()` before destruction if you want a clean flush; the destructor does flush, but `close()` also closes the index and active segment explicitly.

### 6.2 Writing and reading

`put`, `get`, `remove`, and `exists` are overloaded for the common key/value shapes: raw byte buffers, `const char*`, `std::string`, `std::vector<uint8_t>`.

```cpp
// String keys and values
store.put("device.name", "sensor-42");

std::string name;
if (store.get("device.name", name)) {
    printf("name = %s\n", name.c_str());
}

// Byte buffers
uint8_t payload[64] = { /* ... */ };
store.put("blob.0", payload, sizeof(payload));

uint8_t out[256];
uint16_t size = sizeof(out);   // in: capacity, out: actual length
if (store.get("blob.0", out, &size)) {
    // size now holds the real value length; if you only want the length,
    // pass nullptr for `out`.
}

// Logical delete (tombstone) — space is reclaimed at the next compaction
store.remove("device.name");

// Cheap presence check (no disk read, respects TTL)
if (store.exists("blob.0")) { /* ... */ }
```

### 6.3 Iterating

Both `FileStore` and `HeapStore` expose a forward iterator yielding `Entry { key, value, timestamp, ttl }`, and the standard `begin()` / `end()` pair makes them work with range-based `for`. The `TypedStore` iterator forwards through these and decodes the `key` / `value` for you on each step.

#### What the iterator does — and does not — touch

For `FileStore`, the iterator walks the **in-memory hash index**. That means:

| Field | Cost per record |
|-------|-----------------|
| `entry.key` | Free — copied from the index |
| `entry.timestamp` | Free — already in the index |
| `entry.ttl` | Free — already in the index |
| `entry.value` | **One disk read** — segment open, seek, header read, value read, close |

The `value` is **lazy-loaded** on the *first dereference* (`*it` or accessing `entry.value` through the range-`for` reference). It is cached on the iterator until `++` advances to the next entry, at which point the cache is cleared and the next dereference will re-read from disk. `operator->` is metadata-only — using `it->key` / `it->timestamp` is guaranteed to be zero disk I/O.

For `HeapStore` everything is in RAM, so the distinction collapses: all fields are populated by `load()` on each advance with no I/O cost.

#### Metadata-only iteration (no disk reads)

When you only need keys or timestamps — for example, to print a directory listing, count entries by age, or build a candidate list before reading any value — touch only `it->key`, `it->timestamp`, and `it->ttl`. **Never dereference with `*it`** in this pattern, and **never access `entry.value`** through the range-`for` reference, or you will trigger a disk read on every step.

```cpp
// Zero disk I/O — walks the in-memory index only.
for (auto it = store.begin(); it != store.end(); ++it) {
    printf("%.*s  ts=%u\n",
           (int)it->key.size(), (const char*)it->key.data(),
           (unsigned)it->timestamp);
}
```

> **Gotcha:** range-`for` uses `*it` under the hood, which on `FileStore` will load the value. If you want the metadata-only fast path on a `FileStore`, **use the explicit iterator form above** rather than `for (auto& e : store)`.

#### Full iteration (key + value)

When you do need values, range-`for` is the natural form. Be aware that each loop step reads from disk:

```cpp
for (const auto& e : store) {
    printf("%.*s = %.*s\n",
           (int)e.key.size(),   (const char*)e.key.data(),
           (int)e.value.size(), (const char*)e.value.data());
}
```

For `TypedStore`, the wrapper iterates and decodes for you:

```cpp
using StringStore = microStore::TypedStore<std::string, std::string, microStore::FileStore>;
StringStore typed(store);

for (auto& e : typed) {
    printf("%s = %s\n", e.key.c_str(), e.value.c_str());
}
```

#### Filtering — read values only when you need them

The lazy-load behaviour shines when you want to scan keys and only fetch the values that survive a filter. Do the filter against `it->` (metadata only) and then promote to `*it` to materialise the value:

```cpp
const uint32_t cutoff = microStore::time() - 3600;  // last hour

for (auto it = store.begin(); it != store.end(); ++it) {
    if (it->timestamp < cutoff) continue;          // metadata-only, no I/O

    const auto& full = *it;                        // *now* the value is read from disk
    process(full.key, full.value);
}
```

This is materially cheaper than the naive `for (auto& e : store) { if (e.timestamp >= cutoff) ... }` form, which reads every value from disk before the filter runs.

#### Pitfalls

- **Mutating the store during iteration is undefined behaviour.** `put`, `remove`, `clear`, and `compact` all invalidate the underlying `std::unordered_map`, which immediately invalidates outstanding iterators. If you need to mutate as you scan, collect the target keys first and act on them after the loop ends:

  ```cpp
  std::vector<std::string> to_delete;
  for (auto it = store.begin(); it != store.end(); ++it) {
      if (should_evict(it->timestamp))
          to_delete.emplace_back((const char*)it->key.data(), it->key.size());
  }
  for (const auto& k : to_delete) store.remove(k.c_str());
  ```

- **`for (auto& e : store)` reads every value from disk on a `FileStore`.** It is the right shape when you actually want all the values; it is the wrong shape when you only need metadata. For metadata-only walks, prefer the explicit `it != end()` form and touch only `it->key` / `it->timestamp` / `it->ttl`.

- **Each value dereference is a full open/seek/read/close cycle.** There is no batching: walking 10 000 records and reading every value performs 10 000 file opens and 10 000 seeks. On slow flash this can dominate runtime. If the workload is dominated by full scans, consider keeping the data in `HeapStore` (RAM) and persisting only the deltas, or pre-aggregating into a smaller secondary key space.

- **Iteration order is unspecified** for `FileStore` (the index is an unordered hash map). `HeapStore` happens to iterate in key-sorted order because it's backed by `std::map`, but do not lean on that across store types — TypedStore preserves whatever order the underlying store delivers.

- **The lazy-load cache is per-position.** Dereferencing the same iterator twice without advancing serves a cached `value`; advancing past a record and back is impossible (forward iterator) but a fresh iteration *will* re-read. Don't bind a reference from `*it` and expect it to remain valid after `++it`.

- **Expired records may appear in the iterator.** TTL eviction is lazy — entries are only removed on `get` / `exists` or during compaction. Iteration walks the index as-is; check `it->timestamp` against the TTL yourself if your scan must exclude expired records.

- **`begin()` flushes the write buffer.** This is intentional so iteration sees pending writes, but it does count as a flash write. Don't put `store.begin()` inside a hot loop unless you mean to.

### 6.4 Timestamps and TTLs

Every record carries a 32-bit timestamp. On native builds it is wall-clock seconds since the Unix epoch (`gettimeofday`); on Arduino targets it is seconds since boot (`millis()` with a 32→64-bit roll-over fix-up) plus an optional offset settable via `microStore::set_time_offset()` for boards with an RTC.

`put` accepts an explicit timestamp if you want to control it:

```cpp
uint32_t now = microStore::time();
store.put("k", "v", /*ttl=*/0, /*ts=*/now);
```

TTLs are in **seconds** and come in two flavours:

```cpp
// Per-record TTL — overrides the global policy
store.put("session.token", token, /*ttl=*/3600);  // 1 hour

// Global TTL — applies to any record whose per-record ttl is 0
store.set_ttl_secs(86400);   // 24 hours
```

Expired records are evicted lazily — they are detected on `get`/`exists` (which then remove them from the index) and pruned at compaction. They do not consume RAM forever, but they do hold flash until the next compaction sweep.

### 6.5 Size-based eviction

```cpp
store.set_max_recs(500);    // keep at most the 500 newest records
```

The oldest record (by timestamp) is evicted whenever a `put` would push the count over the cap. At `init()` time any over-cap entries are pruned and the persistent index is rewritten so evicted keys do not reappear after reboot.

### 6.6 Storage statistics

```cpp
store.dumpInfo();              // detailed per-segment breakdown
store.dumpInfo(/*detailed=*/false);  // totals only
```

Prints, per segment and overall, the on-disk byte count, the number of live entries, the number of tombstones, and the number of dead (overwritten) records — useful for tuning `USTORE_COMPACT_THRESHOLD` and segment geometry.

### 6.7 Manual compaction

Compaction triggers automatically when (a) all segments fill or (b) the dead-record ratio crosses `USTORE_COMPACT_THRESHOLD`. You can also force one:

```cpp
store.compact();
```

`compact()` returns `false` if it could not complete (e.g. the filesystem is too full to hold even the bounded `compact.tmp`). After a failure the store enters a cooldown of `USTORE_COMPACT_RETRY_MS` before re-trying automatically.

### 6.8 Working with TypedStore

```cpp
#include <microStore/TypedStore.h>

microStore::FileStore backing;
backing.init(fs, "/cfg");

using StringStore = microStore::TypedStore<std::string, std::string, microStore::FileStore>;
StringStore cfg(backing);   // wraps, does not own

cfg.put("user.name", "ada");
std::string name;
cfg.get("user.name", name);
cfg.remove("user.name");

for (auto& e : cfg) {
    printf("%s = %s\n", e.key.c_str(), e.value.c_str());
}
```

A `TypedStore` over a `HeapStore` works identically — just substitute the third template parameter.

### 6.9 Custom codecs

To use your own types as keys or values, specialise `microStore::Codec<T>`:

```cpp
#include <microStore/Codec.h>

namespace microStore {
template<>
struct Codec<int32_t> {
    static std::vector<uint8_t> encode(int32_t v) {
        std::vector<uint8_t> out(sizeof(v));
        std::memcpy(out.data(), &v, sizeof(v));
        return out;
    }
    static bool decode(const std::vector<uint8_t>& data, int32_t& out) {
        if (data.size() < sizeof(int32_t)) return false;
        std::memcpy(&out, data.data(), sizeof(int32_t));
        return true;
    }
};
}

microStore::TypedStore<std::string, int32_t, microStore::FileStore> counters(backing);
counters.put("boot_count", 1);
int32_t n = 0;
counters.get("boot_count", n);
```

### 6.10 A complete walk-through

```cpp
#include <microStore/FileStore.h>
#include <microStore/Adapters/UniversalFileSystem.h>

int main() {
    // 1. Bring up the platform filesystem
    microStore::FileSystem fs{ microStore::Adapters::UniversalFileSystem() };
    if (!fs.init()) return 1;

    // 2. Open the store with a unique prefix
    microStore::FileStore store;
    if (!store.init(fs, "/cfg")) return 2;

    // 3. Policy: keep 24 h of data, but never more than 1000 records
    store.set_ttl_secs(86400);
    store.set_max_recs(1000);

    // 4. Write
    store.put("device.name", "node-42");
    store.put("device.serial", "SN0001");

    // 5. Read
    std::string name;
    if (store.get("device.name", name)) {
        printf("name = %s\n", name.c_str());
    }

    // 6. Iterate
    for (auto& e : store) {
        printf("%.*s\n", (int)e.key.size(), (const char*)e.key.data());
    }

    // 7. Clean shutdown — flushes the write buffer
    store.close();
    return 0;
}
```

---

## 7. Preprocessor Directives

### 7.1 Logging

```c
-DUSTORE_ENABLE_LOG
```

When defined, `USTORE_LOG(...)` expands to `printf(...)`. Otherwise it expands to a no-op, so the strings are dropped from the binary entirely. On Arduino, redirect newlib `printf` to `Serial.write` via a `_write()` shim if you want logs on the monitor (see `src/main.cpp` for an example).

Log output is verbose and includes per-record hex dumps — enable it for diagnostics, leave it off in production.

### 7.2 Filesystem adapter selection

| Macro | Effect |
|-------|--------|
| `USTORE_USE_POSIXFS` | Compile and use the POSIX adapter (native) |
| `USTORE_USE_STDIOFS` | Compile the stdio adapter |
| `USTORE_USE_LITTLEFS` | Compile the ESP32 LittleFS adapter |
| `USTORE_USE_SPIFFS` | Compile the ESP32 SPIFFS adapter |
| `USTORE_USE_INTERNALFS` | Compile the nRF52 InternalFS adapter |
| `USTORE_USE_FLASHFS` | Compile the nRF52 SPI-flash adapter |
| `USTORE_USE_SD` | Compile the SD card adapter |
| `USTORE_USE_UNIVERSALFS` | Compile the auto-selecting adapter |
| `USTORE_USE_NOOPFS` | Compile the no-op stub adapter |

You can enable several at once and pick the one you want at runtime via `microStore::FileSystem{ Adapters::SomeFileSystem(...) }`.

### 7.3 Platform identification

Set exactly one of these, usually mirroring the toolchain:

| Macro | Meaning |
|-------|---------|
| `PLATFORM_NATIVE` | Native build, uses `std::chrono` / `gettimeofday` |
| `PLATFORM_ESP32` | ESP32 build, uses Arduino `millis()` |
| `PLATFORM_NRF52` | nRF52 build, uses Arduino `millis()` |

### 7.4 Engine configuration

All have sane defaults; override only if you have a reason.

| Macro | Default | Effect |
|-------|---------|--------|
| `USTORE_DEFAULT_SEGMENT_COUNT` | `8` | Maximum number of segment files |
| `USTORE_DEFAULT_SEGMENT_SIZE` | `65536` | Per-segment file size in bytes |
| `USTORE_WRITE_BUFFER_SIZE` | `512` | In-memory write buffer (flush granularity) |
| `USTORE_MAX_KEY_LEN` | `64` | Maximum key length in bytes |
| `USTORE_MAX_VALUE_LEN` | `1024` | Maximum value length in bytes |
| `USTORE_MAX_FILENAME_LEN` | `64` | Maximum total length of generated filenames |
| `USTORE_COMPACT_RETRY_MS` | `60000` | Cooldown between failed compaction attempts |
| `USTORE_COMPACT_THRESHOLD` | `25` | % of dead records that triggers auto-compaction (0 to disable) |
| `USTORE_DEFAULT_TTL_SECS` | `0` | Initial global TTL (0 = disabled) |
| `USTORE_DEFAULT_MAX_RECS` | `0` | Initial max-records cap (0 = disabled) |

### 7.5 Build-time guard

| Macro | Meaning |
|-------|---------|
| `LIBRARY_TEST` | If set (and `PIO_UNIT_TESTING` is not), `src/main.cpp` is compiled — used for the library's smoke binary. Leave this **unset** in consumer projects. |

---

## 8. Sizing Guidance

For a flash filesystem of size `F`, the conservative rule with the incremental compaction algorithm is:

```
(N + 1) × S  ≤  F
```

where `N` is `USTORE_DEFAULT_SEGMENT_COUNT` and `S` is `USTORE_DEFAULT_SEGMENT_SIZE`. The extra `S` is the peak overhead for `compact.tmp` during a compaction sweep.

A practical record-count ceiling is:

```
K_max  =  floor( (N - 1) × S / R )
```

where `R = avg_key + avg_value + sizeof(RecordHeader) + sizeof(RecordCommit) ≈ avg_key + avg_value + 20`. The `(N-1)` reflects the fact that `segment 0` is reserved as the compacted base after the first compaction.

See `docs/file_store_sizing.txt` for the full derivation and a sizing table covering 28 KB through 16 MB filesystems.

The one **invariant you must hold** is:

```
S  >  USTORE_MAX_VALUE_LEN + USTORE_MAX_KEY_LEN + sizeof(RecordHeader) + sizeof(RecordCommit) + USTORE_WRITE_BUFFER_SIZE
```

If `S` is too close to that floor, segment rotation fires on nearly every write and compaction never settles.

---

## 9. Crash Safety

| Failure mode | Result |
|--------------|--------|
| Power loss mid-record | Torn record discarded by CRC + commit-marker scan |
| Power loss mid-compaction, before any source deleted | `compact.tmp` discarded on next boot |
| Power loss mid-compaction, some sources already deleted | `compact.tmp` renamed to `segment_0.dat`; index rebuilt from segments; next compaction consolidates |
| Power loss after `JOURNAL_COMMIT` | `finalize_compaction()` reruns and completes the swap |
| Lost / corrupt index file | `rebuild_index_from_segments()` runs and rewrites it |

Full details and the recovery state machine are in `docs/incremental_compaction.md`.

---

## 10. Known Issues

See the **Known Issues** section of `README.md` for current platform caveats — notably the `esp_littlefs` flash-full crash on older ESP-IDF, and the InternalFileSystem `seek()` workaround on the Adafruit nRF52 BSP.
