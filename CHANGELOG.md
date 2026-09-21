# Changelog

## Unreleased

### Added

- Packed zlib SST blocks (`Options::compression`): compress each block, keep
  explicit `(offset, size)`, and pack payloads that fit into 4 KiB device
  pages. Uncompressed tables keep the previous format.
- SST `Get` path: file-backed tables keep an fd, `pread` one block, and use
  `Options::block_cache_capacity` (default 4096). `Table::open(Bytes)` still
  holds the full image for tests.

### Changed

- Puts assign timestamps with `fetch_add`, insert into the memtable skip list,
  and MPSC-enqueue WAL frames. Get/Scan/NewTransaction use a closed timestamp
  prefix so snapshots do not see holes. Transaction commit still serializes
  for validation.
- MemTable is a concurrent skip list (insert-only until drop). Same user key
  with different timestamps can insert without a per-key table lock.

- Point lookups search memtables and SSTs newest-first and return the visible
  version at the read timestamp, instead of materializing every key version.
