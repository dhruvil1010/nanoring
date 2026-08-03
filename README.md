# nanoring

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

**Every number below is `TODO_FILL_IN` because I have not run this on the machine
you are reading about.** The harness prints the values; paste them in. Nothing in
this repository contains an estimated, remembered, or illustrative measurement.

### Environment

| | |
|---|---|
| CPU | `TODO_FILL_IN` |
| Cores / threads | `TODO_FILL_IN` |
| Topology (`lscpu -e`) | `TODO_FILL_IN` |
| L1d / L2 / L3 | `TODO_FILL_IN` |
| Cache line | `TODO_FILL_IN` |
| RAM | `TODO_FILL_IN` |
| OS / kernel | `TODO_FILL_IN` |
| Compiler | `TODO_FILL_IN` |
| Flags | `TODO_FILL_IN` |
| Producer / consumer core | `TODO_FILL_IN` |
| Tuning applied (isolcpus, C-states, turbo, SMT) | `TODO_FILL_IN` |

### Throughput

10,000,000 `uint64_t` messages, 1024-slot queue, one producer, one consumer,
both pinned. Warmup discarded.

| Build | Scenario | messages/sec | ns/op |
|---|---|---|---|
| padded | `spsc_ring`, spin | `TODO_FILL_IN` | `TODO_FILL_IN` |
| padded | `mutex_queue`, spin on `try_*` | `TODO_FILL_IN` | `TODO_FILL_IN` |
| padded | `mutex_queue`, blocking | `TODO_FILL_IN` | `TODO_FILL_IN` |
| unpadded | `spsc_ring`, spin | `TODO_FILL_IN` | `TODO_FILL_IN` |

| Derived | Value |
|---|---|
| `spsc_ring` speedup over `mutex_queue` (spin) | `TODO_FILL_IN` |
| Padded speedup over unpadded (false-sharing cost) | `TODO_FILL_IN` |

### Latency

Ping-pong round trip, halved. 1,000,000 samples per scenario. All figures in
nanoseconds, one way.

| Build | Scenario | min | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|
| padded | `spsc_ring`, spin | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |
| padded | `mutex_queue`, spin | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |
| padded | `mutex_queue`, blocking | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |
| unpadded | `spsc_ring`, spin | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |

Measured `steady_clock::now()` cost, included in every sample above:
p50 `TODO_FILL_IN` ns, p99 `TODO_FILL_IN` ns.
Effective clock resolution: `TODO_FILL_IN` ns.

### Hardware counters (`bench/run_perf.sh`)

Same workload, both builds, counted with `perf stat`.

| Event | padded | unpadded |
|---|---|---|
| cache-misses | `TODO_FILL_IN` | `TODO_FILL_IN` |
| cache-references | `TODO_FILL_IN` | `TODO_FILL_IN` |
| L1-dcache-load-misses | `TODO_FILL_IN` | `TODO_FILL_IN` |
| L1-dcache-loads | `TODO_FILL_IN` | `TODO_FILL_IN` |
| context-switches | `TODO_FILL_IN` | `TODO_FILL_IN` |
| cache miss rate | `TODO_FILL_IN` | `TODO_FILL_IN` |
| L1-dcache miss rate | `TODO_FILL_IN` | `TODO_FILL_IN` |

### Core placement

Same benchmark, three placements, to show that the queue's cost is really the
memory path between two cores.

| Placement | Cores used | `spsc_ring` p50 (ns) | `spsc_ring` messages/sec |
|---|---|---|---|
| SMT siblings, one physical core | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |
| Two physical cores, one socket | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |
| Two sockets (if available) | `TODO_FILL_IN` | `TODO_FILL_IN` | `TODO_FILL_IN` |

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
does not even care about. See [LEARN.md §3](LEARN.md#3-false-sharing-at-the-mesi-level).

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
worked through step by step in [LEARN.md §1](LEARN.md#1-why-relaxed-breaks-this-queue).

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
[LEARN.md §5](LEARN.md#5-why-the-mean-is-the-wrong-number).

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
  loops and giving up wait-freedom; see [LEARN.md §6](LEARN.md#6-why-spsc-is-wait-free-and-mpmc-is-not).
- **Bounded, and it does not block.** `try_push` fails when full. Backpressure is
  the caller's decision, which is the right place for it.
- **Closed-loop latency.** The ping-pong measures service time at queue depth one,
  not response time under a fixed arrival rate.
- **`size()` is approximate** while both threads run, and is for instrumentation
  only. Checking `size()` and then pushing is a TOCTOU bug; `try_push` re-checks.
- **Not tested on non-x86.** The ordering is written to be correct on any C++20
  implementation, but the only architecture it has actually been measured on is
  the one in the environment table above.
