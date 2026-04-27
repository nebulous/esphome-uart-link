#include "uart_bridge.h"
#include "esphome/core/log.h"

namespace esphome::uart_bridge {

void UARTBridge::setup() {
  ESP_LOGCONFIG(TAG, "UART Bridge:");
  if (uart_a_) {
    ESP_LOGCONFIG(TAG, "  Side A: configured");
  }
  if (uart_b_) {
    ESP_LOGCONFIG(TAG, "  Side B: configured");
  }
  ESP_LOGCONFIG(TAG, "  Buffer: %u bytes", (unsigned) buffer_.size());
  ESP_LOGCONFIG(TAG, "  Direction: %s",
                direction_ == DIRECTION_BIDIRECTIONAL ? "A ↔ B" :
                direction_ == DIRECTION_A_TO_B ? "A → B" : "B → A");
}

void UARTBridge::loop() {
  if (!uart_a_ || !uart_b_)
    return;

  if (direction_ == DIRECTION_BIDIRECTIONAL || direction_ == DIRECTION_A_TO_B) {
    forward_(uart_a_, uart_b_);
  }
  if (direction_ == DIRECTION_BIDIRECTIONAL || direction_ == DIRECTION_B_TO_A) {
    forward_(uart_b_, uart_a_);
  }
}

void UARTBridge::forward_(uart::UARTComponent *src, uart::UARTComponent *dst) {
  while (int n = src->available()) {
    n = std::min(n, (int) buffer_.size());
    if (!src->read_array(buffer_.data(), n))
      return;
    dst->write_array(buffer_.data(), n);
    dst->flush();

    // Track stats
    if (src == uart_a_) {
      bytes_a_to_b_ += n;
    } else {
      bytes_b_to_a_ += n;
    }
  }
}

void UARTBridge::dump_config() {
  size_t total = bytes_a_to_b_ + bytes_b_to_a_;
  if (total > 0) {
    ESP_LOGD(TAG, "Bridge stats: A→B %u bytes, B→A %u bytes (total %u)",
             (unsigned) bytes_a_to_b_, (unsigned) bytes_b_to_a_, (unsigned) total);
  }
}

}  // namespace esphome::uart_bridge
