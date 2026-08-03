// ---------------------------------------------------------------------------
// mutex_queue.hpp - the honest baseline.
//
// std::queue guarded by std::mutex, with condition variables so a caller can
// block instead of spin. This is what most production code actually contains,
// and it is what the lock-free ring has to beat to be worth its complexity.
//
// It is deliberately NOT handicapped:
//   * The critical sections hold nothing but a push_back / pop_front - there is
//     no work done under the lock that could be hoisted out.
//   * Both a blocking API (push/pop, condition-variable driven) and a
//     non-blocking one (try_push/try_pop) are provided, so the benchmarks can
//     compare like with like on the spin path *and* let this queue run in its
//     natural blocking mode, which is the mode it is actually good at.
//   * The container is a plain std::queue<T>. Swapping in a preallocated buffer
//     under the mutex would measure a different data structure. Part of this
//     queue's cost really is std::deque's allocation traffic, and pretending
//     otherwise would flatter it.
//
// It supports multiple producers and consumers, which the ring does not. That
// is a genuine feature, not overhead to be apologised for - keep it in mind
// before concluding the ring is strictly better.
// ---------------------------------------------------------------------------
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace nano {

template <typename T, std::size_t Capacity>
class mutex_queue {
  static_assert(Capacity >= 1, "a zero-capacity queue can never hold anything");

 public:
  using value_type = T;

  mutex_queue() = default;

  mutex_queue(const mutex_queue&)            = delete;
  mutex_queue& operator=(const mutex_queue&) = delete;
  mutex_queue(mutex_queue&&)                 = delete;
  mutex_queue& operator=(mutex_queue&&)      = delete;

  // ---- non-blocking API, mirrors spsc_ring exactly -------------------------
  bool try_push(const T& value) { return try_emplace(value); }
  bool try_push(T&& value) { return try_emplace(std::move(value)); }

  bool try_pop(T& out) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop();
    // Notify while still holding the lock. Releasing first would be marginally
    // friendlier to the woken thread, but it opens a window where a blocked
    // producer misses the wakeup unless the predicate is re-checked - and the
    // predicate re-check is exactly what wait(lock, pred) already does, so this
    // is correct either way and simpler to read.
    not_full_.notify_one();
    return true;
  }

  // ---- blocking API --------------------------------------------------------
  // No shutdown/close path: the benchmarks always know exactly how many
  // messages will flow, so nobody is ever left waiting forever. Real code would
  // need a `close()` that wakes every waiter and makes pop() return false.
  void push(const T& value) { emplace_blocking(value); }
  void push(T&& value) { emplace_blocking(std::move(value)); }

  void pop(T& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    // The predicate overload of wait() re-checks after every wakeup, which is
    // what makes spurious wakeups and lost-notify races harmless.
    not_empty_.wait(lock, [this] { return !queue_.empty(); });
    out = std::move(queue_.front());
    queue_.pop();
    not_full_.notify_one();
  }

  // ---- observers -----------------------------------------------------------
  std::size_t size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  bool empty() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  template <typename U>
  bool try_emplace(U&& value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() == Capacity) {
      return false;
    }
    queue_.push(std::forward<U>(value));
    not_empty_.notify_one();
    return true;
  }

  template <typename U>
  void emplace_blocking(U&& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this] { return queue_.size() < Capacity; });
    queue_.push(std::forward<U>(value));
    not_empty_.notify_one();
  }

  // mutable so the const observers can still lock. The mutex is not part of the
  // queue's logical state, so locking it does not violate constness.
  mutable std::mutex      mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::queue<T>           queue_;
};

}  // namespace nano
