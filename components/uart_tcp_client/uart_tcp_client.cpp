#include "uart_tcp_client.h"
#include "esphome/core/log.h"

namespace esphome::uart_tcp_client {

void UARTTCPClientComponent::setup() {
  ring_.init(rx_buffer_size_);

  tcp_client_.onConnect(
      [](void *arg, AsyncClient *client) {
        auto *self = static_cast<UARTTCPClientComponent *>(arg);
        self->connected_ = true;
        self->last_rx_byte_time_ = millis();
        ESP_LOGI(TAG, "Connected to %s:%u", self->host_.c_str(), self->port_);
      },
      this);

  tcp_client_.onDisconnect(
      [](void *arg, AsyncClient *client) {
        auto *self = static_cast<UARTTCPClientComponent *>(arg);
        self->connected_ = false;
        ESP_LOGW(TAG, "Disconnected from %s:%u", self->host_.c_str(), self->port_);
      },
      this);

  tcp_client_.onData(
      [](void *arg, AsyncClient *client, void *data, size_t len) {
        auto *self = static_cast<UARTTCPClientComponent *>(arg);
        self->ring_.write(static_cast<uint8_t *>(data), len);
        self->last_rx_byte_time_ = millis();
      },
      this);

  tcp_client_.onError(
      [](void *arg, AsyncClient *client, int8_t error) {
        auto *self = static_cast<UARTTCPClientComponent *>(arg);
        ESP_LOGE(TAG, "TCP error: %d", error);
        self->connected_ = false;
      },
      this);

  connect_();
}

void UARTTCPClientComponent::connect_() {
  ESP_LOGI(TAG, "Connecting to %s:%u ...", host_.c_str(), port_);
  tcp_client_.connect(host_.c_str(), port_);
  last_connect_attempt_ = millis();
}

void UARTTCPClientComponent::disconnect_() {
  tcp_client_.close();
  connected_ = false;
}

void UARTTCPClientComponent::loop() {
#if !defined(USE_ESP32) && !defined(USE_ESP8266) && !defined(USE_RP2040) && !defined(USE_LIBRETINY)
  tcp_client_.loop();
#endif

  // Stall detection
  if (connected_ && last_rx_byte_time_ > 0 && stall_timeout_ms_ > 0) {
    uint32_t since_last_rx = millis() - last_rx_byte_time_;
    if (since_last_rx > stall_timeout_ms_) {
      ESP_LOGW(TAG, "No data for %ums (stall), forcing reconnect", since_last_rx);
      disconnect_();
      ring_.clear();
      has_peek_ = false;
      last_rx_byte_time_ = 0;
    }
  }

  // Auto-reconnect
  if (!connected_ && (millis() - last_connect_attempt_ > reconnect_interval_ms_)) {
    connect_();
  }
}

void UARTTCPClientComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "UART TCP Client:");
  ESP_LOGCONFIG(TAG, "  Host: %s:%u", host_.c_str(), port_);
  ESP_LOGCONFIG(TAG, "  RX Buffer: %u bytes (ring capacity: %u)", (unsigned) rx_buffer_size_,
                (unsigned) ring_.capacity());
  ESP_LOGCONFIG(TAG, "  Connected: %s", connected_ ? "YES" : "NO");
}

// ---- UARTComponent overrides ----

void UARTTCPClientComponent::write_array(const uint8_t *data, size_t len) {
  if (!connected_) {
    ESP_LOGW(TAG, "write_array: not connected, dropping %u bytes", (unsigned) len);
    return;
  }
  size_t written = tcp_client_.write((const char *) data, len);
  if (written < len) {
    ESP_LOGW(TAG, "write_array: only %u/%u bytes written", (unsigned) written, (unsigned) len);
  }
}

size_t UARTTCPClientComponent::available() {
  return ring_.available() + (has_peek_ ? 1 : 0);
}

bool UARTTCPClientComponent::read_array(uint8_t *data, size_t len) {
  size_t offset = 0;
  if (has_peek_) {
    data[offset++] = peek_buffer_;
    has_peek_ = false;
  }
  size_t got = ring_.read(data + offset, len - offset);
  return (offset + got) == len;
}

bool UARTTCPClientComponent::peek_byte(uint8_t *data) {
  if (has_peek_) {
    *data = peek_buffer_;
    return true;
  }
  if (ring_.read(&peek_buffer_, 1) != 1) {
    return false;
  }
  has_peek_ = true;
  *data = peek_buffer_;
  return true;
}

uart::UARTFlushResult UARTTCPClientComponent::flush() {
  // TCP is full-duplex. No hardware FIFO to drain.
  return uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS;
}

}  // namespace esphome::uart_tcp_client
