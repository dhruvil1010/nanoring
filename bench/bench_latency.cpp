// ---------------------------------------------------------------------------
// bench_latency.cpp - how long does one message take to get from one core to
// another?
//
// Ping-pong: the client timestamps, enqueues a sequence number on the request
// queue, and waits for the server to echo it back on the response queue. The
// round trip is measured on one thread with one clock, so there is no
// cross-thread clock skew to correct for - the price is that we only ever learn
// the *round* trip and have to halve it to get a one-way number. That halving
// assumes the two directions are symmetric, which is true here (both directions
// are the same queue type between the same two cores) and would not be true if,
// say, the server did work before replying.
//
// Every sample goes into a preallocated histogram; nothing is averaged on the
// fly and nothing is thrown away. See latency_hist.hpp for the coordinated
// omission discussion - the short version is that this is a closed-loop test, so
// these are service times at queue depth one, not response times under load.
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "latency_hist.hpp"
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

// Only one message is ever in flight in a ping-pong, so capacity is almost
// irrelevant here - small keeps the whole working set inside L1 and stops the
// buffer itself from being the thing that misses.
constexpr std::size_t kCapacity = 64;
using message_t                 = std::uint64_t;

using spsc_t  = nano::spsc_ring<message_t, kCapacity>;
using mutex_t = nano::mutex_queue<message_t, kCapacity>;

using clock_type = std::chrono::steady_clock;

// See bench_throughput.cpp for the long version of why PAUSE and not yield().
inline void cpu_relax() noexcept {
#if defined(NANORING_HAVE_PAUSE)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

// Empty asm block that pretends to read `value` and to clobber memory, so the
// server's dequeue cannot be optimised away. `volatile` would not do: it forces
// an actual memory access into the loop rather than merely preventing the loop
// from being deleted, so it would inflate the very number we are measuring - and
// it provides no inter-thread ordering guarantee at all.
template <typename T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(value) : "memory");
#else
  static std::atomic<T> sink{};
  sink.store(value, std::memory_order_relaxed);
#endif
}

// ---------------------------------------------------------------------------
// Core pinning.
//
// Latency is where placement shows up most brutally, because the number is
// dominated by one cache line changing owner:
//   * Two SMT siblings on one physical core: the line never leaves L1 and the
//     hand-off is as cheap as it can possibly be - but the two threads are
//     fighting over one core's execution resources, so it is not a placement any
//     real system uses for a hot path.
//   * Two physical cores on one socket: the line is transferred between private
//     caches over the on-die interconnect. This is the realistic case and the
//     default here.
//   * Two sockets: every transfer crosses the inter-socket link and is resolved
//     by the home agent of the NUMA node that owns the memory. Expect a
//     different order of magnitude, and expect the tail to widen more than the
//     median.
// Without pinning the scheduler is free to move a thread mid-run, which turns
// one placement into a random mixture of all three and makes the tail
// uninterpretable. `lscpu -e` prints which core IDs are siblings; do not guess.
// ---------------------------------------------------------------------------
bool pin_current_thread(int core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(core), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
  if (core < 0 || core >= 64) {
    return false;
  }
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
  (void)core;
  return false;
#endif
}

struct config {
  int           client_core = 1;
  int           server_core = 2;
  std::uint64_t samples     = 1'000'000;
  std::uint64_t warmup      = 100'000;
};

void print_usage(const char* argv0) {
  std::printf(
      "usage: %s [client_core] [server_core] [samples] [warmup]\n"
      "  client_core  CPU id for the timing (ping) thread   (default 1)\n"
      "  server_core  CPU id for the echo (pong) thread     (default 2)\n"
      "  samples      recorded round trips per scenario     (default 1000000)\n"
      "  warmup       round trips discarded before timing   (default 100000)\n",
      argv0);
}

config parse_args(int argc, char** argv) {
  config cfg;
  if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
    print_usage(argv[0]);
    std::exit(0);
  }
  if (argc > 1) cfg.client_core = std::atoi(argv[1]);
  if (argc > 2) cfg.server_core = std::atoi(argv[2]);
  if (argc > 3) cfg.samples = std::strtoull(argv[3], nullptr, 10);
  if (argc > 4) cfg.warmup = std::strtoull(argv[4], nullptr, 10);

  if (cfg.samples == 0) {
    std::fprintf(stderr, "sample count must be > 0\n");
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

void print_environment(const config& cfg) {
  std::printf("=========================================================================\n");
  std::printf("nanoring :: latency (ping-pong, one-way = RTT/2)\n");
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
  std::printf("  client core    : %d\n", cfg.client_core);
  std::printf("  server core    : %d\n", cfg.server_core);
  std::printf("  queue capacity : %zu messages of %zu bytes (two queues)\n", kCapacity,
              sizeof(message_t));
  std::printf("  samples        : %llu (+ %llu warmup, discarded)\n",
              static_cast<unsigned long long>(cfg.samples),
              static_cast<unsigned long long>(cfg.warmup));
  std::printf("  clock          : std::chrono::steady_clock\n");
  std::printf("\n");
}

// ---------------------------------------------------------------------------
// Blocking vs spinning is a compile-time choice so the discarded branch is never
// even instantiated - spsc_ring has no push()/pop(), and if constexpr is what
// lets one template cover both queue types.
// ---------------------------------------------------------------------------
template <bool Blocking, typename Queue>
inline void send(Queue& queue, const message_t& value) {
  if constexpr (Blocking) {
    queue.push(value);
  } else {
    while (!queue.try_push(value)) {
      cpu_relax();
    }
  }
}

template <bool Blocking, typename Queue>
inline void receive(Queue& queue, message_t& out) {
  if constexpr (Blocking) {
    queue.pop(out);
  } else {
    while (!queue.try_pop(out)) {
      cpu_relax();
    }
  }
}

void print_result_header() {
  std::printf("all figures are nanoseconds, one way (round trip / 2)\n\n");
  std::printf("%-34s %10s %8s %8s %8s %9s %10s %10s\n", "scenario", "samples", "min", "p50", "p99",
              "p99.9", "p99.99", "max");
  std::printf("%-34s %10s %8s %8s %8s %9s %10s %10s\n", "----------------------------------",
              "----------", "--------", "--------", "--------", "---------", "----------",
              "----------");
}

void print_histogram_row(const char* name, const nano::latency_hist& hist) {
  std::printf("%-34s %10llu %8llu %8llu %8llu %9llu %10llu %10llu\n", name,
              static_cast<unsigned long long>(hist.count()),
              static_cast<unsigned long long>(hist.min()),
              static_cast<unsigned long long>(hist.p50()),
              static_cast<unsigned long long>(hist.p99()),
              static_cast<unsigned long long>(hist.p999()),
              static_cast<unsigned long long>(hist.p9999()),
              static_cast<unsigned long long>(hist.max()));
  if (hist.dropped() != 0) {
    std::fprintf(stderr, "warning: %llu samples dropped in '%s' (histogram undersized)\n",
                 static_cast<unsigned long long>(hist.dropped()), name);
  }
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
template <bool Blocking, typename Queue>
void run_pingpong(const char* name, const config& cfg) {
  auto requests  = std::make_unique<Queue>();
  auto responses = std::make_unique<Queue>();

  // Allocated, sized and page-faulted here, before a single timestamp is taken.
  nano::latency_hist hist(cfg.samples);

  std::atomic<int>  ready{0};
  std::atomic<bool> go{false};

  std::thread server([&] {
    if (!pin_current_thread(cfg.server_core)) {
      std::fprintf(stderr, "warning: could not pin server to core %d\n", cfg.server_core);
    }
    message_t value = 0;
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
      receive<Blocking>(*requests, value);
      do_not_optimize(value);
      send<Blocking>(*responses, value);
    }
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) {
      cpu_relax();
    }

    for (std::uint64_t i = 0; i < cfg.samples; ++i) {
      receive<Blocking>(*requests, value);
      do_not_optimize(value);
      send<Blocking>(*responses, value);
    }
  });

  std::thread client([&] {
    if (!pin_current_thread(cfg.client_core)) {
      std::fprintf(stderr, "warning: could not pin client to core %d\n", cfg.client_core);
    }
    message_t echoed = 0;
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
      send<Blocking>(*requests, i);
      receive<Blocking>(*responses, echoed);
    }
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) {
      cpu_relax();
    }

    for (std::uint64_t i = 0; i < cfg.samples; ++i) {
      const clock_type::time_point t0 = clock_type::now();
      send<Blocking>(*requests, i);
      receive<Blocking>(*responses, echoed);
      const clock_type::time_point t1 = clock_type::now();

      // A real check rather than an assert: asserts vanish under NDEBUG, which
      // is exactly the build we time. It also doubles as a use of `echoed` that
      // the optimiser cannot argue away.
      if (echoed != i) {
        std::fprintf(stderr, "FATAL: echo mismatch (sent %llu, got %llu)\n",
                     static_cast<unsigned long long>(i),
                     static_cast<unsigned long long>(echoed));
        std::abort();
      }

      const std::uint64_t rtt_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      // Integer halving loses at most half a nanosecond, which is far below the
      // clock's own resolution and its read overhead.
      hist.record(rtt_ns / 2);
    }
  });

  while (ready.load(std::memory_order_acquire) != 2) {
    cpu_relax();
  }
  go.store(true, std::memory_order_release);

  client.join();
  server.join();

  hist.finalize();
  print_histogram_row(name, hist);
}

// ---------------------------------------------------------------------------
// How expensive is the measurement itself? Two back-to-back clock reads tell us
// what one steady_clock::now() costs, and every ping-pong sample above contains
// roughly one such read inside it. Printed, never subtracted - subtracting a
// number from your results is how benchmarks start lying.
// ---------------------------------------------------------------------------
void probe_clock_overhead(const config& cfg) {
  if (!pin_current_thread(cfg.client_core)) {
    std::fprintf(stderr, "warning: could not pin main thread to core %d\n", cfg.client_core);
  }

  const std::uint64_t n = 100'000;
  nano::latency_hist   probe(n);
  std::uint64_t       smallest_step = 0;  // smallest non-zero gap ever observed

  for (std::uint64_t i = 0; i < n / 10; ++i) {  // warmup
    do_not_optimize(clock_type::now().time_since_epoch().count());
  }
  for (std::uint64_t i = 0; i < n; ++i) {
    const clock_type::time_point a = clock_type::now();
    const clock_type::time_point b = clock_type::now();
    const std::uint64_t          delta = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    if (delta != 0 && (smallest_step == 0 || delta < smallest_step)) {
      smallest_step = delta;
    }
    probe.record(delta);
  }
  probe.finalize();

  // The effective resolution is the interesting one. std::chrono advertises
  // nanosecond ticks on every platform, but the clock underneath does not
  // always deliver them - Windows' QueryPerformanceCounter typically steps in
  // 100 ns units, so on that platform every figure below is quantised to 100 ns
  // and a sub-100 ns hand-off is simply not observable one sample at a time.
  // Linux answers CLOCK_MONOTONIC from the vDSO with genuine nanosecond ticks.
  std::printf("  clock read cost: steady_clock::now() p50 = %llu ns, p99 = %llu ns"
              " (included in every sample below)\n",
              static_cast<unsigned long long>(probe.p50()),
              static_cast<unsigned long long>(probe.p99()));
  std::printf("  effective clock resolution: %llu ns"
              " (smallest non-zero gap between two consecutive reads)\n\n",
              static_cast<unsigned long long>(smallest_step));
}

}  // namespace

int main(int argc, char** argv) {
  const config cfg = parse_args(argc, argv);
  print_environment(cfg);
  probe_clock_overhead(cfg);

  char spsc_label[64];
  std::snprintf(spsc_label, sizeof(spsc_label), "spsc_ring (%s, spin)",
                nano::padding_enabled ? "padded" : "unpadded");

  print_result_header();
  run_pingpong<false, spsc_t>(spsc_label, cfg);
  run_pingpong<false, mutex_t>("mutex_queue (spin on try_*)", cfg);
  // The blocking scenario parks both threads on a condition variable every hop,
  // so each round trip pays a futex sleep and wake. It is much slower than the
  // spin variants by construction, which is the point of showing it: that cost
  // is what a spin loop buys you, and what it burns a core for.
  run_pingpong<true, mutex_t>("mutex_queue (blocking, cond var)", cfg);

  std::printf("\n");
  return 0;
}
