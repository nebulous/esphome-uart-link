#include "uart_tcp_client.h"
#include "esphome/core/log.h"

namespace esphome::uart_tcp_client {

void UARTTCPClientComponent::setup() {
  ring_.init(rx_buffer_size_);

  tcp_client_.onConnect(
      [](void *arg, AsyncClient *client) {
        auto *self = static_cast<UARTTCPClientComponent *>(arg);
        self->connected_ = true;
        self->connecting_ = false;
        self->last_rx_byte_time_ = millis();
        ESP_LOGI(TAG, "'%s' connected to %s:%u",
                 self->name_.empty() ? "(no id)" : self->name_.c_str(),
                 self->host_.c_str(), self->port_);
      },
      this);

  tcp_client_.onDisconnect(
      [](void *arg, AsyncClient *client) {
        auto *self = static_cast<UARTTCPClientComponent *>(arg);
        self->connected_ = false;
        self->connecting_ = false;
        ESP_LOGW(TAG, "'%s' disconnected from %s:%u",
                 self->name_.empty() ? "(no id)" : self->name_.c_str(),
                 self->host_.c_str(), self->port_);
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
        ESP_LOGE(TAG, "'%s' TCP error: %d",
                 self->name_.empty() ? "(no id)" : self->name_.c_str(), error);
        self->connected_ = false;
        self->connecting_ = false;
      },
      this);

  // Don't connect from setup() — defer to loop() so the system is fully
  // initialized. Use a sentinel value of 0 and check for it in loop().
  last_connect_attempt_ = 0;
}

void UARTTCPClientComponent::connect_() {
  if (connecting_) {
    // A previous attempt is still in-flight. close() will trigger the
    // onDisconnect callback which resets connecting_ and the next loop()
    // will retry.
    tcp_client_.close();
    return;
  }
  ESP_LOGI(TAG, "'%s' connecting to %s:%u ...",
           name_.empty() ? "(no id)" : name_.c_str(), host_.c_str(), port_);
  connecting_ = true;
  last_connect_attempt_ = millis();

  // If the host is a dotted-decimal IP, resolve it synchronously and
  // call connect(ip, port) directly to avoid a DNS callback that may
  // never fire (observed with AsyncTCP's dns_found on ESP32-S3).
  ip_addr_t addr;
  if (ipaddr_aton(host_.c_str(), &addr)) {
    tcp_client_.connect(addr, port_);
  } else {
    tcp_client_.connect(host_.c_str(), port_);
  }
}

void UARTTCPClientComponent::disconnect_() {
  tcp_client_.close();
  connected_ = false;
  connecting_ = false;
}

void UARTTCPClientComponent::loop() {
#if !defined(USE_ESP32) && !defined(USE_ESP8266) && !defined(USE_ESP2040) && !defined(USE_LIBRETINY)
  tcp_client_.loop();
#endif

  // Stall detection
  if (connected_ && last_rx_byte_time_ > 0 && stall_timeout_ms_ > 0) {
    uint32_t since_last_rx = millis() - last_rx_byte_time_;
    if (since_last_rx > stall_timeout_ms_) {
      ESP_LOGW(TAG, "'%s' no data for %ums (stall), forcing reconnect",
               name_.empty() ? "(no id)" : name_.c_str(), (unsigned) since_last_rx);
      disconnect_();
      ring_.clear();
      has_peek_ = false;
      last_rx_byte_time_ = 0;
    }
  }

  // Auto-reconnect
  if (!connected_ && !connecting_ &&
      (last_connect_attempt_ == 0 || millis() - last_connect_attempt_ > reconnect_interval_ms_)) {
    connect_();
  }

  // Safety net: if connecting_ has been stuck true for too long without
  // any callback, force-close and retry. Handles cases where DNS resolution
  // or the async TCP task silently drops the connect attempt.
  if (connecting_ && last_connect_attempt_ > 0 &&
      millis() - last_connect_attempt_ > reconnect_interval_ms_ * 2) {
    ESP_LOGW(TAG, "'%s' connect attempt stuck for %lums, forcing close",
             name_.empty() ? "(no id)" : name_.c_str(),
             (unsigned long)(millis() - last_connect_attempt_));
    tcp_client_.close();
    connecting_ = false;
  }
}

void UARTTCPClientComponent::dump_config() {
  const char *id = name_.empty() ? "(no id)" : name_.c_str();
  ESP_LOGCONFIG(TAG, "UART TCP Client '%s':", id);
  ESP_LOGCONFIG(TAG, "  Host: %s:%u", host_.c_str(), port_);
  ESP_LOGCONFIG(TAG, "  RX Buffer: %u bytes (ring capacity: %u)", (unsigned) rx_buffer_size_,
                (unsigned) ring_.capacity());
  ESP_LOGCONFIG(TAG, "  Connected: %s", connected_ ? "YES" : "NO");
}

// ---- UARTComponent overrides ----

void UARTTCPClientComponent::write_array(const uint8_t *data, size_t len) {
  if (!connected_) {
    ESP_LOGD(TAG, "'%s' write_array: not connected, dropping %u bytes",
             name_.empty() ? "(no id)" : name_.c_str(), (unsigned) len);
    return;
  }
  size_t written = tcp_client_.write((const char *) data, len);
  if (written < len) {
    ESP_LOGW(TAG, "'%s' write_array: only %u/%u bytes written",
             name_.empty() ? "(no id)" : name_.c_str(), (unsigned) written, (unsigned) len);
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
