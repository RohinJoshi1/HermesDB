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

- Puts claim a timestamp, insert, mark READY, and return without waiting for
  a closed prefix. Plain Get/Scan read at the claim cursor (read-your-writes).
  `NewTransaction` waits until the prefix covers timestamps claimed before
  begin.
- Extra Puts skip `maybe_freeze` unless they win `try_lock` on
  `state_change_mutex`, so writers do not queue behind one freeze.
- `BlockCache` uses up to 16 shard LRUs instead of one mutex.
- Group WAL `flush_if_needed` uses `try_lock` on the drain mutex.
- MemTable is a concurrent skip list (insert-only until drop). Same user key
  with different timestamps can insert without a per-key table lock.

- Point lookups search memtables and SSTs newest-first and return the visible
  version at the read timestamp, instead of materializing every key version.
- Publish a Put’s timestamp as soon as the memtable insert finishes, then
  freeze. Flush no longer holds `state_change_mutex` for the SST write, so
  later writers do not spin forever waiting on an unpublished predecessor.
