// ---------------------------------------------------------------------------
// test_spsc.cpp - assert-based tests for spsc_ring. No framework on purpose:
// the queue has no external dependencies, so neither do its tests.
//
// Built twice (padded and unpadded). Padding is a layout change and must not
// change a single observable behaviour - if one variant passes and the other
// does not, the A/B knob is doing more than it claims and every number the
// benchmark produces is suspect.
// ---------------------------------------------------------------------------

// assert() compiles to nothing when NDEBUG is defined, and NDEBUG *is* defined
// in the optimised builds this project uses. Without this the whole file would
// silently degenerate into a program that allocates a queue and exits 0.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "spsc_ring.hpp"

namespace {

// ---------------------------------------------------------------------------
// A type that counts every construction and destruction, so the tests can prove
// the ring destroys exactly the objects it owns - no more (double destruction),
// no fewer (leak), and none at all for slots that were never filled.
// ---------------------------------------------------------------------------
struct tracker {
  static int alive;        // constructions minus destructions
  static int constructed;  // includes copies and moves - each makes a new object
  static int destroyed;

  int value;

  explicit tracker(int v = 0) : value(v) {
    ++constructed;
    ++alive;
  }
  tracker(const tracker& other) : value(other.value) {
    ++constructed;
    ++alive;
  }
  tracker(tracker&& other) noexcept : value(other.value) {
    other.value = -1;  // poison, so a stale read shows up as an obvious value
    ++constructed;
    ++alive;
  }
  // Assignment does not create or destroy an object, so it touches no counter.
  tracker& operator=(const tracker& other) {
    value = other.value;
    return *this;
  }
  tracker& operator=(tracker&& other) noexcept {
    value       = other.value;
    other.value = -1;
    return *this;
  }
  ~tracker() {
    ++destroyed;
    --alive;
  }

  static void reset() { alive = constructed = destroyed = 0; }
};

int tracker::alive       = 0;
int tracker::constructed = 0;
int tracker::destroyed   = 0;

// A type that cannot be copied at all. If spsc_ring's try_push(const T&)
// overload were instantiated eagerly, this would not compile.
struct move_only {
  std::unique_ptr<int> payload;

  explicit move_only(int v = 0) : payload(std::make_unique<int>(v)) {}
  move_only(move_only&&) noexcept            = default;
  move_only& operator=(move_only&&) noexcept = default;
  move_only(const move_only&)                = delete;
  move_only& operator=(const move_only&)     = delete;
};

// ---------------------------------------------------------------------------
void test_empty_and_full() {
  nano::spsc_ring<int, 4> ring;

  assert(ring.empty());
  assert(ring.size() == 0);
  assert(ring.capacity() == 4);
  int out = -1;
  assert(!ring.try_pop(out));  // popping an empty ring must fail, not block
  assert(out == -1);           // ... and must not touch the output

  for (int i = 0; i < 4; ++i) {
    assert(ring.try_push(i));
    assert(ring.size() == static_cast<std::size_t>(i) + 1);
  }

  // All four slots are usable. With the "keep one slot empty" trick this would
  // have failed on the fourth push - see LEARN.md section 4.
  assert(!ring.empty());
  assert(ring.size() == 4);
  assert(!ring.try_push(99));
  assert(ring.size() == 4);

  for (int i = 0; i < 4; ++i) {
    assert(ring.try_pop(out));
    assert(out == i);  // FIFO, not LIFO
  }
  assert(ring.empty());
  assert(!ring.try_pop(out));

  std::printf("  [ok] empty/full behaviour\n");
}

// ---------------------------------------------------------------------------
void test_wraparound() {
  // Capacity 4, but we push 4 * 10 + 3 items through it. Every slot gets reused
  // ten times, and the internal counters run far past Capacity - which is the
  // point: they are never reduced mod Capacity, only masked at index time.
  nano::spsc_ring<int, 4> ring;

  constexpr int kLaps = 10;
  int           next  = 0;
  for (int lap = 0; lap < kLaps; ++lap) {
    // Fill completely, then drain completely, so the read and write indices
    // cross a multiple of Capacity in every possible relative position.
    for (int i = 0; i < 4; ++i) {
      assert(ring.try_push(next + i));
    }
    assert(!ring.try_push(-1));

    for (int i = 0; i < 4; ++i) {
      int out = -1;
      assert(ring.try_pop(out));
      assert(out == next + i);
    }
    assert(ring.empty());
    next += 4;
  }

  // Now a partial lap, leaving the head and tail at a non-zero offset.
  for (int i = 0; i < 3; ++i) {
    assert(ring.try_push(next + i));
  }
  assert(ring.size() == 3);
  for (int i = 0; i < 3; ++i) {
    int out = -1;
    assert(ring.try_pop(out));
    assert(out == next + i);
  }

  std::printf("  [ok] wraparound past capacity (%d laps)\n", kLaps);
}

// ---------------------------------------------------------------------------
void test_move_only_type() {
  nano::spsc_ring<move_only, 8> ring;

  for (int i = 0; i < 8; ++i) {
    assert(ring.try_push(move_only(i)));
  }
  assert(!ring.try_push(move_only(999)));

  for (int i = 0; i < 8; ++i) {
    move_only out;
    assert(ring.try_pop(out));
    assert(out.payload != nullptr);
    assert(*out.payload == i);
  }
  assert(ring.empty());

  std::printf("  [ok] move-only element type\n");
}

// ---------------------------------------------------------------------------
void test_destructor_counts() {
  tracker::reset();

  {
    nano::spsc_ring<tracker, 4> ring;
    // Nothing has been constructed yet. This is the whole reason the storage is
    // a raw byte array instead of std::array<T, Capacity>: an array member would
    // have default-constructed all four slots right here.
    assert(tracker::constructed == 0);
    assert(tracker::alive == 0);

    for (int i = 0; i < 4; ++i) {
      // Each push builds a temporary (+1 alive), move-constructs it into the
      // slot (+1) and destroys the temporary (-1). Net: one live object per push.
      assert(ring.try_push(tracker(i)));
    }
    assert(tracker::alive == 4);

    {
      tracker out;  // +1 alive
      assert(tracker::alive == 5);
      assert(ring.try_pop(out));
      assert(out.value == 0);
      // The popped slot was destroyed inside try_pop, so we are back to three
      // objects in the ring plus `out`.
      assert(tracker::alive == 4);
    }
    assert(tracker::alive == 3);  // `out` went out of scope

    // Three objects are still queued. The ring's destructor has to find exactly
    // those and destroy exactly those - not all four slots, and not zero.
  }

  assert(tracker::alive == 0);
  assert(tracker::destroyed == tracker::constructed);

  // Second scope: a ring that wrapped, so the live range [head, tail) straddles
  // the end of the storage array. A destructor that looped 0..Capacity-1 instead
  // of head..tail would destroy the wrong slots and this would blow up.
  tracker::reset();
  {
    nano::spsc_ring<tracker, 4> ring;
    for (int i = 0; i < 4; ++i) {
      assert(ring.try_push(tracker(i)));
    }
    for (int i = 0; i < 3; ++i) {
      tracker out;
      assert(ring.try_pop(out));
    }
    // head == 3, tail == 4; pushing two more puts tail at 6, so the live slots
    // are indices 3, 0, 1 - wrapped.
    assert(ring.try_push(tracker(100)));
    assert(ring.try_push(tracker(101)));
    assert(tracker::alive == 3);
  }
  assert(tracker::alive == 0);
  assert(tracker::destroyed == tracker::constructed);

  std::printf("  [ok] destructor counts (no leaks, no double frees)\n");
}

// ---------------------------------------------------------------------------
void test_concurrent_fifo() {
  // The only test that actually exercises the memory ordering. Everything above
  // runs on one thread and would pass even with every atomic replaced by a plain
  // int. Run this one under ThreadSanitizer.
  constexpr std::size_t   kCapacity = 1024;
  constexpr std::uint64_t kItems    = 1'000'000;

  auto ring = std::make_unique<nano::spsc_ring<std::uint64_t, kCapacity>>();

  std::thread producer([&ring] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      // Spin rather than sleep: this is a test of ordering, and a sleeping
      // producer would hide ordering bugs by never racing the consumer.
      while (!ring->try_push(i)) {
      }
    }
  });

  std::uint64_t received = 0;
  std::uint64_t checksum = 0;

  std::thread consumer([&ring, &received, &checksum] {
    std::uint64_t expected = 0;
    std::uint64_t value    = 0;
    while (expected < kItems) {
      if (ring->try_pop(value)) {
        // Strict equality, not just "increasing": this catches duplication,
        // loss, and reordering in one assert. A torn or stale read of the slot
        // (the bug a relaxed store would cause) shows up here as a wrong value.
        assert(value == expected);
        checksum += value;
        ++expected;
      }
    }
    received = expected;
  });

  producer.join();
  consumer.join();

  assert(received == kItems);
  assert(ring->empty());
  assert(ring->size() == 0);

  // Independent check on the payload: sum of 0..kItems-1.
  const std::uint64_t want = (kItems - 1) * kItems / 2;
  assert(checksum == want);

  std::printf("  [ok] concurrent FIFO order over %llu items\n",
              static_cast<unsigned long long>(kItems));
}

}  // namespace

int main() {
  std::printf("test_spsc  [%s build]\n",
              nano::padding_enabled ? "padded" : "unpadded");

  test_empty_and_full();
  test_wraparound();
  test_move_only_type();
  test_destructor_counts();
  test_concurrent_fifo();

  std::printf("all tests passed\n");
  return 0;
}
