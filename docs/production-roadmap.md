# Production and Performance Track

This track starts from a working embedded LSM. The course
teaches storage-engine invariants; this document turns that engine into a
production-inspired experimental engine for modern NVMe devices.

## Positioning

The project is not intended to replace SQLite or RocksDB feature-for-feature.
Its target is narrower:

> A modern C++ embedded LSM storage engine focused on predictable tail latency,
> workload-aware compaction, and asynchronous NVMe I/O.

The primary users are storage-system builders, stream processors, write-heavy
services, and researchers who need an understandable engine with reproducible
performance experiments.

## Engineering principles

1. Correctness and recoverability precede performance work.
2. Every optimization needs a workload, baseline, and measured trade-off.
3. Tail latency and sustainable throughput matter more than short peak runs.
4. Foreground writes must be regulated against actual flush and compaction
   capacity rather than stopped only after hard thresholds are crossed.
5. `io_uring` is an architectural backend, not a drop-in syscall replacement.
6. Optimize total write amplification across the database and SSD, not only
   bytes emitted by compaction.
7. Hardware-specific paths remain behind portable interfaces and retain a
   synchronous, fault-injectable reference implementation.

## Phase 0: trustworthy baseline

Complete and freeze the storage semantics before tuning them:

- Version the WAL, manifest, block, and SST formats.
- Add fault injection at every durable-write boundary.
- Test torn writes, partial records, lost directory entries, stale files, and
  crashes during flush and compaction installation.
- Add property tests for iterator ordering, tombstones, snapshots, and
  compaction equivalence.
- Fuzz every decoder with checksums enabled.
- Add online checkpoints, backup/restore, and a format-inspection tool.
- Establish a synchronous `pread`/`pwrite` reference I/O path.

Exit gate:

- Recovery tests pass for every injected crash point.
- Fuzzing runs without memory-safety or invariant failures.
- A database created by the previous supported format can be opened or
  explicitly rejected with a useful compatibility error.

## Phase 1: benchmark and observability foundation

Add a benchmark suite before changing architecture:

- `fillseq`, `fillrandom`, `readrandom`, `readwhilewriting`, range scans, and
  delete-heavy workloads.
- YCSB A–F with uniform, Zipfian, and latest-key distributions.
- Warm-cache, cold-cache, and dataset-larger-than-memory configurations.
- Runs from 1 to the machine's physical-core count.
- Sustained runs long enough to reach compaction steady state.

Measure:

- Throughput and p50, p95, p99, and p99.9 latency.
- User bytes, WAL bytes, flush bytes, and compaction bytes.
- Read, database-write, device-write, and space amplification.
- L0 file count, immutable memtables, pending compaction bytes, and stall time.
- Block-cache admission, hit, eviction, and scan-pollution rates.
- I/O queue depth, batch size, CPU cycles per operation, and allocation count.

All benchmark output should include the commit, compiler, options, dataset,
filesystem, kernel, CPU, memory limit, and storage-device model.

Exit gate:

- One command produces repeatable results and machine-readable output.
- Performance regressions can be associated with a specific engine metric.

## Phase 2: read-path efficiency

Remove avoidable work before introducing specialized I/O:

- Add block restart points inside SST data blocks.
- Add a sharded block cache with explicit memory accounting.
- Partition indexes and filters so metadata does not scale as one monolith.
- Add sequential readahead and bounded scan prefetch. **Done:** a scan
  cache miss `pread`s the current block plus the next 4 (`F_RDADVISE` /
  `posix_fadvise` on the same span). Not an async I/O queue.
- Reduce key/value copies with owned block handles and lifetime-safe views.

Exit gate:

- Scan memory remains bounded independently of result size.
- Cache-resident point reads allocate no memory in the steady state.
- Read amplification and cache pollution are visible in metrics.

## Phase 3: sustainable write path

**Partial.** WAL group commit (64 KiB `pwrite` after enqueue) and concurrent
plain Puts (`fetch_add` timestamp, skip-list insert, MPSC WAL, closed
`visible_timestamp`) are in the engine. Transaction commit remains mutexed.
Still missing: independent flush/compaction thread pools, stall metrics as
admission input, and a feedback write controller.

Make foreground ingestion track background capacity:

- Add atomic write batches and WAL group commit. **Done** for grouped WAL
  `pwrite` and `WriteBatch`; durability still lags visibility.
- Separate visibility from durability and expose explicit sync policies.
- Run flush and compaction in independent resource pools.
- Track L0 pressure, immutable-memtable pressure, pending compaction bytes,
  flush bandwidth, and compaction bandwidth.
- Implement deterministic soft slowdown and hard safety limits.
- Add a proactive admission controller that smoothly adjusts accepted write
  bandwidth before hard stalls occur.
- Start with a transparent feedback controller; treat learned control as an
  experiment only after the measured signals and safety boundaries are stable.

Exit gate:

- A sustained write workload reaches steady state without unbounded backlog.
- p99.9 latency and throughput do not oscillate through repeated stop-and-go
  stalls.
- Safety limits still prevent memory or disk exhaustion.

This phase follows the central result of 2026 work on Sustainable RocksDB:
write stalls are accumulated pressure in a delayed pipeline and should be
treated as continuous admission control, not only threshold-triggered
emergency stops.

## Phase 4: workload-aware compaction

Keep compaction policy separate from execution:

- Represent compaction inputs, expected overlap reduction, bytes rewritten,
  output placement, and tombstone/version reclamation as explicit tasks.
- Schedule within CPU, I/O, temporary-space, and foreground-latency budgets.
- Support leveled and tiered baselines before adaptive policies.
- Score candidates using estimated read benefit, write cost, space cost,
  compaction debt, and transition cost.
- Permit carefully validated intra-level and multi-level candidates.
- Re-evaluate decisions as workload read/write ratio and key skew change.
- Preserve deterministic guardrails and a fixed-policy fallback.

Exit gate:

- Policy simulations predict the task selected by the running engine.
- Dynamic workload transitions improve without correctness changes or
  unbounded temporary space.
- Results report amplification and tail latency during transitions, not only
  after the new layout is reached.

ArceKV motivates optimizing the transition itself under changing workloads
rather than blindly converging to a fixed target layout.

## Phase 5: portable asynchronous I/O

One engine-facing completion API; backends are interchangeable and share
recovery tests.

```text
Engine (flush / compaction / Get block reads)
  |
  +-- sync: pread / pwrite / fsync          (reference, all platforms)
  +-- pool: bounded worker threads          (default on macOS / BSD / Unix)
  +-- io_uring: Linux 5.6+                  (opt-in; fail closed if unavailable)
  +-- inject: fault / delay / reorder       (tests only)
```

Do **not** use `epoll` (or kqueue readiness) as the SST/WAL I/O path. Those
multiplex socket readiness; regular files are typically always readable and
do not provide completion of `pread`/`pwrite`. `epoll` is only relevant if a
network server is added later.

The interface must support reads, writes, fsync, cancellation, priorities
(foreground Get vs background compaction), and completion ownership. Buffers
and FDs stay alive until completion. A full queue applies backpressure into
the write-admission controller (Phase 3).

Linux `io_uring` requirements:

- Batch adjacent flush and compaction writes; submit independent SST reads
  concurrently.
- Registered buffers/files and SQPOLL only after a measured I/O-bound
  baseline; they are not the first switch.
- Separate foreground and background queues even if they share a ring.

Exit gate:

- Sync, pool, and (on Linux) io_uring pass the same crash/recovery tests.
- Ablations report whether gains come from concurrency, batching, or
  submission cost — not from “enabled io_uring” as a boolean.

PVLDB 2026 (Jasny et al.) found modest gains from swapping an existing API
for `io_uring`, and large gains only when the engine is built around async
completion, batching, and registered resources.

## Phase 6: SSD-aware layout (Lee et al. 2–4, LSM form)

Three independent layout work packages. Each has a frozen baseline, a single
treatment, then a combined run. Do not enable all three in the first
comparison.

### 6a. Packed block compression (paper §3)

**Status.** Implemented as `Compression::zlib` (not LZ4/Zstd). Uncompressed
format remains the default. Measure with `hermesdb_bench --compression none|zlib`.
Alphabet-like bench values compress; random values will not. WAL is uncompressed.

**Baseline.** Uncompressed 4 KiB blocks, `pwrite` of whole SSTs as today.

**Treatment.** Compress each block (zlib today; LZ4/Zstd optional later).
Concatenate compressed payloads and pack toward 4 KiB device pages; the
block index stores `(file_offset, compressed_len)`.

**Must not.** Store a 1.5 KiB payload in a 4 KiB slot and call it
compression. That is the in-place failure mode the paper measures.

**Metrics.** Host bytes written, SST bytes on disk, Get p50/p99, blocks
read per Get, compress/decompress CPU ns, and (if available) device NAND
bytes.

### 6b. Lifetime streams (paper §4, file granularity)

**Baseline.** WAL, L0, and lower-level SSTs share one directory / one
allocator; files interleave on the device.

**Treatment.** Two or more write streams with stable lifetime:

| Stream | Contents | Expected death |
| ------ | -------- | -------------- |
| `wal` | redo log | after memtable flush |
| `hot` | L0 (and optionally L1) | next compaction |
| `cold` | lower levels | long |

Implementation: separate directories or filesets so the filesystem/FTL
sees sequential appends per stream. Do **not** mix WAL tail and Lmax SST
in the same file or the same preallocated extent. Optional later: FDP
placement IDs or ZNS zones, one per stream; default remains POSIX files.

This is deathtime grouping at SST granularity. Per-key GDT inside an SST
is out of scope until streams are measurable.

**Metrics.** Same as 6a, plus time-to-delete of each stream’s files
(histogram), L0 vs Lmax file age, and device WAF if OCP/SMART is present.

### 6c. Host GC unit = device reclaim unit (paper §5)

**Baseline.** `target_sst_size` = 2 MiB (current default). Compaction
emits many small files.

**Treatment.** Flush and compaction **output** size is
`max(target_sst_size, reclaim_unit)`, with `reclaim_unit` configured
explicitly (benchmark flag) or inferred (FDP RU; else a documented
constant, e.g. 512 MiB–16 GiB swept in the experiment). Writes within
one SST are sequential. Compaction prefers rewriting a whole stream
segment so deleting inputs can free a contiguous region rather than
punching 2 MiB holes.

**Must not.** Increase size-tiering / extra copies to cut *logical* WAF
while filling the device. Report **total WAF** = host_bytes_written /
user_put_bytes, and when telemetry exists
**device WAF** = nand_bytes / host_bytes_written.

**Metrics.** Files created, mean SST size, fragmentation (1 −
contiguous_free / free), host WAF, device WAF, space amplification,
throughput after the drive is ≥80% full.

### Benchmark protocol (required for 6a–6c and Phase 5)

Fix: commit, compiler, `Options` except the treatment flag, dataset,
filesystem, kernel, CPU, memory limit, device model. Fill the device to
a stated occupancy (e.g. 80–90%) before the measurement window when
claiming SSD-WAF results.

| Workload | Why |
| -------- | --- |
| YCSB-A Zipfian θ = 0.8, dataset ≫ buffer/block cache | paper’s primary |
| `overwrite` + concurrent Get | mixed read/write tails |
| `fillrandom` until host writes ≥ 4× dataset | GC/compaction steady state |
| YCSB-C (read-only, warm) | 6a must not lose point-read tails |

Each treatment vs baseline: ≥3 runs, report median throughput, p50/p99/p99.9,
host write bytes, WAL+flush+compaction bytes (already in `DB::Metrics`),
and device NAND bytes when the platform exposes them. A change ships only
if it improves the **target metric** without a silent regression on the
others (e.g. 6a must not raise Get p99 more than a stated bound).

I/O backend is a **crossed factor**, not a substitute for 6a–6c:

- macOS/Unix CI: `sync` and `pool`
- Linux perf machine: `sync`, `pool`, `io_uring`

Claim “io_uring helped” only if `pool` vs `io_uring` differs with 6a–6c
held fixed.

Exit gate:

- 6a, 6b, and 6c each have a published baseline/treatment table.
- Compression reports CPU, read amp, space, and host bytes to the device.
- Lifetime streams can be disabled; device modes fail closed.
- Combined 6a+6b+6c is measured last, not first.

## Phase 7: multicore and CPU efficiency

**Partial.** The memtable is a concurrent skip list (insert-only until drop).
It is not arena-backed. Remaining: shard table-registry / block-cache
metadata, buffer preallocation, hardware CRC32C, SIMD, NUMA.

Optimize CPU only after I/O and layout are measurable:

- Move the memtable to an arena-backed concurrent skip list or cache-conscious
  tree. **Skip list: done. Arena: not done.**
- Shard mutable indexes, table registries, and block-cache metadata.
- Preallocate WAL, block-builder, and compaction buffers.
- Add hardware CRC32C with runtime dispatch.
- Evaluate SIMD common-prefix and Bloom-filter hashing.
- Add NUMA-aware placement only after cross-socket effects are reproduced.

Exit gate:

- Profiling attributes the majority of CPU time to useful key, checksum,
  compression, and copy work rather than allocation or lock contention.
- Every hardware-specific path has a portable differential test.

## Phase 8: operational readiness

- Structured metrics and tracing for reads, writes, flushes, compactions,
  stalls, recovery, and background errors.
- Runtime option validation and safe defaults.
- Administrative tools for inspection, checkpointing, repair, and migration.
- Bounded shutdown with outstanding-I/O draining.
- Resource quotas and disk-space reservation for compaction.
- Continuous sanitizer, fuzz, crash, compatibility, and benchmark jobs.

Production use should not be claimed until this phase is complete and recovery
has been exercised across long-running deployments.

## Research basis

The roadmap translates the following systems results into implementable work:

- Jasny et al., [High-Performance DBMSs with io_uring: When and How to Use
  It](https://www.vldb.org/pvldb/vol19/p2317-jasny.pdf), PVLDB 19, 2026:
  architecture, batching, and registered resources matter more than a
  mechanical API substitution.
- Shin et al., [How Much Can RocksDB Chew? Achieving Near-Zero Write Stalls
  with Sustainable RocksDB](https://www.vldb.org/pvldb/vol19/p3202-shin.pdf),
  PVLDB 19, 2026: regulate write admission using observed internal pressure and
  time-varying background capacity.
- Liu et al., [ArceKV: Towards Workload-driven LSM-compactions for Key-Value
  Store Under Dynamic Workloads](https://www.vldb.org/pvldb/vol19/p958-liu.pdf),
  PVLDB 19, 2026: optimize compaction actions and transition behavior as
  workloads change.
- Lee et al., [How to Write to
  SSDs](https://www.vldb.org/pvldb/vol19/p1469-lee.pdf), PVLDB 19, 2026:
  evaluate database and SSD write amplification jointly and account for
  compression, placement, and garbage-collection granularity.
- Leis et al., [LeanStore: A High-Performance Storage Engine for NVMe
  SSDs](https://www.vldb.org/pvldb/vol17/p4536-leis.pdf), PVLDB 17, 2024:
  integrate caching, I/O, synchronization, MVCC, logging, checkpoints, and
  recovery as one storage-engine design.

Learned indexes, learned cache admission, and reinforcement-learning control
remain optional experiments. They should not precede deterministic baselines,
observability, or safety guardrails.
