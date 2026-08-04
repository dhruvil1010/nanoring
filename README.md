# nanoring

[![CI](https://github.com/dhruvil1010/nanoring/actions/workflows/ci.yml/badge.svg)](https://github.com/dhruvil1010/nanoring/actions/workflows/ci.yml)

**A wait-free SPSC ring buffer, and a harness honest enough to measure it in nanoseconds.**

A single-producer/single-consumer queue with no locks and no CAS, a
mutex-and-condition-variable queue to compare it against, and a measurement rig
careful enough that the comparison means something.

The project has a second purpose beyond "lock-free is faster": it ships the same
ring buffer compiled two ways. `_padded` puts the producer's and consumer's index
counters on separate cache lines; `_unpadded` (`-DSPSC_DISABLE_PADDING`) puts them
on the same one. Nothing else differs — same source, same flags, same cores. The
gap between those two binaries is the cost of false sharing, isolated.

Written up from first principles in [LEARN.md](LEARN.md).

---

## Results

Measured 2026-08-04 on the machine below. Every number in this section came out
of the harness on that day; nothing is estimated, remembered, or illustrative.
Timing tables report the **median of 3 runs** (individual runs and spread noted
below each table). This is a stock laptop running a desktop session — no core
isolation, no IRQ steering — so treat the extreme tail (p99.99, max) as a
property of the environment, not the queue.

### Environment

| | |
|---|---|
| CPU | Intel Core i5-12450H (Alder Lake, hybrid): 4 P-cores with SMT + 4 E-cores |
| Cores / threads | 8 cores / 12 threads (logical 0–7 = P-cores in SMT pairs, 8–11 = E-cores) |
| Topology | verified empirically: (2,3) ping-pong p50 = 50 ns (shared L1 → SMT siblings); (2,4) p50 = 100 ns (distinct P-cores) |
| L1d / L2 / L3 | 48 KB per P-core, 32 KB per E-core / 1.25 MB per P-core + 2 MB shared E-cluster (7 MB total, matches WMI) / 12 MB shared (matches WMI) |
| Cache line | 64 bytes (printed by the benchmark header) |
| RAM | 16 GB |
| OS / kernel | Windows 11 Home, build 26200 (no WSL — this is a native-Windows run) |
| Compiler | GCC 15.2.0 (MSYS2 UCRT64) |
| Flags | `-std=c++20 -Wall -Wextra -Wpedantic -O2 -g` |
| Producer / consumer core | 2 / 4 (two distinct P-cores, same socket) |
| Tuning applied (isolcpus, C-states, turbo, SMT) | **none** — stock power plan, turbo on, SMT on, desktop apps running |

### Throughput

10,000,000 `uint64_t` messages, 1024-slot queue, one producer, one consumer,
both pinned. Warmup discarded.

| Build | Scenario | messages/sec | ns/op |
|---|---|---|---|
| padded | `spsc_ring`, spin | 78,635,764 | 12.72 |
| padded | `mutex_queue`, spin on `try_*` | 7,924,425 | 126.19 |
| padded | `mutex_queue`, blocking | 4,178,072 | 239.34 |
| unpadded | `spsc_ring`, spin | 43,819,308 | 22.82 |

Run spread (msg/s): spsc padded 66.0M–78.7M, spsc unpadded 42.6M–53.6M,
mutex spin 7.9M–8.5M, mutex blocking 4.1M–4.8M.

| Derived | Value |
|---|---|
| `spsc_ring` speedup over `mutex_queue` (spin) | **9.9×** (78.6M / 7.9M) |
| Padded speedup over unpadded (false-sharing cost) | **1.79×** (78.6M / 43.8M) |

### Latency

Ping-pong round trip, halved. 1,000,000 samples per scenario. All figures in
nanoseconds, one way.

| Build | Scenario | min | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|
| padded | `spsc_ring`, spin | 50 | 100 | 150 | 200 | 11,150 | 780,450 |
| padded | `mutex_queue`, spin | 100 | 500 | 1,300 | 4,700 | 76,450 | 2,646,800 |
| padded | `mutex_queue`, blocking | 650 | 4,750 | 23,800 | 224,400 | 987,350 | 4,317,800 |
| unpadded | `spsc_ring`, spin | 100 | 150 | 200 | 300 | 26,500 | 5,839,400 |

Measured `steady_clock::now()` cost, included in every sample above:
p50 0 ns, p99 100 ns.
Effective clock resolution: 100 ns.

Two caveats on reading this table. First, Windows' `steady_clock` steps in
100 ns units (the harness measures and prints this), so every one-way figure is
quantised to a 50 ns grid — "p50 = 100" means the true median lies somewhere in
that bucket, and min/p50/p99 for the spsc ring are at the grid's limit rather
than precise values. The *ordering* and the padded-vs-unpadded gap are real; the
low-end digits are not fine-grained. Second, p99.99 and max moved a lot between
runs (spsc padded max: 0.28 ms–1.85 ms) — that is the desktop environment
(scheduler, interrupts), not the queue, which is exactly why the tuning checklist
below exists.

### Hardware counters (`bench/run_perf.sh`)

Same workload, both builds, counted with `perf stat`.

**Pending a bare-metal Linux run.** `perf` is Linux-only and the timing results
above were taken on native Windows (no WSL on this machine). The timing A/B
already shows the false-sharing cost (1.79× throughput, +50 ns p50); this table
is the counter-level evidence for *why*, and the rows stay open until the same
binaries run under Linux `perf` on real hardware — not WSL2, whose virtualised
PMU misreports exactly these events.

| Event | padded | unpadded |
|---|---|---|
| cache-misses | pending Linux run | pending Linux run |
| cache-references | pending Linux run | pending Linux run |
| L1-dcache-load-misses | pending Linux run | pending Linux run |
| L1-dcache-loads | pending Linux run | pending Linux run |
| context-switches | pending Linux run | pending Linux run |
| cache miss rate | pending Linux run | pending Linux run |
| L1-dcache miss rate | pending Linux run | pending Linux run |

### Core placement

Same benchmark, same padded binary, different placements — to show that the
queue's cost is really the memory path between two cores. (Cross-core rows are
the median-of-3 headline runs; the sibling and E-core rows are single full runs.)

| Placement | Cores used | `spsc_ring` p50 (ns) | `spsc_ring` messages/sec |
|---|---|---|---|
| SMT siblings, one physical core | 2, 3 | 50 | 780,207,691 (1.28 ns/op) |
| Two physical P-cores, one socket | 2, 4 | 100 | 78,635,764 (12.72 ns/op) |
| Two E-cores, shared L2 cluster | 8, 9 | 100 | 226,150,200 (4.42 ns/op) |
| Two sockets | n/a | — | single-socket laptop |

The spread is the whole lesson of section 3 in LEARN.md, measured. Between SMT siblings
the queue's cache lines never leave one core's L1, so a "message" costs about a
cycle-and-a-half of amortised work — 10× the cross-core throughput. The two
E-cores sit in one cluster sharing a 2 MB L2, so their hand-off (through L2)
lands between the other two: 3× the P-core-to-P-core rate despite the E-cores
being individually slower. Same binary, same algorithm — the only thing that
changed is how far a cache line has to travel.

---

## Design decisions

### Capacity is a power of two, and the index is masked

`idx = counter & (Capacity - 1)` instead of `counter % Capacity`. Integer division
on x86-64 is a multi-cycle, poorly pipelined instruction; `AND` is one cycle and
issues on nearly every port. The compiler will strength-reduce `%` into `AND`
*only* when it can prove the divisor is a power of two, so the constraint is a
`static_assert` rather than a hope. This also puts the wrap on the same execution
port as the rest of the address arithmetic, so it disappears into the load's
addressing mode.

### Counters are monotonic, never wrapped

`head_` and `tail_` count total pops and total pushes since construction, and are
masked only where an index is needed. Occupancy is then exactly `tail_ - head_`:
`0` is empty, `Capacity` is full, and there is no ambiguity to resolve.

The usual alternative — store already-wrapped indices — makes `head == tail` mean
both "empty" and "full", and the standard escape is to declare the buffer full one
slot early. That costs a slot (badly, when `Capacity` is small), and it costs a
branch on the hot path. Monotonic counters cost nothing and use the whole buffer.
The counters do eventually wrap at 2^64, and that is harmless: unsigned
subtraction is modular, so `tail_ - head_` stays correct across the wrap. At one
billion pushes per second it takes roughly 584 years to get there.

### Storage is a raw byte array with placement `new`

`std::array<T, Capacity>` would default-construct every slot at ring construction,
which is wrong for two reasons: it requires `T` to be default-constructible, and it
runs `Capacity` constructors for objects nobody asked for (and touches every page
of the buffer, which for a large ring is a lot of work at startup). The ring
instead holds `alignas(T) unsigned char storage_[Capacity * sizeof(T)]` and
constructs in place on push.

That makes lifetime management the ring's job, and it is handled in exactly two
places: `try_pop()` destroys the slot immediately after moving out of it, and
`~spsc_ring()` destroys precisely the slots in `[head_, tail_)` — not all of them,
and not none. `tests/test_spsc.cpp` counts constructor and destructor calls to
prove it, including the case where the live range wraps around the end of the
array.

### One counter per cache line — and a switch to turn that off

```
PADDED (default)                        UNPADDED (-DSPSC_DISABLE_PADDING)

+0   +-------------------------------+  +0   +-------------------------------+
     | tail_        (producer writes)|       | tail_        (producer writes)|
     | cached_head_ (producer only)  |       | cached_head_ (producer only)  |
     | ...padding to 64...           |       | head_        (consumer writes)|
+64  +-------------------------------+       | cached_tail_ (consumer only)  |
     | head_        (consumer writes)|       | ...rest of the line unused... |
     | cached_tail_ (consumer only)  |  +64  +-------------------------------+
     | ...padding to 64...           |       | storage_[...]                 |
+128 +-------------------------------+       |                               |
     | storage_[...]                 |       +-------------------------------+
     |                               |
     +-------------------------------+       one line, written by BOTH cores
```

In the padded layout the producer's writes land in line 0 and the consumer's in
line 1, so neither invalidates the other. In the unpadded layout they share a
line, and every push steals it from the consumer while every pop steals it back —
the coherence protocol ping-pongs a line whose *logical* contents the other side
does not even care about. See [LEARN.md section 3](LEARN.md#3-false-sharing-at-the-mesi-level).

The storage array gets its own line in both builds. Otherwise the first few
elements would share a line with `head_`, the producer writing element 0 would
invalidate the consumer's counter, and false sharing would leak in through the
payload — contaminating the very comparison the switch exists to make.

`alignas` uses `std::hardware_destructive_interference_size` where the library
provides it, falling back to 64. The class itself is cache-line aligned in both
builds, so the unpadded layout is deterministic rather than dependent on where the
allocator happened to place the object.

### Each side caches the other's index

The producer keeps a plain, non-atomic `cached_head_`. It only reloads the real
`head_` when `tail_ - cached_head_ == Capacity`, i.e. when the queue *looks* full.

This is safe because the cached value is always conservative: the consumer only
moves `head_` forward, so a stale `cached_head_` can only make the queue look
*fuller* than it is. The worst case is one wasted comparison followed by a real
load — never a lost slot, never an overwrite.

The win is that reading the real `head_` touches a cache line the consumer owns
and writes. Doing that on every push means a coherence transfer on every push. With
the cache, a producer that stays ahead of a keeping-up consumer touches the shared
line roughly once per lap around the buffer instead of once per message. The
consumer mirrors the trick with `cached_tail_`.

### Memory ordering

| Thread | Its own counter | The other counter | Publishing its counter |
|---|---|---|---|
| Producer | `tail_`: **relaxed** load | `head_`: **acquire** load | `tail_`: **release** store |
| Consumer | `head_`: **relaxed** load | `tail_`: **acquire** load | `head_`: **release** store |

- **Relaxed on your own counter** — you are its only writer, so nobody can
  surprise you with a different value. There is no race to prevent, and an
  acquire here would emit a compiler barrier for nothing.
- **Acquire on the other counter** — pairs with the other side's release store.
  Producer→consumer, it is what makes the payload written before the release
  visible after the acquire. Consumer→producer, it is what makes the slot's
  destructor finish before the producer constructs into that slot again.
- **Release on your own counter** — the store that publishes. Everything the
  thread did to the payload must be complete before the other side can observe
  the counter move, or the other side sees a slot advertised as ready and reads
  bytes that are not there yet.

Every one of these carries a one-line comment in `spsc_ring.hpp` naming the exact
race it prevents. The two-thread interleaving that a `relaxed` store would allow is
worked through step by step in [LEARN.md section 1](LEARN.md#1-why-relaxed-breaks-this-queue).

On x86-64 the acquire load and the release store both compile to a plain `mov` —
the hardware's TSO model gives them for free. They are not decoration: they stop
the *compiler* from reordering, which it will happily do, and they are what makes
the code correct on ARM and POWER where the hardware does reorder.

### The baseline is not sandbagged

`mutex_queue` is `std::queue` + `std::mutex` + two `std::condition_variable`s.
It holds the lock for exactly one `push`/`pop` and nothing else. It is measured
twice: once driven through the same `try_*` spin loop the ring gets (so the
harness is identical and only the queue differs), and once through its blocking
API (so it also gets to run the way it is actually good at). It also supports
multiple producers and consumers, which the ring does not — a real feature, not
overhead to sneer at.

Part of its cost is `std::deque` allocation. That is left in deliberately: it is
what the code people actually write does, and replacing it with a preallocated
buffer under the mutex would be measuring a different data structure.

### The histogram keeps every sample

`latency_hist` preallocates a `std::vector<uint64_t>` sized to the sample count,
writes into it by index, and sorts once at the end. Percentiles are nearest-rank
with no interpolation — an interpolated p99.99 is a value that was never measured.

`resize()` rather than `reserve()`, because resize also zero-fills, which faults
in every page before timing starts; otherwise the first write to each page would
take a page fault *inside the measured region*. `record()` never grows the vector;
if it runs out of room it counts the overflow and the benchmark reports it, because
a `realloc` mid-measurement would memcpy tens of megabytes into the tail you are
trying to measure.

Whether this harness suffers from coordinated omission — and what it measures
instead — is discussed at the top of `include/latency_hist.hpp` and in
[LEARN.md section 5](LEARN.md#5-why-the-mean-is-the-wrong-number).

---

## Building

Requires CMake ≥ 3.16 and a C++20 compiler. No other dependencies — no Boost, no
Google Benchmark, no HdrHistogram.

```sh
cmake -S . -B build
cmake --build build -j
```

Options:

| Option | Default | Effect |
|---|---|---|
| `-DCMAKE_BUILD_TYPE=` | `RelWithDebInfo` | `-O2 -g`; `-g` is what lets `perf report` attribute misses to source lines |
| `-DNANORING_NATIVE=ON` | `OFF` | adds `-march=native -mtune=native`; results stop being comparable across machines |
| `-DNANORING_SANITIZE=ON` | `OFF` | ThreadSanitizer. Correctness only — **never quote timings from this build** |

Each benchmark and the test suite are built twice:

```
build/bench_throughput_padded    build/bench_throughput_unpadded
build/bench_latency_padded       build/bench_latency_unpadded
build/test_spsc_padded           build/test_spsc_unpadded
```

Everything compiles clean under `-Wall -Wextra -Wpedantic`.

CI (badge above) runs on every push: the full test suite on GCC and Clang, both
layouts, plus a ThreadSanitizer build and run — so the correctness claims are
machine-checked on Ubuntu even though the performance numbers come from dedicated
hardware.

## Running

```sh
# tests (both layouts must pass identically)
ctest --test-dir build --output-on-failure

# throughput: producer core, consumer core, messages, warmup
./build/bench_throughput_padded   1 2 10000000 100000
./build/bench_throughput_unpadded 1 2 10000000 100000

# latency: client core, server core, samples, warmup
./build/bench_latency_padded   1 2 1000000 100000
./build/bench_latency_unpadded 1 2 1000000 100000

# hardware counters, both builds side by side (Linux)
bash bench/run_perf.sh 1 2 10000000 100000
```

Pick the two core IDs from `lscpu -e`. Two *physical* cores on one socket is the
realistic placement; SMT siblings of the same physical core are not adjacent IDs on
most machines, so do not guess.

## Platform notes

The target is Linux, and everything works there. The code builds and runs
elsewhere, with these caveats:

| | Linux | Windows (MinGW / MSVC) | macOS |
|---|---|---|---|
| Queue, tests, both benchmarks | yes | yes | yes |
| Core pinning | `pthread_setaffinity_np` | `SetThreadAffinityMask` (masks are 64 CPUs wide) | **no** — `thread_affinity_policy` is only a hint, so pinning is skipped and the benchmark warns |
| `bench/run_perf.sh` | yes | no (`perf` is Linux-only; use WSL2, or VTune/AMD uProf natively) | no (`Instruments`/`dtrace` instead) |
| ThreadSanitizer | yes | **no** (not supported by MinGW; MSVC has no TSan) | yes, with Clang |
| `steady_clock` resolution | true nanoseconds via the vDSO | ~100 ns (`QueryPerformanceCounter` granularity) | true nanoseconds |

The Windows clock granularity matters for `bench_latency`: a single hand-off can
easily be faster than one clock tick there, so individual samples get quantised to
100 ns steps. The benchmark measures and prints its own clock resolution in the
header so you can see this rather than be misled by it. Throughput numbers are
unaffected — they time millions of operations at once.

If you are on Windows and want the real numbers, run it under WSL2. Note that WSL2
is a VM: pinning works, but you are sharing physical cores with the host, so treat
the tail with suspicion and check `context-switches`.

## Getting numbers worth quoting

Latency tails are mostly a property of the machine, not the queue. Before
recording anything for a report:

```sh
lscpu -e                                   # confirm the topology of your two cores
cat /sys/devices/system/cpu/cpu1/topology/thread_siblings_list
sudo cpupower frequency-set -g performance # stop the governor from changing clocks mid-run
sudo cpupower idle-set -D 0                # keep cores out of deep C-states
```

Bigger hammers, in rough order of effect on the tail:

- `isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2` on the kernel command line — takes the
  benchmark cores away from the scheduler and stops the periodic tick on them.
- Move device interrupts off those cores (`/proc/irq/*/smp_affinity`).
- Disable turbo (`/sys/devices/system/cpu/intel_pstate/no_turbo`) so the clock does
  not drift between runs, or accept that it does and say so.
- Disable the SMT sibling of each benchmark core, or pin nothing else to it.
- `chrt -f 80` to run at real-time priority.
- Report `context-switches` from `perf stat` alongside every latency number. A run
  with more than a handful was sharing its cores, and its tail is not yours.

Run each configuration at least three times and report the spread. A single run of
a tail statistic is an anecdote.

## Repository layout

```
CMakeLists.txt              two build variants of every target, TSan + native options
include/spsc_ring.hpp       the wait-free SPSC ring buffer (header-only)
include/mutex_queue.hpp     std::queue + mutex + condvar baseline
include/latency_hist.hpp    preallocated sample collector, nearest-rank percentiles
bench/bench_throughput.cpp  messages/sec and ns/op, three scenarios
bench/bench_latency.cpp     ping-pong RTT/2 into a histogram, three scenarios
bench/run_perf.sh           perf stat A/B of the padded and unpadded builds
tests/test_spsc.cpp         assert-based tests, no framework
LEARN.md                    the concepts, from first principles, with counterexamples
```

## Known limits

- **SPSC only.** Exactly one producer thread and exactly one consumer thread. Two
  producers will corrupt the queue silently — there is no runtime check, because a
  check would cost more than the operation it guards. Making it MPMC means CAS
  loops and giving up wait-freedom; see [LEARN.md section 6](LEARN.md#6-why-spsc-is-wait-free-and-mpmc-is-not).
- **Bounded, and it does not block.** `try_push` fails when full. Backpressure is
  the caller's decision, which is the right place for it.
- **Closed-loop latency.** The ping-pong measures service time at queue depth one,
  not response time under a fixed arrival rate.
- **`size()` is approximate** while both threads run, and is for instrumentation
  only. Checking `size()` and then pushing is a TOCTOU bug; `try_push` re-checks.
- **Not tested on non-x86.** The ordering is written to be correct on any C++20
  implementation, but the only architecture it has actually been measured on is
  the one in the environment table above.
