// ---------------------------------------------------------------------------
// latency_hist.hpp - sample collector for the latency benchmark.
//
// Not a histogram in the bucketed HdrHistogram sense: it keeps every raw sample
// and sorts once at the end. That is the simplest thing that cannot lie. Bucket
// widths quantise the tail, and quantising the tail is precisely the mistake
// this project exists to avoid. The cost is memory - 8 bytes per sample, so ten
// million samples is 80 MB - which is fine for an offline benchmark and would
// not be fine for a server that reports percentiles continuously.
//
// ---------------------------------------------------------------------------
// COORDINATED OMISSION
// ---------------------------------------------------------------------------
// Coordinated omission is the measurement bug where the load generator's own
// stalls delete the samples that would have looked worst. The classic shape:
// you intend to send one request every millisecond; one request takes 200 ms;
// while you sit waiting for it you fail to send the ~200 requests that were due
// in the meantime. Those requests would each have queued behind the stall and
// recorded 200 ms, 199 ms, 198 ms ... Instead you record one 200 ms sample and
// 199 zeros' worth of nothing. Your p99.9 comes out looking healthy, and the
// user - who did experience the stall - does not agree.
//
// Does THIS harness suffer from it? Plainly: no, and also it does not fix it.
//
//   * It cannot suffer from classic coordinated omission because bench_latency
//     is a closed-loop ping-pong with no intended send schedule. Iteration N+1
//     is not "due" at any wall-clock time; it starts when iteration N finishes.
//     Nothing is ever skipped, so nothing is ever omitted.
//   * What that buys is honest *service time* under a queue depth of exactly
//     one. What it does not measure is *response time* under sustained arrival
//     rate, where queueing delay dominates the tail. If you need that number,
//     drive the producer from a fixed-rate schedule computed up front and record
//     (completion_time - intended_send_time), not (completion - actual_send).
//
// So: the numbers this produces are real, and they answer "how long does one
// round trip take when the pipe is empty", not "what does my p99.9 look like at
// two million messages a second".
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nano {

class latency_hist {
 public:
  // resize(), not reserve(): resize also writes zeros across the whole buffer,
  // which faults in every page before timing starts. reserve() alone would leave
  // the pages unmapped and the first write to each would take a page fault
  // *inside the measured region* - a tail artefact of the harness that would be
  // indistinguishable from a tail artefact of the queue.
  explicit latency_hist(std::size_t expected_samples)
      : samples_(expected_samples, 0) {}

  // The only thing that runs on the measured path. One compare, one store, one
  // increment - no allocation, no branch that depends on data, no I/O.
  void record(std::uint64_t nanoseconds) {
    if (count_ < samples_.size()) {
      samples_[count_++] = nanoseconds;
    } else {
      // Never silently grow. A realloc here would memcpy tens of megabytes in
      // the middle of the measurement; counting the overflow instead lets the
      // benchmark report that it happened. (An assert would not do: NDEBUG is
      // defined in the optimised builds we actually time.)
      ++dropped_;
    }
  }

  // Call once, after the timed loop. Sorting is O(n log n) and allocates
  // nothing; doing it here rather than incrementally is what keeps record()
  // cheap.
  void finalize() {
    samples_.resize(count_);
    std::sort(samples_.begin(), samples_.end());
    finalized_ = true;
  }

  // Everything below reads the sorted array, so it is only meaningful after
  // finalize(). finalized() is exposed so a caller can check rather than guess.
  bool          finalized() const noexcept { return finalized_; }
  std::size_t   count() const noexcept { return count_; }
  std::uint64_t dropped() const noexcept { return dropped_; }

  std::uint64_t min() const { return empty() ? 0 : samples_.front(); }
  std::uint64_t max() const { return empty() ? 0 : samples_.back(); }

  std::uint64_t p50() const { return percentile(0.50); }
  std::uint64_t p99() const { return percentile(0.99); }
  std::uint64_t p999() const { return percentile(0.999); }
  std::uint64_t p9999() const { return percentile(0.9999); }

  double mean() const {
    if (empty()) {
      return 0.0;
    }
    // A uint64 accumulator is enough: even a billion samples of a billion
    // nanoseconds each stays under 2^64. Dividing once at the end avoids the
    // rounding drift of a running average.
    std::uint64_t sum = 0;
    for (const std::uint64_t v : samples_) {
      sum += v;
    }
    return static_cast<double>(sum) / static_cast<double>(samples_.size());
  }

  // Nearest-rank percentile: the smallest recorded value v such that at least
  // `fraction` of the samples are <= v. No interpolation between neighbours -
  // an interpolated p99.99 is a number that was never measured, and inventing
  // numbers is the one thing a latency report must not do.
  std::uint64_t percentile(double fraction) const {
    if (empty()) {
      return 0;
    }
    const double n    = static_cast<double>(samples_.size());
    std::size_t  rank = static_cast<std::size_t>(std::ceil(fraction * n));
    if (rank < 1) {
      rank = 1;
    }
    if (rank > samples_.size()) {
      rank = samples_.size();
    }
    return samples_[rank - 1];
  }

 private:
  bool empty() const { return samples_.empty(); }

  std::vector<std::uint64_t> samples_;
  std::size_t                count_{0};
  std::uint64_t              dropped_{0};
  bool                       finalized_{false};
};

}  // namespace nano
