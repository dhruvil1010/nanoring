// ---------------------------------------------------------------------------
// spsc_ring.hpp - a wait-free single-producer / single-consumer ring buffer.
//
// "Single producer, single consumer" is a hard contract, not a suggestion:
// exactly one thread may ever call try_push(), exactly one other thread may ever
// call try_pop(). Those two threads never touch the same counter as writers, so
// no atomic read-modify-write instruction (lock xadd, lock cmpxchg, ...) appears
// anywhere in this file. Every operation finishes in a bounded number of steps
// no matter what the other thread is doing - that is what "wait-free" means, and
// it is the property MPMC queues cannot have (see LEARN.md section 6).
//
// Compile-time switch:
//   SPSC_DISABLE_PADDING  collapses head_ and tail_ onto the same cache line.
//                         Defining it does not change behaviour, only layout -
//                         it exists so the false-sharing experiment can be an
//                         A/B of two binaries built from identical source.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace nano {

// ---------------------------------------------------------------------------
// Cache line size
// ---------------------------------------------------------------------------
// std::hardware_destructive_interference_size is the standard's name for "how
// far apart two objects must be so that touching one does not disturb the
// other". On x86-64 that is 64 bytes; some ARM cores pair lines and want 128.
#if defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
// GCC 12+ warns that this constant is not ABI-stable, because its value follows
// -mtune. That matters when the constant leaks into a shipped library's layout;
// here the ring is header-only and both sides of every comparison are built from
// the same headers with the same flags, so the warning is noise. Silenced
// locally rather than project-wide so the real diagnostics stay on.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
inline constexpr std::size_t cache_line_size =
    std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic pop
#endif
#else
// Pre-C++17 library, or libc++ which still does not define the feature-test
// macro. 64 is right for every x86-64 part and is a safe over-estimate nowhere.
inline constexpr std::size_t cache_line_size = 64;
#endif

#if defined(SPSC_DISABLE_PADDING)
// Natural alignment: the two counters end up adjacent, i.e. on one line.
inline constexpr std::size_t index_alignment = alignof(std::atomic<std::size_t>);
inline constexpr bool        padding_enabled = false;
#else
// One counter per line: the producer's writes and the consumer's writes land in
// different coherence units and stop invalidating each other.
inline constexpr std::size_t index_alignment = cache_line_size;
inline constexpr bool        padding_enabled = true;
#endif

// ---------------------------------------------------------------------------
// spsc_ring
// ---------------------------------------------------------------------------
// The class itself is cache-line aligned in both build variants. In the padded
// build the member alignas() would force that anyway; in the unpadded build it
// is what makes the experiment deterministic - without it the object could start
// mid-line and the two counters might accidentally straddle a line boundary and
// land apart, which is the effect we are trying to *remove*.
template <typename T, std::size_t Capacity>
class alignas(cache_line_size) spsc_ring {
  // A power-of-two capacity turns "wrap the index" into a bit mask. Modulo by a
  // runtime value compiles to a hardware divide, which on x86-64 has a latency
  // in the tens of cycles and is not pipelined; `& (Capacity - 1)` is a single
  // one-cycle AND. The compiler can only strength-reduce % into an AND when it
  // can prove the divisor is a power of two, so we make it a static_assert
  // instead of hoping.
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so the index wrap is a bit mask");
  static_assert(Capacity >= 2, "a one-slot ring is a mailbox, not a queue");
  static_assert(std::is_nothrow_destructible_v<T>,
                "a throwing destructor would leave the ring in a half-popped state");

  static constexpr std::size_t kMask = Capacity - 1;

 public:
  using value_type = T;

  spsc_ring() = default;

  ~spsc_ring() {
    // Only the slots in [head_, tail_) ever had an object constructed in them,
    // so only those get destroyed. Relaxed loads are fine: destroying an object
    // while another thread still uses it is undefined behaviour regardless of
    // memory order, so by the time we are here the ring is single-threaded.
    if constexpr (!std::is_trivially_destructible_v<T>) {
      const std::size_t head = head_.load(std::memory_order_relaxed);
      const std::size_t tail = tail_.load(std::memory_order_relaxed);
      for (std::size_t i = head; i != tail; ++i) {
        slot(i & kMask)->~T();
      }
    }
  }

  // Copying a queue that two threads are actively using is meaningless, and
  // moving one would move the counters out from under those threads. Deleted so
  // the mistake is a compile error rather than a Heisenbug.
  spsc_ring(const spsc_ring&)            = delete;
  spsc_ring& operator=(const spsc_ring&) = delete;
  spsc_ring(spsc_ring&&)                 = delete;
  spsc_ring& operator=(spsc_ring&&)      = delete;

  // ---- producer side -------------------------------------------------------
  // Both overloads exist so a caller with an rvalue gets a move and a caller
  // with an lvalue gets a copy, without the queue ever silently copying
  // something expensive.
  bool try_push(const T& value) { return emplace(value); }
  bool try_push(T&& value) { return emplace(std::move(value)); }

  // ---- consumer side -------------------------------------------------------
  bool try_pop(T& out) {
    // Relaxed: we are the only thread that ever writes head_, so nobody can
    // surprise us with a different value. There is no race to prevent here, and
    // an acquire would cost a compiler barrier for nothing.
    const std::size_t head = head_.load(std::memory_order_relaxed);

    if (head == cached_tail_) {
      // Acquire: this is the load that must not be reordered with the read of
      // the payload below. It pairs with the producer's release store to tail_
      // and is what guarantees the bytes it wrote into the slot are visible to
      // us. Without it we can observe a bumped tail_ and stale slot contents -
      // the exact interleaving is in LEARN.md section 1.
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) {
        return false;  // genuinely empty, not just a stale cache
      }
    }

    T* cell = slot(head & kMask);
    out     = std::move(*cell);
    // Destroy immediately rather than at overwrite time. The slot is dead
    // storage from here on, so a queued object never outlives its pop - which
    // matters when T owns a socket, a lock, or a big allocation.
    cell->~T();

    // Release: everything above (the move-out and the destructor) must be
    // finished before the producer can see the slot as free. Otherwise the
    // producer's placement-new could start writing into a cell we are still
    // reading - a write/read race on the payload.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // ---- observers -----------------------------------------------------------
  // Approximate while both threads run: the value can be stale before the caller
  // reads it. Useful for instrumentation and assertions, not for control flow -
  // deciding "there is room" with size() and then pushing is a TOCTOU bug, which
  // is why try_push() re-checks internally.
  std::size_t size() const noexcept {
    // Load head_ *first*. Both counters only ever increase and head_ <= tail_ at
    // every instant, so reading the smaller one earlier keeps the difference
    // non-negative. In the other order tail_ could be read, then head_ could
    // race past it, and the unsigned subtraction would wrap to ~2^64.
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return tail - head;
  }

  bool empty() const noexcept { return size() == 0; }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  template <typename U>
  bool emplace(U&& value) {
    // Relaxed for the same reason as in try_pop(): the producer owns tail_.
    const std::size_t tail = tail_.load(std::memory_order_relaxed);

    // ---- the cached-index trick -------------------------------------------
    // cached_head_ is a plain, producer-private copy of the consumer's counter.
    // Reading the real head_ means touching a cache line the *consumer* writes,
    // which drags that line into our L1 in shared state and forces the consumer
    // to re-acquire it on its next pop - a coherence round trip on every single
    // push. Instead we trust the cached value, which is always <= the real head_
    // (the consumer only moves forward), so trusting it is conservative: the
    // worst it can do is make a non-full queue look full. Only in that case do
    // we pay for the real load. With a consumer that keeps up, the shared line
    // is touched roughly once per lap around the buffer instead of once per item.
    if (tail - cached_head_ == Capacity) {
      // Acquire: pairs with the consumer's release store to head_, so the
      // slot's destructor call in try_pop() happens-before our placement-new
      // into that same slot. Without it we could construct into a cell whose
      // old occupant is still being torn down.
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ == Capacity) {
        return false;  // really full
      }
    }

    // Note the invariant that makes `== Capacity` exact rather than a guess:
    // cached_head_ <= head_ <= tail_, and we never push once the difference
    // reaches Capacity, so tail_ - cached_head_ can never exceed Capacity.

    ::new (raw_slot(tail & kMask)) T(std::forward<U>(value));

    // Release: publishes both the new counter and the payload written above.
    // The store must be the last thing the consumer can observe, or it would
    // see a slot advertised as full before the object in it exists.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  void* raw_slot(std::size_t index) noexcept {
    return static_cast<void*>(storage_ + index * sizeof(T));
  }

  T* slot(std::size_t index) noexcept {
    // std::launder: the bytes at this address are only a T after the placement
    // new in emplace(). Casting the char array directly would be reading through
    // a pointer whose type never matched the object created there, which the
    // optimiser is allowed to exploit. launder tells it "re-derive what lives
    // here", and costs nothing at runtime.
    return std::launder(reinterpret_cast<T*>(storage_ + index * sizeof(T)));
  }

  // ---- layout --------------------------------------------------------------
  // Producer-owned line: tail_ is written by the producer and read by the
  // consumer; cached_head_ is producer-private. Grouping them is deliberate -
  // the producer dirties this line constantly, so nothing the consumer writes
  // may live here.
  alignas(index_alignment) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};

  // Consumer-owned line, mirror image of the above.
  alignas(index_alignment) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};

  // Why monotonic counters instead of indices already reduced mod Capacity:
  // with wrapped indices head == tail is ambiguous - it means both "empty" and
  // "full" - and the usual escape is to declare the buffer full one slot early,
  // wasting a slot and making the useful capacity Capacity-1 (a nasty surprise
  // when Capacity is 2). Counters that only ever increase make the occupancy
  // exactly tail_ - head_, so empty is 0, full is Capacity, and all Capacity
  // slots are usable. The counters themselves wrapping at 2^64 is harmless:
  // unsigned subtraction is modular, so the difference stays correct across the
  // wrap - and at a billion pushes a second it takes ~584 years to get there.
  //
  // The storage gets its own line too. Without this the first elements would
  // share a line with head_/cached_tail_, and the producer writing element 0
  // would invalidate the consumer's counter line - false sharing sneaking back
  // in through the payload, which would also contaminate the padded/unpadded
  // comparison.
  alignas(cache_line_size) alignas(T) unsigned char storage_[Capacity * sizeof(T)];
};

}  // namespace nano
