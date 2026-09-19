# Changelog

## Unreleased

### Added

- C API (`hermesdb.h`) for Open/Put/Get/Delete/Sync, plus CMake package
  `find_package(HermesDB)` and `hermesdb::hermesdb`.

### Changed

- Point lookups search memtables and SSTs newest-first and return the visible
  version at the read timestamp, instead of materializing every key version.
- MVCC WAL appends are grouped: `Put` copies into an in-memory buffer under the
  commit lock, then a `pwrite` runs after that lock is released once the buffer
  reaches 64 KiB. `Sync`, memtable freeze, flush, and WAL close drain the
  buffer first. Get still sees the key as soon as it is in the memtable;
  durability lags until the group is written.
