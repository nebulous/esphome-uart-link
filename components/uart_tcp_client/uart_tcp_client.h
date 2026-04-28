#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/async_tcp/async_tcp.h"
#include "../uart_common/spsc_ring_buffer.h"
#include <string>

namespace esphome::uart_tcp_client {

static const char *const TAG = "uart_tcp_client";

class UARTTCPClientComponent : public uart::UARTComponent, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Configuration setters
  void set_host(const std::string &host) { host_ = host; }
  void set_port(uint16_t port) { port_ = port; }
  void set_rx_buffer_size(size_t size) { rx_buffer_size_ = size; }
  void set_reconnect_interval(uint32_t ms) { reconnect_interval_ms_ = ms; }
  void set_stall_timeout(uint32_t ms) { stall_timeout_ms_ = ms; }

  // UARTComponent interface
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override;
  uart::UARTFlushResult flush() override;

 protected:
  void check_logger_conflict() override {}
  void connect_();
  void disconnect_();

  std::string host_;
  uint16_t port_{0};
  size_t rx_buffer_size_{4096};
  uint32_t reconnect_interval_ms_{5000};

  AsyncClient tcp_client_;
  uart_common::SPSCRingBuffer ring_;

  // Peek support
  uint8_t peek_buffer_{0};
  bool has_peek_{false};

  // State
  bool connected_{false};
  uint32_t last_connect_attempt_{0};

  // Stall detection
  uint32_t stall_timeout_ms_{15000};
  volatile uint32_t last_rx_byte_time_{0};
};

}  // namespace esphome::uart_tcp_client
