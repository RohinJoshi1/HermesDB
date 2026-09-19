# API guide

The C++ API lives in namespace `hermesdb`. Include `hermesdb/db.hpp` for the
database and `hermesdb/transaction.hpp` when calling transaction methods.
`#include <hermesdb/hermesdb.hpp>` pulls in that public surface. The C API is
`#include <hermesdb.h>` and is intended for other languages via FFI.

## Opening and closing

`DB::Open(path, options)` creates or recovers a database directory and
returns `std::shared_ptr<DB>`. `Close()` finishes database shutdown.
`Sync()` requests durable synchronization of active persistence components.
Close handles before deleting or moving their database directory.

The important `Options` fields are:

- `block_size`: target uncompressed block size, in bytes.
- `target_sst_size`: approximate SST and memtable size target, in bytes.
- `num_memtable_limit`: number of memory tables allowed before flush pressure.
- `compaction_options`: one of the policy option structs below.
- `enable_wal`: write new mutations to a write-ahead log.
- `serializable`: enable serializable conflict validation for transactions.

The compaction choice is a `CompactionOptions` variant:

- `NoCompactionOptions`
- `SimpleLeveledCompactionOptions`
- `LeveledCompactionOptions`
- `TieredCompactionOptions`

The defaults are intended for examples and experimentation. Workloads should
tune file size, memory pressure, and compaction thresholds together.

## Keys, values, and writes

`Put`, `Get`, and `Delete` accept either `ByteView` or `std::string_view`.
`Bytes` is `std::vector<std::uint8_t>` and `ByteView` is
`std::span<const std::uint8_t>`. Empty keys are not a portable assumption;
callers should use non-empty keys.

`WriteBatch` accepts a span of `PutRecord` and `DeleteRecord` variants. A batch
is the right interface when related writes must share one write operation.

## Scans

`Scan(lower, upper)` returns a `DbIterator`. Bounds may be unbounded, included,
or excluded:

```cpp
auto iterator = db->Scan(
    hermesdb::KeyBound::Included("account/100"),
    hermesdb::KeyBound::Excluded("account/200"));

while (iterator.valid()) {
  consume(iterator.key(), iterator.value());
  iterator.next();
}
```

`key()` and `value()` return non-owning views. Copy them before advancing the
iterator if they must be retained.

## Transactions

`NewTransaction()` captures a read timestamp. Reads observe the transaction's
snapshot plus its own buffered writes. `Commit()` publishes the transaction.
When `serializable` is enabled, commit can reject a transaction whose tracked
read/write set conflicts with intervening work; callers must be prepared to
handle an exception and retry the complete logical transaction.

Do not keep a transaction alive indefinitely: its snapshot can prevent old
versions from being reclaimed.

## Maintenance

- `ForceFlush()` freezes and writes the active memtable.
- `ForceFullCompaction()` compacts the L0/L1 data selected by the engine.
- `AddCompactionFilter(prefix)` registers a prefix filter for eligible data.
- `DumpStructure()` returns a human-readable layout for diagnostics.

Forced maintenance is useful in tests and demonstrations. Normal applications
should generally allow background flush and compaction decisions to operate.

Errors are reported as exceptions, including `hermesdb::Error` for common
format and validation failures. Treat a failed write, flush, sync, commit, or
close as an operation that did not establish the durability guarantee the
caller requested.

## C API

`#include <hermesdb.h>` is a C ABI over the same engine. Open with
`hermesdb_open`, then `hermesdb_put`, `hermesdb_get`, `hermesdb_delete`, and
`hermesdb_sync`. `hermesdb_get` returns a malloc'd buffer (NUL-terminated for
convenience; `*value_len` is the exact byte length) or `NULL` if the key is
absent. Free that buffer and any `err` string with `hermesdb_free`.
`hermesdb_close` shuts the database down.
