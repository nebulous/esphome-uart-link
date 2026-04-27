#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart_component.h"
#include <vector>
#include <algorithm>

namespace esphome::uart_bridge {

static const char *const TAG = "uart_bridge";

enum Direction : uint8_t {
  DIRECTION_BIDIRECTIONAL = 0,  // A ↔ B (default)
  DIRECTION_A_TO_B = 1,         // A → B only
  DIRECTION_B_TO_A = 2,         // B → A only
};

class UARTBridge : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_uart_a(uart::UARTComponent *a) { uart_a_ = a; }
  void set_uart_b(uart::UARTComponent *b) { uart_b_ = b; }
  void set_buffer_size(size_t size) { buffer_.resize(size, 0); }
  void set_direction(Direction dir) { direction_ = dir; }

 protected:
  void forward_(uart::UARTComponent *src, uart::UARTComponent *dst);

  uart::UARTComponent *uart_a_{nullptr};
  uart::UARTComponent *uart_b_{nullptr};
  std::vector<uint8_t> buffer_{std::vector<uint8_t>(512)};
  Direction direction_{DIRECTION_BIDIRECTIONAL};

  // Stats
  uint32_t bytes_a_to_b_{0};
  uint32_t bytes_b_to_a_{0};
};

}  // namespace esphome::uart_bridge
