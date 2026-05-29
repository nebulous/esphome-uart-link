#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart_component.h"
#include "../uart_common/spsc_ring_buffer.h"
#include <vector>
#include <cstddef>
#include <cstdint>

namespace esphome::uart_bridge {

static const char *const TAG = "uart_bridge";

/// Per-member flow control.
///   FLOW_BOTH         — bridge reads from and writes to this member
///   FLOW_TO_BRIDGE    — member injects data into bridge, receives nothing back
///   FLOW_FROM_BRIDGE  — member receives data from bridge, can't inject
enum Flow : uint8_t {
  FLOW_BOTH = 0,
  FLOW_TO_BRIDGE = 1,
  FLOW_FROM_BRIDGE = 2,
};

struct Member {
  uart::UARTComponent *uart{nullptr};
  Flow flow{FLOW_BOTH};

  /// Whether the bridge reads from this member (fan-in source).
  bool reader() const { return flow == FLOW_BOTH || flow == FLOW_TO_BRIDGE; }

  /// Whether the bridge writes to this member (fan-out destination).
  bool writer() const { return flow == FLOW_BOTH || flow == FLOW_FROM_BRIDGE; }
};

/// Legacy A↔B direction for backward compatibility with uart_a/uart_b syntax.
enum Direction : uint8_t {
  DIRECTION_BIDIRECTIONAL = 0,
  DIRECTION_A_TO_B = 1,
  DIRECTION_B_TO_A = 2,
};

class UARTBridge : public uart::UARTComponent, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // --- New N-member API ---
  void add_member(uart::UARTComponent *uart, Flow flow) {
    members_.push_back({uart, flow});
  }
  void set_buffer_size(size_t size);

  // --- Legacy A↔B API (deprecated) ---
  void set_uart_a(uart::UARTComponent *a);
  void set_uart_b(uart::UARTComponent *b);
  void set_direction(Direction dir) {
    legacy_direction_ = dir;
    // Re-apply if members already populated
    apply_legacy_direction_();
  }

  // --- UARTComponent interface ---
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override;
  uart::UARTFlushResult flush() override;

 protected:
  void check_logger_conflict() override {}

  std::vector<Member> members_;
  std::vector<uint8_t> buffer_;

  /// Internal ring buffer — bytes from all reader members are merged here.
  /// Consumers read from the bridge via read_array() which drains this ring.
  uart_common::SPSCRingBuffer rx_ring_;

  // Peek support
  uint8_t peek_buffer_{0};
  bool has_peek_{false};

  // Legacy state
  bool legacy_mode_{false};
  Direction legacy_direction_{DIRECTION_BIDIRECTIONAL};

  void apply_legacy_direction_();

  // Stats
  uint32_t total_bytes_forwarded_{0};
};

}  // namespace esphome::uart_bridge
