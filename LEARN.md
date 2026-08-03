# LEARN.md

Everything in `spsc_ring.hpp` that is not obvious, explained from the bottom up.

You already know C++. What you have probably never had to think about is that
*two threads do not agree on the order in which things happened*, and that the
smallest unit the hardware shares between cores is not a variable but a 64-byte
cache line. Those two facts generate every design decision in this repository.

A note on numbers: there are none here. Every timing in this project is something
you measure on your own machine and paste into `README.md`. The tables below use
logical step numbers (T1, T2, ...) to order events, not clock readings.

**Contents**

1. [Why `relaxed` breaks this queue](#1-why-relaxed-breaks-this-queue)
2. [What `acquire`/`release` actually guarantee](#2-what-acquirerelease-actually-guarantee)
3. [False sharing at the MESI level](#3-false-sharing-at-the-mesi-level)
4. [Why `head == tail` is ambiguous](#4-why-head--tail-is-ambiguous)
5. [Why the mean is the wrong number](#5-why-the-mean-is-the-wrong-number)
6. [Why SPSC is wait-free and MPMC is not](#6-why-spsc-is-wait-free-and-mpmc-is-not)
7. [Interview questions](#interview-questions)

---

## 1. Why `relaxed` breaks this queue

### From first principles

A push does two things:

```
(A)  construct the object into storage_[tail & mask]      // plain, non-atomic write
(B)  tail_ = tail + 1                                     // atomic write
```

and a pop does the mirror image:

```
(C)  t = tail_                                            // atomic read
(D)  read storage_[head & mask]                           // plain, non-atomic read
```

In your source, (A) is written before (B), and (C) before (D). Neither the
compiler nor the CPU is under any obligation to keep it that way, because *within
a single thread* the order does not matter — (A) and (B) touch different addresses,
so swapping them cannot change what that thread computes. Both are free to reorder
them, and both do:

- **The compiler** reorders when it schedules instructions, sinks stores past
  loop bodies, or hoists a load out of a branch. `-O2` does all three.
- **The CPU** reorders because stores go into a store buffer and drain to cache
  asynchronously, and because loads are issued speculatively as soon as their
  address is known. On x86-64 the drain order of stores is preserved (TSO), so
  (A)/(B) will not swap in hardware — but on ARM, POWER and RISC-V it will.

`memory_order_relaxed` means: *this operation is atomic (no torn reads) and
nothing more*. No ordering with respect to anything else. So a `relaxed` store to
`tail_` gives the compiler and the CPU explicit permission to do the reordering
above.

The consumer is on a different core. It has no idea any of this happened. It sees
whatever order the memory system chooses to show it.

### The broken code

```cpp
// BROKEN - do not ship this
template <typename U>
bool emplace(U&& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ == Capacity) {
        cached_head_ = head_.load(std::memory_order_relaxed);   // BUG
        if (tail - cached_head_ == Capacity) return false;
    }
    ::new (raw_slot(tail & kMask)) T(std::forward<U>(value));   // (A)
    tail_.store(tail + 1, std::memory_order_relaxed);           // BUG: (B) can land first
    return true;
}

bool try_pop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
        cached_tail_ = tail_.load(std::memory_order_relaxed);   // BUG: (C) creates no edge
        if (head == cached_tail_) return false;
    }
    T* cell = slot(head & kMask);                               // (D)
    out = std::move(*cell);
    cell->~T();
    head_.store(head + 1, std::memory_order_relaxed);           // BUG
    return true;
}
```

This compiles, passes every single-threaded test, and on an x86 laptop it will
very likely run for hours without visibly misbehaving. It is still wrong: the
plain write in (A) and the plain read in (D) are unsynchronised accesses to the
same memory from two threads, which the standard calls a data race and defines as
undefined behaviour. "It didn't crash" is not evidence.

### The exact interleaving

Ring of capacity 4. The previous lap left the value `41` in slot 0. The producer
is about to push `42`; `head_ == tail_ == 4` (both counters monotonic, so slot
index `4 & 3 == 0`).

| Step | Producer (core 0) | Consumer (core 1) | State visible to the consumer |
|---|---|---|---|
| T1 | loads `tail_` = 4 (its own counter, relaxed) | spinning in `try_pop`, `head_` = 4 | `tail_`=4 → looks empty, consumer keeps spinning |
| T2 | issues (A): store `42` into slot 0 — **sits in the store buffer**, or is scheduled later by the compiler | | slot 0 still holds `41` |
| T3 | issues (B): store `tail_` = 5 — **drains to cache first** | | `tail_`=5 |
| T4 | | loads `tail_` = 5 (relaxed) | `head_`=4 ≠ `tail_`=5 → "there is an item" |
| T5 | | reads slot 0 → gets **`41`** | the store from T2 has not landed |
| T6 | | `out = 41`, destroys the slot, stores `head_` = 5 | consumer has delivered a value the producer never sent |
| T7 | (A) finally drains: slot 0 = `42` | | too late — and it now overwrites a slot the consumer has already marked free |

Two independent bugs are visible in that table:

- **T2/T3 — store/store reordering.** The counter was published before the
  payload. The consumer read a slot that had not been written yet.
- **T4/T5 — load/load reordering.** Even if the producer's stores had landed in
  program order, nothing stops the consumer's read of the slot from being issued
  *before* its read of `tail_`. With `relaxed`, the compiler may legally emit:

  ```cpp
  T value = *slot(head & kMask);                              // hoisted above the check
  if (head == tail_.load(std::memory_order_relaxed)) return false;
  out = std::move(value);                                     // stale
  ```

  because a relaxed atomic load imposes no constraint on the ordinary loads around
  it.

With `T = int` the symptom is a silently wrong value. With `T = std::string` the
consumer reads a `std::string` whose internal pointer is whatever bytes happened to
be in the slot, then calls its destructor: a `free()` of a garbage pointer, at a
call site with no connection to the actual bug. This is why the failure mode of
memory-ordering bugs is "crash in an unrelated place, three weeks later, only in
production".

### The fix

```cpp
// producer publishes
::new (raw_slot(tail & kMask)) T(std::forward<U>(value));
tail_.store(tail + 1, std::memory_order_release);   // (A) cannot be reordered after this

// consumer observes
cached_tail_ = tail_.load(std::memory_order_acquire); // (D) cannot be reordered before this
```

`release` on the producer's store means: no memory operation sequenced before it
in this thread may be reordered after it. `acquire` on the consumer's load means:
no memory operation sequenced after it may be reordered before it. Together they
build a one-way barrier pair that pins (A) before (B) and (C) before (D), and —
crucially — establishes the *inter-thread* guarantee described in §2.

The same argument runs in the other direction for `head_`: the consumer's
`cell->~T()` must be finished before the producer's placement-`new` can start on
that slot, so the consumer's `head_` store is `release` and the producer's `head_`
load is `acquire`.

The loads of your *own* counter stay `relaxed`, and that is not a compromise —
it is exactly right. You are the only writer of that counter, so no other thread
can put a value there that you have not already seen. There is no race to prevent,
so there is nothing to pay for.

---

## 2. What `acquire`/`release` actually guarantee

### From first principles

The C++ memory model is built from three relations.

**Sequenced-before** — ordinary program order, *within one thread*. `a; b;` means
`a` is sequenced-before `b`.

**Synchronizes-with** — the only relation that crosses threads. A release
operation on an atomic object `M` *synchronizes-with* an acquire operation on the
same object `M` **if and only if the acquire reads the value written by that
release** (or a value later in its release sequence).

**Happens-before** — the transitive closure of the two. If `a` happens-before `b`,
then `b` is guaranteed to see the effects of `a`.

Put together, release/acquire gives you exactly this:

```
Producer thread                            Consumer thread

  construct slot[7] = 42          (A)
        |
        | sequenced-before
        v
  tail_.store(8, release)         (B)
        |
        |  synchronizes-with  (only because the load below reads the value 8)
        v
                                     tail_.load(acquire) == 8      (C)
                                           |
                                           | sequenced-before
                                           v
                                     read slot[7]                  (D)

  (A) happens-before (D)  =>  the read is guaranteed to see 42.
```

Three things people get wrong about this edge:

1. **It is per-object.** A release on `tail_` synchronizes only with an acquire on
   `tail_`. An acquire on `head_` creates no edge with it whatsoever.
2. **It is value-dependent.** The edge exists only if the acquire load actually
   *reads the value that the release stored*. If the consumer's load returns the
   old value 7, there is no edge — and that is fine, because it then concludes the
   queue is empty and never touches slot 7.
3. **It is one-way.** A release store does not stop *later* operations from moving
   *before* it. An acquire load does not stop *earlier* operations from moving
   *after* it. That asymmetry is why they are cheap: a full fence in both
   directions is what `seq_cst` costs you.

### The broken code

Three ways to write something that looks synchronised and is not.

```cpp
// BROKEN #1 - release with no acquire on the other side.
// The producer's half of the barrier is there; the consumer's is missing, so
// there is no synchronizes-with edge and therefore no happens-before.
tail_.store(tail + 1, std::memory_order_release);   // producer
...
cached_tail_ = tail_.load(std::memory_order_relaxed);  // consumer: no edge
```

```cpp
// BROKEN #2 - acquire on the wrong object.
// This creates an edge with whoever released head_ (the consumer itself), which
// says nothing at all about the payload the producer wrote.
cached_tail_ = tail_.load(std::memory_order_relaxed);
(void)head_.load(std::memory_order_acquire);   // wrong variable, useless edge
```

```cpp
// BROKEN #3 - fence on the wrong side of the load.
// The fence-based spelling of acquire is: relaxed load, THEN acquire fence.
// Putting the fence first orders the loads that came before it - which are not
// the ones that matter.
std::atomic_thread_fence(std::memory_order_acquire);      // too early
const std::size_t t = tail_.load(std::memory_order_relaxed);
T value = *slot(head & kMask);   // still free to be reordered above the load
```

### The fix

```cpp
// Either the explicit ordering on the operation ...
const std::size_t t = tail_.load(std::memory_order_acquire);

// ... or the fence spelling, with the fence AFTER the load whose value
// creates the edge. These two are equivalent here.
const std::size_t t = tail_.load(std::memory_order_relaxed);
std::atomic_thread_fence(std::memory_order_acquire);
```

### What this costs on real hardware

| | x86-64 | AArch64 | POWER |
|---|---|---|---|
| `load(acquire)` | `mov` | `ldar` | `lwz; lwsync` |
| `store(release)` | `mov` | `stlr` | `lwsync; stw` |
| `store(seq_cst)` | `xchg` (locked) or `mov; mfence` | `stlr` + `dmb ish` | `hwsync; stw` |

On x86-64 acquire and release are *free at the instruction level*: the hardware's
TSO model already forbids store/store and load/load reordering, so the compiler
emits a plain `mov`. They are not free at the compiler level, and that is the
entire point — the compiler is the reordering engine you are actually fighting on
x86. Writing `relaxed` and testing on x86 tells you nothing about whether the code
is correct, and everything about whether today's version of GCC happened to keep
your instructions in order.

Note the last row: `seq_cst` stores are the expensive ones, because sequential
consistency requires a single total order over *all* `seq_cst` operations in the
program, which the hardware can only provide by draining the store buffer. That is
why this queue never uses `seq_cst`, and why the default `memory_order` argument
being `seq_cst` is a trap for anyone who does not pass one explicitly.

---

## 3. False sharing at the MESI level

### From first principles

Two facts collide here:

1. Caches are coherent — if core 0 writes a location, core 1 will not keep reading
   a stale copy of it forever. The hardware guarantees this.
2. Coherence is tracked per **cache line** (64 bytes on x86-64), not per variable.
   The protocol has no idea your line contains two unrelated counters.

MESI is the protocol that provides fact 1. Every cache line, in every core's
private cache, is in one of four states:

| State | Meaning |
|---|---|
| **M**odified | This core has the only copy, and it is dirty. Memory is stale. |
| **E**xclusive | This core has the only copy, and it is clean. |
| **S**hared | Several cores may have clean copies. |
| **I**nvalid | This core does not have a usable copy. |

The rule that matters: **to write a line, a core must own it in M or E.** Getting
there means issuing a Request For Ownership (RFO), which invalidates every other
core's copy. There is exactly one writer per line at a time, always.

Now put `head_` and `tail_` on the same line, and watch what one push and one pop
cost:

| Step | Producer (core 0), writes `tail_` | Consumer (core 1), writes `head_` | Line state on core 0 / core 1 |
|---|---|---|---|
| T1 | — | reads `head_` | I / **S** |
| T2 | wants to write `tail_` → sends RFO | | invalidated on core 1 |
| T3 | writes `tail_` | | **M** / I |
| T4 | | reads `head_` → **miss**. Snoop finds the line M on core 0; it is written back / forwarded | S / **S** |
| T5 | | wants to write `head_` → sends RFO | invalidated on core 0 |
| T6 | | writes `head_` | I / **M** |
| T7 | reads `tail_` → **miss**, snoop, transfer | | **S** / S |
| T8 | wants to write `tail_` again → RFO | | back to T3 |

The line ping-pongs between the two cores forever, once per operation, and **not
one byte of the data either core cares about has changed**. Core 1 re-fetches the
line only to read a `head_` that only it ever writes. That is false sharing: a
performance coupling created purely by address layout.

Two second-order effects make it worse than "one extra cache miss":

- **The writer stalls too.** A store cannot retire until its line is owned. Under
  a ping-pong the store buffer backs up, and the producer — the one doing the
  writing — stalls just as much as the reader.
- **Memory-order machine clears.** On Intel, a core that has speculatively
  executed loads from a line which is then invalidated must flush the pipeline
  (`machine_clears.memory_ordering`). A tight spin loop over a contended line
  generates these constantly.

True sharing — two threads actually contending for the same variable — costs the
same coherence traffic, but at least it is buying you something. False sharing
buys nothing, which is why it is worth an `alignas`.

### The broken code

```cpp
// BROKEN - 16 bytes, guaranteed to land on one cache line
struct queue_indices {
    std::atomic<std::size_t> head;   // written only by the consumer
    std::atomic<std::size_t> tail;   // written only by the producer
};
```

The same bug in its other favourite disguise, per-thread statistics:

```cpp
// BROKEN - N counters packed into ~N/8 cache lines. Every thread's increment
// invalidates its neighbours' counters. Adding threads makes it slower.
std::atomic<std::uint64_t> per_thread_counter[64];
```

### The fix

```cpp
alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> tail_{0};
std::size_t cached_head_{0};          // producer-private, shares the producer's line

alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> head_{0};
std::size_t cached_tail_{0};          // consumer-private, shares the consumer's line
```

Note what goes *with* what. `cached_head_` is read and written only by the
producer, so it belongs on the producer's line — moving it there costs nothing and
saves a second cache line's worth of footprint. The grouping rule is **by writer**,
not by meaning: variables written by the same thread want to be together;
variables written by different threads must be apart.

`std::hardware_destructive_interference_size` is the standard's name for this
distance. It is 64 on x86-64, and 128 on some parts — Intel's adjacent-line
prefetcher pulls lines in pairs, so two variables 64 bytes apart can still
interfere. The ring falls back to a literal 64 if the library does not define the
constant.

The `SPSC_DISABLE_PADDING` build switch collapses this layout back to the broken
one. That is the whole experiment: build both, run both, and read the difference
out of `perf stat`.

### Seeing it

```sh
# The general shape: more cache-misses in the unpadded build for identical work.
bash bench/run_perf.sh 1 2 10000000 100000

# The specific tool. perf c2c finds cache lines that are being contended and
# tells you the offsets within the line, which is exactly a false-sharing report.
sudo perf c2c record -- ./build/bench_throughput_unpadded 1 2 10000000
sudo perf c2c report --stdio
```

---

## 4. Why `head == tail` is ambiguous

### From first principles

If your indices are stored already reduced modulo `Capacity`, they live in
`[0, Capacity)`. There are `Capacity` distinct values for each index, so
`Capacity²` distinct states — but the queue has `Capacity + 1` distinct occupancy
levels (0, 1, ..., Capacity), and only `Capacity` distinguishable differences
`(tail - head) mod Capacity`. One state has to do double duty, and the one that
does is `head == tail`: it means the queue is empty, and it also means the queue is
completely full, because after `Capacity` pushes the tail has wrapped all the way
back to the head. You have thrown away the one bit of information you needed.

### The broken code

```cpp
// BROKEN - wrapped indices with no way to tell full from empty
std::atomic<std::size_t> head_{0};   // in [0, Capacity)
std::atomic<std::size_t> tail_{0};   // in [0, Capacity)

bool empty() const { return head_ == tail_; }
bool full()  const { return head_ == tail_; }   // ... identical condition
```

Capacity 4, starting empty:

| Step | Action | `head` | `tail` | `head == tail`? | Reality |
|---|---|---|---|---|---|
| T1 | — | 0 | 0 | yes | empty — correct |
| T2 | push A | 0 | 1 | no | 1 item |
| T3 | push B | 0 | 2 | no | 2 items |
| T4 | push C | 0 | 3 | no | 3 items |
| T5 | push D | 0 | **0** | **yes** | **4 items — full, but it reads as empty** |
| T6 | push E | 0 | 1 | no | slot 0 silently overwritten; A is gone |

The producer thinks the queue is empty and keeps pushing; the consumer thinks the
queue is empty and refuses to pop. Data loss with no error anywhere.

### Fix A — sacrifice one slot

Declare the queue full one slot early, so `head == tail` can only ever mean empty.

```cpp
bool try_push(const T& v) {
    const std::size_t next = (tail_ + 1) & kMask;
    if (next == head_) return false;      // "full" - one slot always stays empty
    storage_[tail_] = v;
    tail_ = next;
    return true;
}
bool empty() const { return head_ == tail_; }   // now unambiguous
```

Correct, one line of code, and the standard answer in embedded C. The costs:

- You lose a slot. Usable capacity is `Capacity - 1`, which is a nasty surprise
  when `Capacity` is 2 (you get a one-item queue) and an annoying one when you
  sized the buffer to fit an exact burst.
- `size()` needs a conditional: `(tail - head + Capacity) & kMask`.
- The full-check needs the extra increment-and-mask before the comparison.

### Fix B — monotonic counters (what this project does)

Never wrap the counters at all. Count total pushes and total pops since
construction, and mask only when you need an index.

```cpp
std::atomic<std::size_t> head_{0};   // total pops   - unbounded
std::atomic<std::size_t> tail_{0};   // total pushes - unbounded

std::size_t size()  const { return tail_ - head_; }        // exact
bool        empty() const { return tail_ == head_; }
bool        full()  const { return tail_ - head_ == Capacity; }

T& slot(std::size_t counter) { return storage_[counter & kMask]; }
```

Empty is `0`, full is `Capacity`, and every slot is usable. There is no extra
branch and no lost capacity.

The obvious objection is overflow, and it is a non-issue twice over. First, a
64-bit counter at one billion pushes per second takes about 584 years to wrap.
Second, even if it did wrap, the code still works: unsigned arithmetic in C++ is
defined to be modulo 2^64, so `tail_ - head_` stays correct across the wrap as
long as the true difference is less than 2^64 — which it is, because it is at most
`Capacity`. (This is also why the counters must be *unsigned*. Signed overflow is
undefined behaviour, and the same code with `int64_t` would be broken.)

### Fix C — the trap

The third answer people give in interviews is "keep a separate `count_`":

```cpp
// Correct, and the wrong choice for SPSC.
std::atomic<std::size_t> count_{0};
// producer: count_.fetch_add(1, release);
// consumer: count_.fetch_sub(1, release);
```

It resolves the ambiguity, and it costs you the entire design. `count_` is written
by *both* threads, so it is a permanently contended cache line — the false sharing
of §3, except now it is true sharing and you cannot pad it away. Worse, both sides
now need a read-modify-write (`lock xadd`), which is an atomic bus/cache-locked
operation, where the whole point of the two-counter design is that each side does
a plain load and a plain store to a location nobody else writes. You have turned a
wait-free algorithm into a contended one to save a subtraction.

---

## 5. Why the mean is the wrong number

### From first principles

Latency distributions are not bell curves. They are a narrow spike (the common
path: everything in cache, nothing interrupted you) with a long, lumpy right tail,
where each lump is a *different physical event*:

- the scheduler ran something else on your core
- a timer interrupt or a device IRQ landed
- the core came out of a deep C-state and had to ramp up
- the frequency governor changed the clock mid-operation
- a TLB miss, or a page fault on a page you had not touched
- a cache line had to come from another socket
- (in a managed language) a GC pause

The mean mixes all of that into one scalar, where it is simultaneously dominated
by the spike (so it hides the tail) and dragged around by the tail (so it does not
describe the spike either). It answers no question anyone has.

Percentiles keep the modes separate. p50 tells you the common path. p99 tells you
what a busy but healthy system looks like. p99.9 and p99.99 are where the
scheduler, the interrupts and the C-states live — the events you can actually go
and fix.

### Why the tail matters more than its probability suggests

This is arithmetic, not a measurement. Suppose one user-visible action involves
100 independent hops, each with a 1% chance of exceeding its p99:

```
P(no hop exceeds p99) = 0.99^100 ≈ 0.366
```

So roughly **63% of user actions hit at least one p99 event**. Your "1% tail" is
the majority experience. At a fanout of 1000 hops, essentially every action hits
one. This is why service-level objectives are written at p99.9 or p99.99, and why
"the average is fine" is not an answer.

Put it in rate terms instead: a queue doing one million messages per second breaches
its p99.9 one thousand times per second. That is not an edge case, that is a
constant background of misses.

### The broken code

Two separate bugs, and they usually appear together.

```cpp
// BROKEN - the harness generates the tail it then reports
std::vector<std::uint64_t> samples;           // no reserve()
std::uint64_t total = 0;

for (int i = 0; i < N; ++i) {
    const auto t0 = clock::now();
    do_one_operation();
    const auto t1 = clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    samples.push_back(ns);       // BUG 1: reallocates ~log2(N) times, memcpy inside
                                 //        the measured region. Each realloc is a
                                 //        huge sample that belongs to your vector,
                                 //        not to the thing you are benchmarking.
    total += ns;
}

std::printf("average: %f ns\n", double(total) / N);   // BUG 2: reports only the mean,
                                                      //        which hides everything
```

Bug 1 is the subtle one. Growth happens *between* `t1` and the next `t0`, so the
`memcpy` is not inside a timed window — but it evicts the cache, dirties the
allocator, and may fault in new pages, so the *next several* samples are inflated.
You have injected artificial tail latency and then reported it as the queue's.

A third variant of the same mistake: computing p99 per shard or per second and
then averaging those p99s. Percentiles do not average. The mean of ten p99s is not
the p99 of the union, and there is no correction factor — you have to merge the
samples.

### The fix

```cpp
// Allocate and fault in every page before the first timestamp.
nano::latency_hist hist(N);          // resize(), not reserve() - see latency_hist.hpp

for (int i = 0; i < N; ++i) {
    const auto t0 = clock::now();
    do_one_operation();
    const auto t1 = clock::now();
    hist.record(ns_between(t0, t1));   // one compare, one store, one increment
}

hist.finalize();                       // sort once, at the end
// report min, p50, p99, p99.9, p99.99, max - and the sample count, so a reader
// can tell whether the p99.99 is backed by 100 samples or by 1.
```

Report the sample count next to the percentiles, always. A p99.99 computed from
10,000 samples rests on a single observation; it is a data point, not a statistic.
Rule of thumb: you need at least 100× the inverse of the percentile you are
quoting, so p99.99 wants a million samples minimum.

Also keep `min`. It is the only figure in the table that is nearly noise-free — it
is the machine's floor, the best the hardware can do when nothing goes wrong, and a
change in `min` between two builds is a real change in the code path rather than a
change in the environment.

---

## 6. Why SPSC is wait-free and MPMC is not

### The progress hierarchy

| Guarantee | Definition | Failure mode |
|---|---|---|
| **Blocking** | A thread holding a lock can stop everyone else. | Priority inversion, convoying, deadlock. If the holder is descheduled, everyone waits for the scheduler. |
| **Obstruction-free** | A thread makes progress if it runs alone for long enough. | Livelock under contention. |
| **Lock-free** | *Some* thread makes progress in a bounded number of steps, system-wide. | An individual thread can starve indefinitely while others succeed. |
| **Wait-free** | *Every* thread completes in a bounded number of its own steps. | None. This is the strongest guarantee. |

### Why this queue is wait-free

Look at what `try_push` does: two relaxed/acquire loads, a construct, one release
store. No loop, no retry, no CAS. It executes the same number of instructions
whether the consumer is running flat out, descheduled, or dead.

That is possible only because of a structural property: **each counter has exactly
one writer.** The producer owns `tail_`; the consumer owns `head_`. Neither ever
needs to atomically read-modify-write a value that someone else might change
underneath it, so neither ever needs to detect and retry a lost race. Take that
property away and the whole thing collapses.

### What breaks with two producers

```cpp
// BROKEN with more than one producer
const std::size_t tail = tail_.load(std::memory_order_relaxed);
::new (raw_slot(tail & kMask)) T(value);
tail_.store(tail + 1, std::memory_order_release);
```

| Step | Producer 1 | Producer 2 | Result |
|---|---|---|---|
| T1 | loads `tail_` = 7 | | |
| T2 | | loads `tail_` = 7 | both intend to use slot 7 |
| T3 | constructs `A` into slot 7 | | |
| T4 | | constructs `B` into slot 7 | `A` is overwritten without ever being destroyed |
| T5 | stores `tail_` = 8 | | |
| T6 | | stores `tail_` = 8 | one message lost, one slot leaked, and the counter is now wrong by one |

The read-then-write of `tail_` is not atomic as a unit, and with two writers that
is fatal. Producers must **claim** a slot atomically.

### MPMC option 1: `fetch_add` to claim

```cpp
const std::size_t pos = tail_.fetch_add(1, std::memory_order_relaxed);  // lock xadd
// this producer now exclusively owns slot (pos & mask)
```

`fetch_add` is itself wait-free — `lock xadd` always succeeds, it never retries.
But it creates a new problem: slots are now claimed out of order, so a consumer
seeing `tail_ == 10` cannot conclude that slots 0..9 are *filled*, only that they
are *claimed*. The producer that claimed slot 7 may still be descheduled with the
slot half-written.

The standard solution is Vyukov's bounded MPMC queue: give every slot its own
`std::atomic<size_t> sequence`. A producer may only write slot `i` when
`sequence == i`, and sets `sequence = i + 1` when done; a consumer may only read
it when `sequence == i + 1`, and sets `sequence = i + Capacity` when done. The
per-slot sequence *is* the ready flag, and it makes each slot's handoff
independent. The claim step becomes a CAS loop, so the queue is lock-free but no
longer wait-free.

### MPMC option 2: CAS loop

```cpp
std::size_t pos = tail_.load(std::memory_order_relaxed);
while (!tail_.compare_exchange_weak(pos, pos + 1,
                                    std::memory_order_acq_rel,
                                    std::memory_order_relaxed)) {
    // pos was refreshed with the current value; try again
}
```

Lock-free: some thread always wins each round, so the system makes progress. **Not**
wait-free: an unlucky thread can lose every round for an unbounded time. Under
heavy contention this degrades badly, because every failed CAS still took the cache
line exclusively and invalidated everyone else's copy — N threads spinning on one
CAS generate N times the coherence traffic to accomplish one operation.

### The ABA problem

CAS compares a *value* and infers "nothing changed". That inference is false if the
value can return to its old bit pattern. The classic victim is a Treiber stack over
a reused node pool:

```cpp
// BROKEN under node reuse
void pop() {
    node* old_head = head_.load(std::memory_order_acquire);
    while (old_head &&
           !head_.compare_exchange_weak(old_head, old_head->next)) {   // (*)
    }
    // ...
}
```

| Step | Thread 1 | Thread 2 | Stack |
|---|---|---|---|
| T1 | reads `head_` = **X**, reads `X->next` = **Y** | | X → Y → Z |
| T2 | *preempted before the CAS* | | X → Y → Z |
| T3 | | pops X | Y → Z |
| T4 | | pops Y | Z |
| T5 | | pushes X back (same address, from the free list) — and now `X->next` = **Z** | X → Z |
| T6 | resumes, CAS(`head_`, expected **X**, desired **Y**) → **succeeds**, because `head_` really is X again | | `head_` = **Y** |
| T7 | | | **Y is not in the stack.** It is on the free list. The stack now points into freed memory. |

Thread 1's CAS could not tell "unchanged" from "changed and changed back". The
pointer was the same; the world was not.

**Fixes:**

- **Tagged pointers / version counters** — store a monotonically increasing tag
  alongside the pointer and CAS both at once (`cmpxchg16b` on x86-64, or steal
  unused high pointer bits). The tag never repeats, so ABA cannot occur. This just
  moves the problem to "what if the tag wraps".
- **Hazard pointers** — each thread publishes the pointers it is about to
  dereference; memory is not reused while any thread has it hazarded.
- **Epoch-based reclamation / RCU** — retire nodes into an epoch and free them
  only once every thread has passed through a quiescent point. Lower per-operation
  cost than hazard pointers, higher memory footprint.
- **Do not reuse memory** — correct, and rarely practical.

**Why this ring is immune by construction.** ABA needs a value that can come back.
`head_` and `tail_` here are monotonic 64-bit counters that only ever increase, so
no value is ever seen twice. There is no CAS to fool, and no memory reclamation
problem either: slots are reused, but ownership of a slot is decided by the
counters rather than by comparing a pointer. Bounded queues with sequence counters
sidestep an entire category of lock-free hazard that unbounded node-based
structures have to engineer around — which is a large part of why bounded ring
buffers are what actually runs in latency-critical production systems.

---

## Interview questions

### 1. On x86-64, `store(release)` and `store(relaxed)` compile to the same `mov`. So why write `release`?

Because the compiler is a reordering engine too, and it obeys the memory model,
not the ISA. `relaxed` gives GCC and Clang permission to sink the payload store
below the counter store, hoist the payload load above the counter load, or keep a
value in a register across the operation. The barrier is real even when it emits
zero instructions.

Second: the source outlives the machine. The same code on AArch64 needs `stlr`
instead of `str`, and on POWER an `lwsync`. `release` is what makes the compiler
emit the right thing on each. Writing `relaxed` and testing on x86 does not test
the code, it tests today's optimiser.

Third: `-fsanitize=thread` and formal reasoning both work off the declared
ordering. `relaxed` is a claim you are making about the algorithm, and here the
claim would be false.

### 2. When is `seq_cst` actually necessary, and what does it cost?

`seq_cst` adds one thing over `acq_rel`: a single total order over all `seq_cst`
operations, agreed on by every thread. You need it only when a thread's behaviour
depends on the *relative order of two different atomic variables* as observed by a
third party.

The canonical cases are Dekker's algorithm (two flags, each thread stores its own
and loads the other; with `release`/`acquire` both can read `false` and both enter
the critical section, because store-load reordering is not prevented) and IRIW
(independent reads of independent writes: two readers can disagree about the order
of two independent writes unless everything is `seq_cst`).

The cost on x86-64 is on the store side: a `seq_cst` store must prevent
store→load reordering, which the hardware otherwise allows, so it compiles to a
locked `xchg` or `mov; mfence` — that is a store-buffer drain of tens of cycles,
on the critical path. Loads stay plain `mov`. On AArch64 you pay on both sides.

This queue needs no total order — it only ever needs "the payload before the
counter", which is exactly a release/acquire pair — so it never pays for one.

### 3. Why can't `volatile` replace atomics?

`volatile` means "do not elide or coalesce this access, it might have a side
effect". It was designed for memory-mapped I/O registers. It gives you three
things you need and two you do not have:

- It stops the compiler caching the value in a register (useful).
- It does not stop the compiler reordering the *non-volatile* accesses around it.
- It provides no ordering or visibility guarantee at the hardware level at all.
- It does not make the access atomic: a `volatile long long` on a 32-bit target
  can still tear.
- Two threads accessing a non-atomic `volatile` object is still a data race by
  the standard's definition, hence still undefined behaviour.

The practical demonstration is exactly §1's table: `volatile` on `tail_` prevents
none of it. In benchmark harnesses `volatile` is *also* the wrong tool for
defeating the optimiser, because it forces a real memory access into the loop and
therefore changes what you are measuring — an empty `asm volatile` with a memory
clobber prevents elision without adding an instruction.

### 4. What is false sharing, how does it differ from true sharing, and how would you find it in a running system?

False sharing is two threads writing *different* variables that happen to occupy
the same cache line: the coherence protocol serialises them because it tracks
ownership per line, and the line ping-pongs between cores with no data actually
being communicated. True sharing is two threads contending for the *same* variable
— the same traffic, but the communication is real and cannot be optimised away by
layout.

Finding it:

- `perf c2c record` / `perf c2c report` is the purpose-built tool. It reports
  contended cache lines, the offsets within the line, and which instructions and
  which cores are fighting over them — that offset breakdown is what distinguishes
  false from true sharing.
- `perf stat -e cache-misses,cache-references` on an A/B build (which is what
  `run_perf.sh` does here).
- On Intel, `mem_load_l3_hit_retired.xsnp_hitm` — a load that hit in L3 and found
  the line Modified in another core's cache — is close to a direct counter for
  cross-core line stealing.
- Structurally: look for hot variables written by different threads within 64
  bytes of each other. Per-thread counters in an array are the classic.

The fix is `alignas(hardware_destructive_interference_size)`, or grouping
per-writer, or making the counter thread-local and aggregating on read. The cost of
the fix is memory footprint, so it is not free: padding every field in a large
structure will blow up your cache footprint and make things slower.

### 5. Walk me through what happens in the coherence protocol when the producer publishes an item and the consumer picks it up.

Assume padded layout, both counters in separate lines, payload in a third.

1. Producer writes the payload into slot `i`. It needs that line in M — RFO if it
   does not already own it. Normally it does, because it wrote the previous slot in
   the same line, so this is an L1 hit.
2. Producer's release store to `tail_`. `tail_`'s line is in M in the producer's L1
   (the consumer only *reads* it, so the consumer's copies get invalidated on the
   producer's first write and, thanks to `cached_tail_`, are not re-requested on
   every item).
3. Consumer's acquire load of `tail_` misses — the line is M on the producer. The
   snoop resolves it: the line is transferred core-to-core (via L3 or a direct
   forward, depending on the microarchitecture) and ends up S on both.
4. Consumer reads the payload. That line was last written by the producer, so this
   is another cross-core transfer, again S on both.
5. Consumer's release store to `head_` — its own line, normally M and local.
6. Producer, when it eventually thinks the ring is full, acquires `head_`'s line.
   With `cached_head_` this happens roughly once per lap, not once per item.

So the steady-state cost per item is one line transfer for the payload plus an
amortised fraction of a line transfer for each counter. Batching helps because it
amortises steps 3 and 6 over more items in the same payload line.

### 6. The producer reads a cached, non-atomic copy of the consumer's index. Why is that not a race?

Because the cached value is only ever used to answer a question whose wrong answer
is safe. `cached_head_` is a value `head_` genuinely had at some point in the past,
and `head_` only increases, so `cached_head_ <= head_` always. The producer uses it
to decide "is the queue full?"; a stale (smaller) value can only make the queue
look *fuller* than it is. The failure mode is a false "full", and the code handles
it by reloading the real atomic and re-checking. It can never produce a false "there
is room", which is the answer that would corrupt the queue.

It is not a data race in the standard's sense either: `cached_head_` is touched by
one thread only. The cross-thread communication happens entirely through the
atomic `head_`; the cache is a private memo of a value that was legitimately
acquired.

The win: reading `head_` touches a line the consumer writes. Doing that per push is
a coherence transfer per push. With the cache, the producer touches it about once
per lap around the buffer.

### 7. How would you extend this to MPMC, and what would you give up?

Producers can no longer own `tail_`, so slot acquisition must become atomic. The
standard construction is Vyukov's bounded MPMC queue: each slot carries its own
`std::atomic<size_t> sequence`; a producer CASes `tail_` to claim ticket `i`,
writes the slot, then stores `sequence = i + 1` with release; a consumer CASes
`head_` for ticket `i`, spins until `sequence == i + 1` with acquire, reads, then
stores `sequence = i + Capacity`. The per-slot sequence decouples the slots so
producers are not blocked by a slow neighbour.

What you give up:

- **Wait-freedom.** The claim is a CAS loop, so it is lock-free only.
- **Throughput under contention.** Every failed CAS still took the counter's line
  exclusively; N spinning threads generate N times the coherence traffic per
  successful operation.
- **The simple ordering story.** You now have three atomics interacting instead of
  two, and the correctness argument is much harder to make by inspection.

If only one side needs to be multi, do only that side: MPSC keeps the consumer's
plain `head_` and only makes the producer side atomic, which is meaningfully
cheaper than full MPMC. Always ask whether sharding into N SPSC queues (one per
producer, consumer round-robins) is acceptable — it usually is, and it keeps every
queue wait-free.

### 8. What is coordinated omission, and does your harness have it?

Coordinated omission is the measurement error where the load generator's own
stalls delete the samples that would have looked worst. If you intend to send one
request per millisecond and one request takes 200 ms, you fail to send the ~200
requests that were due during the stall. Each of those would have queued behind it
and recorded 200 ms, 199 ms, 198 ms... Instead you record one bad sample and skip
the 200 that inherited the damage, so your p99.9 looks fine while every user
behind that stall did not agree.

This harness is a closed-loop ping-pong: iteration N+1 starts when N finishes, and
there is no intended schedule, so nothing is ever skipped and classic coordinated
omission cannot occur. The honest caveat is what it therefore does *not* measure —
these are service times at a queue depth of one, not response times under a
sustained arrival rate, where queueing delay is what dominates the tail.

To measure that instead: compute the intended send time of every request up front
(`start + i * interval`), sleep or spin until it arrives, send, and record
`completion - intended_send_time`, not `completion - actual_send_time`. If the
producer falls behind, the difference between those two definitions is exactly the
omitted latency.

### 9. `rdtsc` versus `clock_gettime` versus `std::chrono::steady_clock` — when do you use each?

`steady_clock` on Linux is `clock_gettime(CLOCK_MONOTONIC)`, answered from the vDSO
without a syscall. It is monotonic, in real nanoseconds, comparable across cores
and across processes, and it costs tens of nanoseconds per call. It is the right
default and it is what this project uses.

`rdtsc`/`rdtscp` read the timestamp counter directly: a handful of cycles, no
function call. You want it when the interval you are timing is itself only tens of
nanoseconds, because otherwise the clock read is a large fraction of the
measurement. The costs you take on:

- It counts **ticks, not nanoseconds**, at the CPU's base (not current) frequency.
  You must calibrate against a real clock to convert, and quote the calibration.
- Cross-core comparison requires **invariant TSC** (`constant_tsc` +
  `nonstop_tsc` in `/proc/cpuinfo`), and even then the TSC must have been
  synchronised at boot. Timestamping on one core and subtracting on another is only
  valid if you have checked.
- `rdtsc` is **not ordered** against surrounding instructions; the out-of-order
  engine can move it. Use `rdtscp` (which waits for prior instructions to retire)
  or `lfence; rdtsc`, and be aware that the serialisation itself costs cycles and
  perturbs what you are measuring.
- In a VM, `rdtsc` may trap to the hypervisor, at which point it is far slower than
  `clock_gettime`.

Rule of thumb: `steady_clock` for anything measured in bulk or across threads;
`rdtscp` for timestamping single events inline in a hot path where you control the
calibration and the affinity. Never `system_clock` for durations — it is wall
time and NTP can step it backwards mid-measurement.

### 10. I run your benchmark twice and get different p99.9 values. What do you check, in order?

Assume the code is fixed and only the environment varies.

1. **Was anything else on those cores?** `perf stat` reports `context-switches`
   and `cpu-migrations`; a run with more than a handful is not clean. Fix with
   `taskset`/affinity plus `isolcpus=` and `nohz_full=` on the kernel command line.
2. **Did the clock frequency move?** Turbo and the `powersave` governor change the
   clock underneath you, and a core that was idle ramps up over the first
   milliseconds. Pin the governor to `performance`, consider disabling turbo, and
   warm up before recording.
3. **C-states.** A core that entered a deep idle state has to wake up, and that
   wakeup lands squarely in the tail of a spin-wait benchmark. `cpupower idle-set
   -D 0`, or keep the core busy.
4. **Interrupts.** Check `/proc/interrupts` for the benchmark cores and move device
   IRQs elsewhere via `smp_affinity`. A NIC queue bound to your consumer core will
   show up as a periodic tail.
5. **Topology.** Confirm with `lscpu -e` that the two core IDs are what you think —
   whether they are SMT siblings, on the same socket, or on different NUMA nodes
   changes the answer by an order of magnitude, and core numbering is not
   consistent across machines.
6. **Memory placement.** On a multi-socket box, which node the ring was allocated on
   matters. `numactl --membind` to make it deterministic.
7. **THP and page faults.** First-touch page faults inside the measured region show
   up as tail. The histogram here is `resize()`d up front for exactly this reason;
   check that anything else on the hot path is pre-faulted too.
8. **Sample count and the statistic itself.** A p99.9 over 10,000 samples rests on
   ten observations. Some of the variance is not the machine, it is you quoting a
   statistic the sample size does not support.

Then re-run each configuration at least three times and report the spread rather
than one number. A single measurement of a tail statistic is an anecdote.
