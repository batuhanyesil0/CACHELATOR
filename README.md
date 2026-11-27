# CACHELATOR

A single-level, set-associative CPU cache simulator written in C++. It runs
memory access traces through a configurable cache and reports hit/miss
statistics, write-policy traffic, a full 3C miss classification
(compulsory / capacity / conflict), and representative AMAT/MPKI figures.

Traces can be supplied by hand, or generated automatically from any real
program using Intel Pin.

## Features

- **Configurable set-associative cache** — total cache size, block size, and
  associativity (ways) are all parameters. Direct-mapped and fully-associative
  are just the endpoints of the associativity range (1 way, or 1 set).
- **True LRU replacement** — implemented with an N×N bit matrix per set.
- **Write policies** — both write-back (with dirty-bit tracking and
  write-back-on-eviction) and write-through are supported, with separate
  traffic counters.
- **3C miss classification** — every miss is labelled compulsory, capacity, or
  conflict. Capacity vs. conflict is decided by a parallel fully-associative
  LRU "oracle" of equal capacity (see below).
- **Representative AMAT and MPKI** — average memory access time and a
  per-access MPKI proxy, computed from clearly-labelled assumptions.
- **Optional Intel Pin integration** — produce a real trace from any binary
  without changing the simulator core.
- **64-bit addressing** — addresses are parsed and handled as `uint64_t`.

## Build

```
g++ -O2 -std=c++17 main.cpp -o cache_sim
```

(`clang++` with the same flags works as well.)

## Usage

The simulator has three modes:

```
./cache_sim                                              # default: ./trace.txt
./cache_sim --file <trace.txt>                           # explicit trace file
./cache_sim --pin <pin_path> <pintool_path> <program>    # generate trace via Pin
```

The cache core is identical in all three modes; the mode only decides where the
trace comes from. **Intel Pin is optional** — if you do not have it, hand-write
or otherwise produce a trace and use `--file`.

### Trace format

One memory access per line:

```
R <hex_address>
W <hex_address>
```

`R` = read, `W` = write. The address is hexadecimal (with or without a `0x`
prefix). Lines beginning with `#` are treated as comments and skipped (the Pin
tool emits a trailing `#eof`).

Example:

```
R 0x4A8600
W 0x7FFC186CEBF8
R 0x4A8620
```

## Generating a trace with Intel Pin

CACHELATOR uses Pin's stock `pinatrace` example, lightly modified so its output
matches the trace format above (`R/W 0x<addr>`). Pin runs only on x86/x86-64
(Linux or Windows).

The only change from the stock example is in the two record functions, which
now print `R`/`W` plus the hex address (instead of Pin's default format):

```c
VOID RecordMemRead (VOID* ip, VOID* addr) { fprintf(trace, "R 0x%lx\n", (unsigned long)addr); }
VOID RecordMemWrite(VOID* ip, VOID* addr) { fprintf(trace, "W 0x%lx\n", (unsigned long)addr); }
```

The tool instruments every instruction (`INS_AddInstrumentFunction`), and for
each memory operand inserts a predicated call to the matching record function,
passing the effective address (`IARG_MEMORYOP_EA`). On exit it writes a
trailing `#eof` and closes the file.

**To build it**, apply the change above to
`<pin>/source/tools/ManualExamples/pinatrace.cpp` and build with Pin's makefile:

```
cd <pin>/source/tools/ManualExamples
make obj-intel64/pinatrace.so
```

This produces `obj-intel64/pinatrace.so`, the pintool path used below.

```
# 1. Compile your program with optimisation OFF, so the access pattern is preserved
gcc -O0 -o my_program my_program.c

# 2. Run it under the simulator's --pin mode (single command, end to end)
./cache_sim --pin \
    ./pin-3.30-.../pin \
    ./pin-3.30-.../source/tools/ManualExamples/obj-intel64/pinatrace.so \
    ./my_program
```

Internally, `--pin` invokes Pin as a blocking subprocess, which writes
`pinatrace.out`; the simulator then consumes that file exactly as in `--file`
mode.

> On NixOS, Pin's prebuilt binaries do not run directly because the system is
> not FHS-compliant. They were made runnable with `patchelf` (patching the ELF
> interpreter and RPATH). On a standard FHS distribution (Ubuntu, etc.) no such
> step is needed.

## How the 3C classification works

Each miss is classified as follows:

1. **Compulsory** — the block has never been seen before (tracked with an
   `unordered_set`). First touch of a block always misses, in any cache.
2. Otherwise, the block was seen before but is no longer cached. To decide
   whether the *capacity* or the *set-mapping (conflict)* is to blame, the miss
   is checked against an **oracle**: a fully-associative LRU cache of the *same
   total capacity* as the real cache.
   - **Conflict** — the oracle still holds the block (so capacity was
     sufficient; the real cache lost it only because of its set mapping).
   - **Capacity** — even the oracle had evicted it (capacity genuinely
     insufficient).

A fully-associative cache has no sets and therefore no conflict misses, which
is exactly why it serves as a pure capacity reference. The oracle is kept
**synchronised** with the real cache: it is updated on every access (hit,
compulsory, and capacity/conflict alike), so that at the moment of any query it
reflects the same access history.

The oracle is implemented with a `std::list` (LRU order: newest at the front,
oldest at the back) plus an `std::unordered_map` from block number to its
position in the list. The list gives O(1) "evict the oldest"; the map gives O(1)
lookup and O(1) move-to-front. Together they form a standard O(1)
fully-associative LRU.

As a correctness invariant, in every run:

```
compulsory + capacity + conflict == total misses
```

and a fully-associative configuration always reports `conflict == 0`.

## Statistics reported

For each configuration the simulator prints:

- total accesses, hits, misses, hit rate
- write-backs (write-back policy) / write-throughs (write-through policy)
- the 3C miss breakdown
- **AMAT** = `hit_time + miss_rate × miss_penalty`
- **MPKI (proxy)** = `(misses / total_accesses) × 1000`

### Assumptions / limitations

- `hit_time = 1` cycle and `miss_penalty = 100` cycles are **representative
  constants**, not measured values. They model a single-level L1 whose misses
  go straight to main memory. Change them in `printStats` if you want a
  different scenario.
- MPKI is a **proxy**: the trace contains memory accesses, not an instruction
  count, so misses are divided by total accesses rather than by instructions.
  It is labelled accordingly and should not be read as a true MPKI.
- The simulator models a **single cache level** (no L2/L3 hierarchy) and does
  not model data *contents* — only addresses, hit/miss behaviour and write
  traffic.
- `seenBlocks` and the oracle are **analysis aids**, not hardware structures.
  Real hardware experiences compulsory misses but does not *label* them; the 3C
  breakdown is a property of the simulator, not of a real cache.

## Project layout

```
main.cpp              # the simulator (single translation unit)
trace.txt             # default trace (c[i] = A[i] * B[i]), used when no mode is given
README.md
```
