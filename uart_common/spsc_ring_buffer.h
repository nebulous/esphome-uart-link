#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>

namespace esphome::uart_common {

/// Single-producer / single-consumer ring buffer.
/// Producer: write() — typically called from TCP thread callback.
/// Consumer: read() / available() / clear() — typically called from main loop.
/// Power-of-2 capacity for mask-based modulo (no division).
/// On overflow, oldest bytes are dropped.
struct SPSCRingBuffer {
  std::vector<uint8_t> buf;
  volatile size_t head{0};  // write position (producer)
  volatile size_t tail{0};  // read position (consumer)
  size_t mask{0};           // capacity - 1

  /// Allocate and zero-fill. Rounds up to next power of 2.
  void init(size_t min_size) {
    size_t cap = 1;
    while (cap < min_size)
      cap <<= 1;
    buf.resize(cap, 0);
    mask = cap - 1;
    head = 0;
    tail = 0;
  }

  /// Reset read/write positions without reallocating.
  void clear() { tail = head; }

  /// Write bytes into ring buffer (producer side).
  /// Drops oldest bytes on overflow.
  void write(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
      size_t next_head = (head + 1) & mask;
      if (next_head != tail) {
        buf[head] = data[i];
        head = next_head;
      } else {
        // Buffer full — drop oldest byte
        tail = (tail + 1) & mask;
        buf[head] = data[i];
        head = next_head;
      }
    }
  }

  /// Read up to len bytes. Returns count actually read.
  size_t read(uint8_t *data, size_t len) {
    size_t count = 0;
    while (count < len && tail != head) {
      data[count++] = buf[tail];
      tail = (tail + 1) & mask;
    }
    return count;
  }

  /// Number of bytes available to read.
  size_t available() const { return (head - tail) & mask; }

  /// Allocated capacity (bytes).
  size_t capacity() const { return buf.size(); }
};

}  // namespace esphome::uart_common
