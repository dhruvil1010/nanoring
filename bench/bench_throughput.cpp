// ---------------------------------------------------------------------------
// bench_throughput.cpp - how many messages per second can one producer hand to
// one consumer?
//
// Three scenarios, all with the same message count, the same warmup, the same
// core placement and the same harness code:
//
//   1. spsc_ring, spin on try_push / try_pop
//   2. mutex_queue, spin on try_push / try_pop   (identical harness path)
//   3. mutex_queue, blocking push / pop          (its natural, best mode)
//
// Scenario 2 is the apples-to-apples comparison: same loop, same back-off, only
// the queue differs. Scenario 3 exists so the baseline is not judged solely on a
// spin loop that hammers its mutex - a condition variable is how you would
// actually write this, and it deserves to be measured that way.
//
// Nothing in this file prints a number that was not measured on the machine it
// is running on.
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "mutex_queue.hpp"
#include "spsc_ring.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>

#include <fstream>
#include <string>
#elif defined(_WIN32)
// Guarded: MinGW's libstdc++ already defines NOMINMAX, and redefining it is a
// warning we would otherwise have to explain away.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#define NANORING_HAVE_PAUSE 1
#endif

// Filled in by CMake so the output header describes the binary that produced it.
// Defaults keep a hand-rolled `g++ ...` build compiling.
#ifndef NANORING_COMPILER_ID
#define NANORING_COMPILER_ID "unknown compiler"
#endif
#ifndef NANORING_BUILD_FLAGS
#define NANORING_BUILD_FLAGS "unknown flags"
#endif
#ifndef NANORING_BUILD_TYPE
#define NANORING_BUILD_TYPE "unknown build type"
#endif

namespace {

constexpr std::size_t kCapacity = 1024;
using message_t                 = std::uint64_t;

using spsc_t  = nano::spsc_ring<message_t, kCapacity>;
using mutex_t = nano::mutex_queue<message_t, kCapacity>;

// ---------------------------------------------------------------------------
// Spin hint. PAUSE tells the core "this is a spin-wait": it de-pipelines the
// speculative loads that would otherwise pile up and eat the memory-order
// machine clear when the value finally changes, and it lets an SMT sibling have
// the execution ports. Yielding to the OS instead would cost a context switch,
// which is orders of magnitude more than the wait we are trying to ride out.
// ---------------------------------------------------------------------------
inline void cpu_relax() noexcept {
#if defined(NANORING_HAVE_PAUSE)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

// ---------------------------------------------------------------------------
// Optimiser barrier.
//
// The consumer pops values and does nothing with them. Without help, the
// compiler is entitled to notice that and delete the loop body - and then we
// would be benchmarking an empty loop. This asm block claims to read `value`
// and to clobber all of memory, so the compiler must materialise the value and
// must not move memory operations across it. It emits no instructions.
//
// Why not `volatile`? Because volatile is about *access*, not about ordering or
// visibility between threads. A volatile store forces a real store instruction,
// which adds a store to the measured loop that the real workload would not have
// - so it changes what you are measuring instead of protecting it. (Volatile
// also gives no atomicity and no happens-before, which is why it is the wrong
// tool for the queue itself as well.)
// ---------------------------------------------------------------------------
template <typename T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(value) : "memory");
#else
  // MSVC has no inline asm on x64. A relaxed store to a global atomic is the
  // portable fallback: the compiler cannot prove nobody reads it, so the value
  // has to be computed.
  static std::atomic<T> sink{};
  sink.store(value, std::memory_order_relaxed);
#endif
}

// ---------------------------------------------------------------------------
// Core pinning.
//
// Why it matters: without an affinity mask the scheduler moves threads around,
// and every migration drags the queue's cache lines to a new core - so the
// benchmark measures the scheduler as much as the queue, and reruns disagree
// with each other. Pinning makes the memory path a property of the experiment
// instead of an accident.
//
// What changes with placement:
//   * Same physical core (two SMT siblings): the two threads share one L1 and
//     one L2, so the queue's lines never leave the core - the fastest hand-off
//     available, but the threads also compete for the same execution ports, so
//     throughput of the *work* around the queue suffers.
//   * Same socket, different physical cores: the line moves between private L1s
//     via the on-die interconnect, usually arbitrated through the shared L3.
//     This is the placement a real producer/consumer pair should use, and the
//     one the default core IDs assume.
//   * Different sockets: the line has to cross UPI/Infinity-Fabric and be
//     resolved by the home agent of whichever NUMA node owns the memory - a
//     different order of magnitude. If your numbers look strange, check
//     `lscpu -e` and confirm the two core IDs really are on one socket.
// Sibling threads of one physical core are usually *not* adjacent core IDs -
// `lscpu -e` prints the real topology, so use it rather than assuming.
// ---------------------------------------------------------------------------
bool pin_current_thread(int core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(core), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
  if (core < 0 || core >= 64) {
    return false;  // a single affinity mask cannot address more than 64 CPUs
  }
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
  (void)core;
  return false;
#endif
}

struct config {
  int           producer_core = 1;
  int           consumer_core = 2;
  std::uint64_t messages      = 10'000'000;
  std::uint64_t warmup        = 100'000;
};

void print_usage(const char* argv0) {
  std::printf(
      "usage: %s [producer_core] [consumer_core] [messages] [warmup]\n"
      "  producer_core  CPU id to pin the producer to      (default 1)\n"
      "  consumer_core  CPU id to pin the consumer to      (default 2)\n"
      "  messages       messages per timed run             (default 10000000)\n"
      "  warmup         messages discarded before timing   (default 100000)\n",
      argv0);
}

config parse_args(int argc, char** argv) {
  config cfg;
  if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
    print_usage(argv[0]);
    std::exit(0);
  }
  if (argc > 1) cfg.producer_core = std::atoi(argv[1]);
  if (argc > 2) cfg.consumer_core = std::atoi(argv[2]);
  if (argc > 3) cfg.messages = std::strtoull(argv[3], nullptr, 10);
  if (argc > 4) cfg.warmup = std::strtoull(argv[4], nullptr, 10);

  if (cfg.messages == 0) {
    std::fprintf(stderr, "message count must be > 0\n");
    std::exit(1);
  }
  return cfg;
}

#if defined(__linux__)
std::string cpu_model() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string   line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos && colon + 2 <= line.size()) {
        return line.substr(colon + 2);
      }
    }
  }
  return "unknown";
}
#endif

// Everything a reader needs to reproduce the run, printed before any result.
void print_environment(const config& cfg) {
  std::printf("=========================================================================\n");
  std::printf("nanoring :: throughput\n");
  std::printf("=========================================================================\n");
  std::printf("  padding        : %s\n",
              nano::padding_enabled ? "ENABLED (head_/tail_ on separate cache lines)"
                                   : "DISABLED (head_/tail_ share a cache line)");
  std::printf("  cache line     : %zu bytes\n", nano::cache_line_size);
  std::printf("  compiler       : %s\n", NANORING_COMPILER_ID);
  std::printf("  build type     : %s\n", NANORING_BUILD_TYPE);
  std::printf("  flags          : %s\n", NANORING_BUILD_FLAGS);
  std::printf("  C++ standard   : %ld\n", static_cast<long>(__cplusplus));
#if defined(__linux__)
  std::printf("  cpu            : %s\n", cpu_model().c_str());
#endif
  std::printf("  logical cores  : %u\n", std::thread::hardware_concurrency());
  std::printf("  producer core  : %d\n", cfg.producer_core);
  std::printf("  consumer core  : %d\n", cfg.consumer_core);
  std::printf("  queue capacity : %zu messages of %zu bytes\n", kCapacity, sizeof(message_t));
  std::printf("  messages       : %llu (+ %llu warmup, discarded)\n",
              static_cast<unsigned long long>(cfg.messages),
              static_cast<unsigned long long>(cfg.warmup));
  std::printf("  clock          : std::chrono::steady_clock\n");
  std::printf("\n");
}

struct result {
  double        seconds  = 0.0;
  std::uint64_t messages = 0;
  bool          checksum_ok = false;
};

// ---------------------------------------------------------------------------
// The harness. Two pinned threads, a rendezvous so neither starts before the
// other is ready, a discarded warmup phase, then the timed run.
//
// Why warm up: the first pass through this code takes every cold-start cost
// there is - the code is not in the i-cache, the ring's pages are not faulted
// in, the branch predictors have never seen these branches, and the CPU may
// still be at its idle frequency. None of that is a property of the queue, and
// all of it lands in the first few thousand iterations.
//
// Why steady_clock: it is monotonic (never stepped by NTP or by the user), and
// on Linux it is clock_gettime(CLOCK_MONOTONIC), which the vDSO answers from
// user space without a syscall. The alternative is reading the TSC directly with
// rdtsc/rdtscp: fewer instructions and lower overhead, which matters when the
// interval you are timing is only tens of nanoseconds, but you then own all the
// problems - the TSC is a raw tick count needing calibration to nanoseconds, it
// is only comparable across cores if the CPU advertises invariant TSC, and rdtsc
// can be reordered by the out-of-order engine unless you fence it (rdtscp, or
// lfence;rdtsc). Rule of thumb: steady_clock for anything you time in bulk (this
// file), rdtscp when you must timestamp a single event inline in a hot path.
// ---------------------------------------------------------------------------
template <typename Queue, typename PushOp, typename PopOp>
result measure(Queue& queue, PushOp push_one, PopOp pop_one, const config& cfg) {
  std::atomic<int>  ready{0};
  std::atomic<bool> go{false};

  std::chrono::steady_clock::time_point t_begin{};
  std::chrono::steady_clock::time_point t_end{};
  std::uint64_t                         consumer_checksum = 0;

  std::thread producer([&] {
    if (!pin_current_thread(cfg.producer_core)) {
      std::fprintf(stderr, "warning: could not pin producer to core %d\n", cfg.producer_core);
    }
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
      push_one(queue, i);
    }
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) {
      cpu_relax();
    }

    t_begin = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < cfg.messages; ++i) {
      push_one(queue, i);
    }
  });

  std::thread consumer([&] {
    if (!pin_current_thread(cfg.consumer_core)) {
      std::fprintf(stderr, "warning: could not pin consumer to core %d\n", cfg.consumer_core);
    }
    message_t value = 0;
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
      pop_one(queue, value);
      do_not_optimize(value);
    }
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) {
      cpu_relax();
    }

    // Kept in a local the compiler can hold in a register: a captured variable
    // would have to be spilled to memory on every do_not_optimize() because of
    // the "memory" clobber, which would put a store in the measured loop.
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < cfg.messages; ++i) {
      pop_one(queue, value);
      do_not_optimize(value);
      sum += value;
    }
    t_end             = std::chrono::steady_clock::now();
    consumer_checksum = sum;
  });

  // The warmup also drains the queue, so the timed phase starts empty.
  while (ready.load(std::memory_order_acquire) != 2) {
    cpu_relax();
  }
  go.store(true, std::memory_order_release);

  producer.join();
  consumer.join();
  // join() is a synchronisation point, so reading t_begin/t_end/checksum here
  // is ordered after the threads' writes - no atomics needed for them.

  result r;
  r.messages = cfg.messages;
  r.seconds  = std::chrono::duration<double>(t_end - t_begin).count();
  // The producer sends 0..messages-1, so the consumer must have summed exactly
  // that. Proves nothing was dropped, duplicated or reordered while timing.
  const std::uint64_t expected = (cfg.messages - 1) * cfg.messages / 2;
  r.checksum_ok                = (consumer_checksum == expected);
  return r;
}

void print_result_header() {
  std::printf("%-34s %14s %12s %18s %12s\n", "scenario", "messages", "seconds", "messages/sec",
              "ns/op");
  std::printf("%-34s %14s %12s %18s %12s\n", "----------------------------------", "--------------",
              "------------", "------------------", "------------");
}

template <typename Queue, typename PushOp, typename PopOp>
void run_scenario(const char* name, const config& cfg, PushOp push_one, PopOp pop_one) {
  // Heap-allocated: the ring is cache-line aligned and several kilobytes, and
  // operator new is alignment-aware since C++17, so this respects alignas(64).
  auto queue = std::make_unique<Queue>();

  const result r = measure(*queue, push_one, pop_one, cfg);

  if (!r.checksum_ok) {
    std::fprintf(stderr, "FATAL: checksum mismatch in scenario '%s' - results are meaningless\n",
                 name);
    std::exit(2);
  }

  const double per_sec = static_cast<double>(r.messages) / r.seconds;
  const double ns_op   = (r.seconds * 1e9) / static_cast<double>(r.messages);
  std::printf("%-34s %14llu %12.4f %18.0f %12.2f\n", name,
              static_cast<unsigned long long>(r.messages), r.seconds, per_sec, ns_op);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  const config cfg = parse_args(argc, argv);
  print_environment(cfg);

  // Generic lambdas so one harness drives both queue types. `spin_*` is the
  // busy-wait path a latency-sensitive system uses (a dedicated core is cheaper
  // than a context switch); `blocking_*` uses the mutex queue's condition
  // variables and lets the OS park the thread.
  auto spin_push = [](auto& q, message_t v) {
    while (!q.try_push(v)) {
      cpu_relax();
    }
  };
  auto spin_pop = [](auto& q, message_t& out) {
    while (!q.try_pop(out)) {
      cpu_relax();
    }
  };
  auto blocking_push = [](auto& q, message_t v) { q.push(v); };
  auto blocking_pop  = [](auto& q, message_t& out) { q.pop(out); };

  char spsc_label[64];
  std::snprintf(spsc_label, sizeof(spsc_label), "spsc_ring (%s, spin)",
                nano::padding_enabled ? "padded" : "unpadded");

  print_result_header();
  run_scenario<spsc_t>(spsc_label, cfg, spin_push, spin_pop);
  run_scenario<mutex_t>("mutex_queue (spin on try_*)", cfg, spin_push, spin_pop);
  run_scenario<mutex_t>("mutex_queue (blocking, cond var)", cfg, blocking_push, blocking_pop);

  std::printf("\n");
  return 0;
}
