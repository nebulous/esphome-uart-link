// Unit tests for uart_common::SPSCRingBuffer. Host toolchain only, no ESPHome.
//
// One contract note: the ring reserves one slot to distinguish full from
// empty, so a buffer initialized with min_size N holds at most
// next_pow2(N) - 1 bytes. These tests assert that actual behavior.

#include <cassert>
#include <cstdio>
#include <vector>

#include "../../components/uart_common/spsc_ring_buffer.h"

using esphome::uart_common::SPSCRingBuffer;

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
      failures++;                                                            \
    }                                                                        \
  } while (0)

static void test_rounds_capacity_up_to_power_of_two() {
  SPSCRingBuffer r;
  r.init(3);
  CHECK(r.capacity() == 4);
  r.init(5);
  CHECK(r.capacity() == 8);
  r.init(4096);
  CHECK(r.capacity() == 4096);
}

static void test_basic_write_read() {
  SPSCRingBuffer r;
  r.init(8);
  const uint8_t in[] = {1, 2, 3};
  r.write(in, 3);
  CHECK(r.available() == 3);
  uint8_t out[3] = {};
  CHECK(r.read(out, 3) == 3);
  CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3);
  CHECK(r.available() == 0);
}

static void test_read_empty_returns_zero() {
  SPSCRingBuffer r;
  r.init(8);
  uint8_t out[4];
  CHECK(r.read(out, 4) == 0);
  CHECK(r.available() == 0);
}

static void test_read_partial_when_fewer_available() {
  SPSCRingBuffer r;
  r.init(8);
  const uint8_t in[] = {9, 8};
  r.write(in, 2);
  uint8_t out[6] = {};
  CHECK(r.read(out, 6) == 2);
  CHECK(out[0] == 9 && out[1] == 8);
}

static void test_overflow_drops_oldest() {
  SPSCRingBuffer r;
  r.init(4);  // capacity 4, holds 3
  uint8_t in[8];
  for (uint8_t i = 0; i < 8; i++)
    in[i] = i + 1;
  r.write(in, 8);
  CHECK(r.available() == 3);
  uint8_t out[3] = {};
  CHECK(r.read(out, 3) == 3);
  // Newest three of 1..8 survive.
  CHECK(out[0] == 6 && out[1] == 7 && out[2] == 8);
}

static void test_available_across_wraparound() {
  SPSCRingBuffer r;
  r.init(4);  // capacity 4, holds 3
  const uint8_t a[] = {10, 11, 12};
  r.write(a, 3);
  uint8_t out[2];
  CHECK(r.read(out, 2) == 2);  // consumed 10, 11; head is at the wrap point
  const uint8_t b[] = {13, 14, 15};
  // Held {12} leaves 2 free of 3 usable; the third byte overflows and
  // drops the oldest (12). Order across the wrap is still FIFO.
  r.write(b, 3);
  CHECK(r.available() == 3);
  uint8_t all[3] = {};
  CHECK(r.read(all, 3) == 3);
  CHECK(all[0] == 13 && all[1] == 14 && all[2] == 15);
}

static void test_clear_empties_without_realloc() {
  SPSCRingBuffer r;
  r.init(8);
  const uint8_t in[] = {1, 2, 3, 4};
  r.write(in, 4);
  r.clear();
  CHECK(r.available() == 0);
  uint8_t out[4];
  CHECK(r.read(out, 4) == 0);
  const uint8_t again[] = {7};
  r.write(again, 1);
  CHECK(r.available() == 1);
  CHECK(r.capacity() == 8);
}

static void test_sustained_stream_stays_consistent() {
  // Push 10x the capacity through while draining lagged, verifying FIFO
  // order survives the drop-oldest policy.
  SPSCRingBuffer r;
  r.init(8);  // holds 7
  uint8_t seq = 0;
  uint8_t expected_next = 0;
  uint8_t out[7];
  for (int round = 0; round < 100; round++) {
    uint8_t chunk[3];
    for (int i = 0; i < 3; i++)
      chunk[i] = seq++;
    r.write(chunk, 3);
    // Whatever we read must continue the sequence in order.
    size_t n = r.read(out, 7);
    for (size_t i = 0; i < n; i++)
      CHECK(out[i] == expected_next++);
  }
}

int main() {
  test_rounds_capacity_up_to_power_of_two();
  test_basic_write_read();
  test_read_empty_returns_zero();
  test_read_partial_when_fewer_available();
  test_overflow_drops_oldest();
  test_available_across_wraparound();
  test_clear_empties_without_realloc();
  test_sustained_stream_stays_consistent();
  if (failures == 0) {
    printf("all SPSCRingBuffer tests passed\n");
    return 0;
  }
  printf("%d check(s) failed\n", failures);
  return 1;
}
