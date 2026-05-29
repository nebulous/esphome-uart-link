#include "uart_bridge.h"
#include "esphome/core/log.h"

namespace esphome::uart_bridge {

void UARTBridge::set_buffer_size(size_t size) {
  buffer_.resize(size, 0);
}

void UARTBridge::set_uart_a(uart::UARTComponent *a) {
  legacy_mode_ = true;
  if (members_.empty())
    members_.resize(2);
  members_[0].uart = a;
  members_[0].flow = FLOW_BOTH;
}

void UARTBridge::set_uart_b(uart::UARTComponent *b) {
  legacy_mode_ = true;
  if (members_.size() < 2)
    members_.resize(2);
  members_[1].uart = b;
  members_[1].flow = FLOW_BOTH;
  apply_legacy_direction_();
}

void UARTBridge::apply_legacy_direction_() {
  if (members_.size() < 2)
    return;
  // Apply legacy direction to member flow
  if (legacy_direction_ == DIRECTION_A_TO_B) {
    members_[0].flow = FLOW_TO_BRIDGE;
    members_[1].flow = FLOW_FROM_BRIDGE;
  } else if (legacy_direction_ == DIRECTION_B_TO_A) {
    members_[0].flow = FLOW_FROM_BRIDGE;
    members_[1].flow = FLOW_TO_BRIDGE;
  } else {
    members_[0].flow = FLOW_BOTH;
    members_[1].flow = FLOW_BOTH;
  }
}

void UARTBridge::setup() {
  // Initialize rx ring (use buffer_size as capacity hint)
  rx_ring_.init(buffer_.empty() ? 512 : buffer_.size());

  ESP_LOGCONFIG(TAG, "UART Bridge:");
  for (size_t i = 0; i < members_.size(); i++) {
    const char *flow_str =
        members_[i].flow == FLOW_BOTH ? "both" :
        members_[i].flow == FLOW_TO_BRIDGE ? "to_bridge" :
        "from_bridge";
    ESP_LOGCONFIG(TAG, "  UART %u: %s (flow: %s)", (unsigned) i,
                  members_[i].uart ? "configured" : "NULL", flow_str);
  }
  ESP_LOGCONFIG(TAG, "  Buffer: %u bytes", (unsigned) buffer_.size());
  ESP_LOGCONFIG(TAG, "  RX ring: %u bytes", (unsigned) rx_ring_.capacity());
}

void UARTBridge::loop() {
  // Fan-in: read from all reader members → rx ring + writer members
  for (auto &src : members_) {
    if (!src.reader() || !src.uart)
      continue;

    while (src.uart->available()) {
      size_t n = std::min((size_t) src.uart->available(), buffer_.size());
      if (!src.uart->read_array(buffer_.data(), n))
        break;

      // Write to internal ring for consumer reads
      rx_ring_.write(buffer_.data(), n);

      // Fan out to all writer members except the source
      for (auto &dst : members_) {
        if (&dst == &src || !dst.writer() || !dst.uart)
          continue;
        dst.uart->write_array(buffer_.data(), n);
        dst.uart->flush();
      }

      total_bytes_forwarded_ += n;
    }
  }
}

void UARTBridge::dump_config() {
  if (total_bytes_forwarded_ > 0) {
    ESP_LOGD(TAG, "Bridge stats: %u bytes forwarded", (unsigned) total_bytes_forwarded_);
  }
}

// ---- UARTComponent overrides ----

void UARTBridge::write_array(const uint8_t *data, size_t len) {
  // Consumer writes to bridge → forward to all writer members
  size_t sent_count = 0;
  for (auto &member : members_) {
    if (member.writer() && member.uart) {
      member.uart->write_array(data, len);
      member.uart->flush();
      sent_count++;
    }
  }
  if (sent_count == 0 && len > 0) {
    ESP_LOGD(TAG, "write_array: no writer members, dropping %u bytes", (unsigned) len);
  }
}

size_t UARTBridge::available() {
  return rx_ring_.available() + (has_peek_ ? 1 : 0);
}

bool UARTBridge::read_array(uint8_t *data, size_t len) {
  size_t offset = 0;
  if (has_peek_) {
    data[offset++] = peek_buffer_;
    has_peek_ = false;
  }
  size_t got = rx_ring_.read(data + offset, len - offset);
  return (offset + got) == len;
}

bool UARTBridge::peek_byte(uint8_t *data) {
  if (has_peek_) {
    *data = peek_buffer_;
    return true;
  }
  if (rx_ring_.read(&peek_buffer_, 1) != 1) {
    return false;
  }
  has_peek_ = true;
  *data = peek_buffer_;
  return true;
}

uart::UARTFlushResult UARTBridge::flush() {
  return uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS;
}

}  // namespace esphome::uart_bridge
