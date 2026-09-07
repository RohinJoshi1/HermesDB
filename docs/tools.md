# CLI and compaction simulator

## Interactive CLI

Start the CLI with a database directory and policy:

```sh
tiny_lsm_cli --path scratch.db --compaction leveled --enable-wal
```

Add `--serializable` to enable serializable validation for transactions opened
through the database. The CLI itself issues individual database operations.

Commands are whitespace-delimited:

```text
fill 1 100
get 42
del 42
scan
scan 10 20
dump
flush
full_compaction
quit
```

`fill BEGIN END` uses an inclusive unsigned-decimal range and writes values in
the form `value<key>@<epoch>`. `scan BEGIN END` also uses inclusive bounds.
Keys containing whitespace cannot be entered in this minimal REPL. Non-printing
bytes returned by the engine are escaped as `\xNN`.

`quit` and `close` both close the database before exiting. End-of-file (usually
Control-D) does the same. `dump` is diagnostic text and is not a stable
machine-readable interface.

WAL and compaction are independent choices. Enabling the WAL records active
memtable changes for recovery; selecting `none` disables automatic compaction,
not flushing or persistence.

## Compaction simulator

The simulator creates one unit-sized mock SST per iteration and models file
placement. It neither opens a `MiniLsm` database nor creates real SST data:

```sh
compaction_simulator --policy all
compaction_simulator --policy simple --l0-trigger 2 --max-levels 4
compaction_simulator --policy tiered --tier-width 3 --iterations 50
compaction_simulator --policy leveled --level-multiplier 10 --seed 7
```

Options:

- `--policy simple|leveled|tiered|all`
- `--iterations N`
- `--l0-trigger N`
- `--max-levels N`
- `--tier-width N`
- `--level-multiplier N`
- `--seed N`
- `--quiet`

The per-flush display lists file IDs; parenthesized values are modeled file
sizes. Final metrics mean:

- `write_amp`: flushed plus rewritten units divided by flushed units.
- `peak_space`: maximum simultaneously live file count divided by flush count.
- `read_amp`: approximate number of runs consulted by a full point lookup.

These estimates demonstrate policy trade-offs. They omit block indexes, Bloom
filters, tombstones, key popularity, compression, cache effects, concurrency,
and actual byte overlap. They should not be used as database benchmarks.

Use a fixed `--seed` when comparing policies. Leveled simulation generates
repeatable mock key ranges from that seed; all policies receive the same flush
count.
