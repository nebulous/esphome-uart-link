#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/async_tcp/async_tcp.h"
#include "../uart_common/spsc_ring_buffer.h"
#include <vector>
#include <cstdio>

namespace esphome::uart_tcp_server {

static const char *const TAG = "uart_tcp_server";

enum ClientMode : uint8_t {
  CLIENT_MODE_FANOUT = 0,
  CLIENT_MODE_EXCLUSIVE = 1,
};

/// Per-client state. Each accepted TCP client gets its own ring buffer.
struct ClientState {
  AsyncClient *client{nullptr};
  uart_common::SPSCRingBuffer ring;
  volatile uint32_t last_rx_byte_time{0};
  bool connected{false};
};

class UARTTCPServerComponent : public uart::UARTComponent, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { port_ = port; }
  void set_max_clients(size_t n) { max_clients_ = n; }
  void set_rx_buffer_size(size_t size) { rx_buffer_size_ = size; }
  void set_client_mode(ClientMode mode) { client_mode_ = mode; }
  void set_idle_timeout(uint32_t ms) { idle_timeout_ms_ = ms; }

  // UARTComponent interface
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override;
  uart::UARTFlushResult flush() override;

 protected:
  void check_logger_conflict() override {}
  ClientState *accept_client_(AsyncClient *client);
  void merge_rx_();

  uint16_t port_{0};
  size_t max_clients_{2};
  size_t rx_buffer_size_{4096};
  ClientMode client_mode_{CLIENT_MODE_FANOUT};
  uint32_t idle_timeout_ms_{0};

  AsyncServer *tcp_server_{nullptr};
  std::vector<ClientState *> clients_;

  // Merged RX — bytes from all client rings are drained here on each loop()
  uart_common::SPSCRingBuffer merged_ring_;

  uint8_t peek_buffer_{0};
  bool has_peek_{false};

  static std::string remote_addr_(AsyncClient *client);

  volatile uint32_t total_clients_accepted_{0};
  volatile uint32_t total_clients_rejected_{0};
};

}  // namespace esphome::uart_tcp_server
