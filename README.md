# tiny-lsm course starter

`tiny-lsm` is a C++20 course port of
[Mini-LSM](https://github.com/skyzh/mini-lsm). This branch is deliberately
incomplete: it is the starter scaffold, not a working storage engine.

## Branches

- `course` is the exercise branch. It contains explicit chapter-labeled TODOs.
- `solution-checkpoints` preserves completed solutions for reference or
  instructor use.

Completed checkpoint tags belong to the preserved solution history; they are
not the workflow for starting an exercise.

## Start Week 1 Day 1

1. Read the course material for **Week 1 Day 1**.
2. Implement the `TODO(week1-day1)` items in the memtable and database state.
3. Configure and build:

   ```sh
   cmake --preset default
   cmake --build --preset default
   ```

4. Run the current exercise test:

   ```sh
   ctest --preset default -R week1_day1 --output-on-failure
   ```

The test is expected to fail on a fresh checkout with a message naming a
`TODO(week1-day1)` stub. It passes only after the exercise is implemented.
The test includes overwrite, tombstone, ordering, freeze, and concurrency edge
cases; do not remove those cases to make the test pass.

To list all current and future course work:

```sh
rg 'TODO\(' include src
```

## Starter scope

The active build contains the shared byte/error helpers, the pre-MVCC ordered
user-key memtable API, and the mutable plus newest-first immutable memtable
database state scaffold. Later chapters—internal keys, iterators, blocks, SSTs,
persistence, compaction, and MVCC transactions—remain declarations or explicit
source skeletons and are excluded from the active starter build.

Requirements: CMake 3.21 or newer, Ninja, and a compiler with C++20 support.
The library target is `tiny_lsm::tiny_lsm`; public headers are under
`include/tiny_lsm`.

## Upstream and licensing

This work is derived from Mini-LSM by Alex Chi Z, pinned to upstream commit
[`1d658ff`](https://github.com/skyzh/mini-lsm/tree/1d658ff). Mini-LSM starter
and solution code is licensed under the Apache License 2.0. Upstream copyright
and license details are in `docs/upstream.md`; repository licensing terms are
in `LICENSE`.
