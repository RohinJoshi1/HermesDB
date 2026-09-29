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

- Memtables allocate nodes and values from a concurrent arena. Each Put
  copies key and value once into it (no per-node `new`, no temporary
  `InternalKey`), node towers match their height, and the per-node mutex is
  gone: values are immutable records swapped atomically. `MemTable::get`
  no longer allocates a lookup key, and memtable scans return views.
  `DumpStructure` reports `arena_bytes` for the mutable memtable.
- Scan sources pull 16-row batches (one restart interval). `DbIterator`
  still exposes one row at a time. User-key compare and same-user skip
  use 16-byte NEON/SSE2 vectors.
- SST data blocks use restart interval 16: keys compress against the last
  restart, and `BlockIterator::seek` binary-searches restart keys.
- `checksum` / WAL CRC-32 keep the zlib polynomial. ARM uses the CRC32
  instruction; other CPUs use slicing-by-8.
- SST scan cache misses read the current block plus the next four in one
  `pread`, insert them as non-point cache entries, and hint kernel readahead.
- `Scan` is a move-only lazy cursor: memtable and SST sources are seeked,
  batch-merged, then collapsed to the newest visible user key. Memory stays
  proportional to source count and the current block, not result size.
  Transaction scans overlay a workspace snapshot and record the read set as
  rows are emitted. Consume a transaction scan before `Commit`.
- Puts claim a timestamp, insert, mark READY, and return without waiting for
  a closed prefix. Plain Get/Scan read at the claim cursor (read-your-writes).
  `NewTransaction` waits until the prefix covers timestamps claimed before
  begin.
- Engine state uses C++20 `std::atomic<std::shared_ptr<const State>>`.
  Hot Gets and Puts use two-epoch RCU views instead of acquiring shared
  ownership on every operation. Mutable generations have writer gates, so
  freeze waits for in-flight writers before publishing an immutable memtable.
- MVCC WAL append accepts non-owning record views and encodes directly into
  trailing storage in one queue-node allocation, avoiding owned key/value
  copies and intermediate payload/frame allocations. WAL drain, write, and
  sync are serialized to preserve queue order, and drains reuse their staging
  buffer.
- Skip-list insertion uses CAS publication instead of locking hot predecessor
  nodes; exact-key value replacement retains its per-node lock.
- Timestamp claims reserve READY-ring capacity atomically, preventing
  concurrent writers from wrapping and overwriting an unclosed slot.
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
