# microStore

Advanced header-only KV store for embedded filesystems that follows the log-structured KV architecture used by systems such as Bitcask, which combines an append-only log with an in-memory index for fast lookups.

## Features
- append-only segmented log
- robin-hood hash index
- persistent index file (fast boot)
- write batching
- crash safe commits
- tombstone deletes
- automatic compaction
- filesystem agnostic
- filesystem-agnostic backend
- journaled crash-safe compaction
- index-based compaction walk
- streaming iterator
- automatic age-based eviction (TTL)
- automatic size-based eviction (keep N most recent records)
- support for custom memory allocator

## FileStore Configuration


## Known Issues

### Bug in esp_littlefs on flash full

There is a bug in older versions of `esp_littlefs` that manifests as a crash on ESP32 builds using the default `espressif32` platofrm.
A fix is present in more recent versions of the ESP-IDF (5.x), but that version does not (yet) ship with PlatformIO.
Since ESP-IF is provided as a binary for linking, there is no known clean option for overriding the `esp_littlefs` with a patched version.
The current workaround for this issue is to use the `pioarduino` fork for Arduino Core 3.x / ESP-IDF 5.x support which contains this fix. This can be enable with the following platform definition in `platformio.ini`:

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/51.03.07/platform-espressif32.zip
```

or for the (currently) latest stable version:

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.32/platform-espressif32.zip
```

### InternalFileSystem::File::seek() missing "whence" parameter

The InternFileSystem `File` implementation does not expose the `whence` parameter to `File::seek()`, and directional seek capability is required by `microStore::FileStore`. Since InternFileSystem lives inside the framework-arduinoadafruitnrf52, it is difficult to cleanly override this behavior without messy patching.
A workaround is present in `microStore::Adapters::InternalFSFileSystem` that emulates whence-based seeking using absolute positions computed from `position()` and `size()`.
- SeekModeSet — passes pos directly, no change in behavior.
- SeekModeEnd — pos=0 (the only way FileStore uses it) correctly seeks to size(). If negative offsets from end were ever needed, pos would need to be a signed type in the interface — but that's not the case here.
- SeekModeCur — adds pos to current position. Same unsigned caveat for backward relative seeks, but FileStore never uses this mode.
- Return value now matches the interface contract: the new absolute position on success, -1 on failure (Adafruit returns bool).

A PR will be made to the upstream BSP repository [https://github.com/adafruit/Adafruit_nRF52_Arduino] to hopefully have this addressed in later BSP releases.
The (currently) installed version is 1.10700.0 (maps to BSP v1.7.0). The relevant file to patch in a PR would be:
- libraries/Adafruit_LittleFS/src/Adafruit_LittleFS_File.h  (add seek(uint32_t, uint8_t) overload)
- libraries/Adafruit_LittleFS/src/Adafruit_LittleFS_File.cpp (implement it via lfs_file_seek)
The underlying lfs_file_seek already takes a whence parameter (LFS_SEEK_SET/CUR/END), so the implementation would be trivial. Worth filing — but your adapter fix is the right defensive solution either way.

## Contributing

Contributions to **microStore** are welcome and appreciated.

Before opening an issue or submitting a pull request, please read the project's **[CONTRIBUTING.md](CONTRIBUTING.md)** guide. It describes the expectations for bug reports, feature requests, coding standards, testing, and pull requests.

In particular, contributors are asked to:

- Clearly identify the problem being solved before describing the proposed solution.
- Keep pull requests focused on a **single logical concern**.
- Test changes thoroughly, including on **all affected platforms** when modifying shared code.
- Update documentation when introducing user-visible changes.

Following these guidelines helps streamline reviews, improve software quality, and make it easier to integrate contributions.