# Changelog

## [0.1.7] - 2026-06-27

### Added

- Added support for `FileStore` directories.
- Added support for treating trailing `/` paths as directory prefixes during `FileStore::init()`.
- Added propagation of subdirectory names in `listDirectory()`.
- Added initial `microStore` user guide documentation.
- Added `USTORE_ENABLE_LOG` preprocessor option for configurable logging.
- Added optional `basepath` constructor support to `LittleFSFileSystem` and `StdioFileSystem`.
- Added `close()` methods to `FileStore` and `HeapStore`.

### Changed

- Refactored file paths to use `./` relative naming for broader filesystem compatibility.
- Made `size()` methods `const`.
- Changed `TypedStore::iterator::operator*()` to return `Entry&` rather than by value.
- Disabled `main()` unless `LIBRARY_TEST` is defined.

### Improved

- Improved logging behavior when stores are accessed before initialization.
- Improved consistency across filesystem adapters.
- Expanded and improved test coverage.

### Fixed

- Fixed POSIX directory existence checks.
- Fixed stdio directory existence checks.
- Fixed range-based iteration behavior in `TypedStore`.
- Fixed directory handling during filesystem enumeration.
- Various minor compatibility and stability fixes.
