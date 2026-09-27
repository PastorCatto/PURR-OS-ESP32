# littlefs (vendored)

The LittleFS core, copied unchanged from upstream so builds need no network and the same
code can run in the boot package's read-only reader.

- Source: https://github.com/littlefs-project/littlefs
- Version: v2.11.3 (commit 6cb4e86540eca0d9ba62500a298385c9d863c8be)
- Files: `lfs.c`, `lfs.h`, `lfs_util.c`, `lfs_util.h`, `LICENSE.md` (BSD-3-Clause)

Do not edit these files. PURR OS code talks to it through `purr_fs`
(`PurrOS/components/kernel/src/purr_fs.c`). To update, copy the same five files from a newer
tag and change the version and commit above.
