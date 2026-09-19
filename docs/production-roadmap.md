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

- Replace materialized scans with lazy iterators from memtable through SST.
- Add block restart points and binary search within SST indexes.
- Use a heap for large fan-in merges while preserving source precedence.
- Add a sharded block cache with explicit memory accounting.
- Separate cache admission from eviction so long scans do not evict hot point
  lookup data.
- Partition indexes and filters so metadata does not scale as one monolith.
- Add sequential readahead and bounded scan prefetch.
- Reduce key/value copies with owned block handles and lifetime-safe views.

Exit gate:

- Scan memory remains bounded independently of result size.
- Cache-resident point reads allocate no memory in the steady state.
- Read amplification and cache pollution are visible in metrics.

## Phase 3: sustainable write path

Make foreground ingestion track background capacity:

- Add atomic write batches and WAL group commit.
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

Introduce one engine-facing completion API with multiple backends:

```text
Engine
  |
  +-- synchronous pread/pwrite backend
  +-- portable worker-pool backend
  +-- Linux io_uring backend
  +-- fault-injection backend
```

The interface must support reads, writes, synchronization, cancellation,
priorities, deadlines, and completion ownership. Buffers and file handles must
remain alive until completion, and queue saturation must apply backpressure.

For the Linux backend:

- Batch adjacent flush and compaction writes.
- Submit independent SST reads concurrently.
- Add adaptive read batching that limits queueing delay.
- Evaluate registered buffers and registered files.
- Evaluate polling and direct I/O only for I/O-bound configurations.
- Keep foreground reads separate from background compaction scheduling even if
  they share rings or device queues.

Exit gate:

- The synchronous and asynchronous backends pass identical fault and recovery
  tests.
- Async execution overlaps useful work with I/O.
- Results show whether gains come from concurrency, batching, or lower
  submission cost.

PVLDB 2026 reports only modest gains from replacing an existing API with
`io_uring`, but substantially larger gains when the DBMS is redesigned around
asynchronous execution, batching, and registered buffers. The implementation
must therefore follow the staged design above rather than starting with
low-level ring flags.

## Phase 6: SSD-aware layout

Treat device behavior as an optional policy input:

- Add per-block LZ4 and Zstandard compression.
- Pack variable-sized compressed blocks without causing avoidable
  cross-alignment reads.
- Measure total host and device write amplification where device telemetry is
  available.
- Separate hot, short-lived output from cold, long-lived output when placement
  information is reliable.
- Add experimental FDP placement hints and ZNS zone append behind capability
  detection.
- Preserve the conventional filesystem backend as the default.

Exit gate:

- Compression reports CPU cost, read amplification, space savings, and total
  bytes reaching the device.
- Device-specific modes fail closed when required capabilities are absent.

Recent PVLDB work on SSD writes shows that lowering database-level write
amplification can still worsen device-level amplification. Total WAF is the
metric, and compression or placement must be evaluated with alignment and SSD
garbage collection in mind.

## Phase 7: multicore and CPU efficiency

Optimize CPU only after I/O and layout are measurable:

- Move the memtable to an arena-backed concurrent skip list or cache-conscious
  tree.
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
