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

class UARTTCPServerComponent;

/// Per-client state. The RX ring buffers bytes received from the client; the
/// TX ring buffers bytes queued for the client until the TCP send buffer has room.
struct ClientState {
  AsyncClient *client{nullptr};
  uart_common::SPSCRingBuffer ring;
  uart_common::SPSCRingBuffer tx_ring;
  volatile uint32_t last_rx_byte_time{0};
  bool connected{false};
  UARTTCPServerComponent *server{nullptr};
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
  void set_tx_buffer_size(size_t size) { tx_buffer_size_ = size; }
  void set_client_mode(ClientMode mode) { client_mode_ = mode; }
  void set_idle_timeout(uint32_t ms) { idle_timeout_ms_ = ms; }
  void set_name(const std::string &name) { name_ = name; }

  // UARTComponent interface
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override;
  uart::UARTFlushResult flush() override;
#if defined(USE_ESP8266) || defined(USE_ESP32)
  // load_settings(bool) became pure-virtual in UARTComponent on ESPHome
  // 2026.7.0. This is a virtual/network UART with no hardware settings to load.
  void load_settings(bool dump_config) override {}
#endif

 protected:
  void check_logger_conflict() override {}
  ClientState *accept_client_(AsyncClient *client);
  void merge_rx_();
  void drain_tx_();
  void enqueue_tx_(ClientState *cs, const uint8_t *data, size_t len);

  uint16_t port_{0};
  size_t max_clients_{2};
  size_t rx_buffer_size_{4096};
#if defined(USE_ESP32)
  // ESP32 has heap to spare; buffer bursts that exceed the TCP send window.
  size_t tx_buffer_size_{16384};
#else
  // Constrained platforms (ESP8266, etc.): 0 writes straight through and
  // drops on a short write, trading integrity for RAM under load.
  size_t tx_buffer_size_{0};
#endif
  ClientMode client_mode_{CLIENT_MODE_FANOUT};
  uint32_t idle_timeout_ms_{0};

  AsyncServer *tcp_server_{nullptr};
  std::vector<ClientState *> clients_;

  // Merged RX — bytes from all client rings are drained here on each loop()
  uart_common::SPSCRingBuffer merged_ring_;

  uint8_t peek_buffer_{0};
  bool has_peek_{false};

  std::string name_;

  static std::string remote_addr_(AsyncClient *client);

  volatile uint32_t total_clients_accepted_{0};
  volatile uint32_t total_clients_rejected_{0};
  volatile uint32_t total_tx_dropped_{0};
};

}  // namespace esphome::uart_tcp_server
