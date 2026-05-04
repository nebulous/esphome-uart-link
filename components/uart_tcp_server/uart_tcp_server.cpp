#include "uart_tcp_server.h"
#include "esphome/core/log.h"

namespace esphome::uart_tcp_server {

std::string UARTTCPServerComponent::remote_addr_(AsyncClient *client) {
  char buf[24];
#if defined(USE_ESP8266)
  auto ip = client->remoteIP();
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], client->remotePort());
#else
  uint32_t addr = client->getRemoteAddress();
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF,
           (addr >> 24) & 0xFF, client->remotePort());
#endif
  return buf;
}

void UARTTCPServerComponent::setup() {
  merged_ring_.init(rx_buffer_size_);

  tcp_server_ = new AsyncServer(port_);
  tcp_server_->onClient(
      [](void *arg, AsyncClient *client) {
        static_cast<UARTTCPServerComponent *>(arg)->accept_client_(client);
      },
      this);
  tcp_server_->begin();

  ESP_LOGI(TAG, "Listening on port %u (max_clients=%u, mode=%s)", port_, (unsigned) max_clients_,
           client_mode_ == CLIENT_MODE_FANOUT ? "fanout" : "exclusive");
}

ClientState *UARTTCPServerComponent::accept_client_(AsyncClient *client) {
  // Exclusive mode: disconnect existing clients
  if (client_mode_ == CLIENT_MODE_EXCLUSIVE) {
    for (auto *cs : clients_) {
      if (cs->connected) {
        ESP_LOGI(TAG, "Exclusive mode: disconnecting existing client %s", remote_addr_(cs->client).c_str());
        cs->client->close();
        cs->connected = false;
      }
    }
  }

  // Find a free slot
  ClientState *slot = nullptr;
  for (auto *cs : clients_) {
    if (!cs->connected) {
      slot = cs;
      break;
    }
  }

  // No free slot — try to create one
  if (!slot) {
    if (clients_.size() >= max_clients_) {
      ESP_LOGW(TAG, "Rejecting client %s — max_clients=%u reached",
               remote_addr_(client).c_str(), (unsigned) max_clients_);
      client->close();
      total_clients_rejected_++;
      return nullptr;
    }
    slot = new ClientState();
    slot->ring.init(rx_buffer_size_);
    clients_.push_back(slot);
  }

  slot->client = client;
  slot->connected = true;
  slot->ring.clear();
  slot->last_rx_byte_time = millis();
  total_clients_accepted_++;

  size_t active = 0;
  for (auto *cs : clients_)
    if (cs->connected) active++;
  ESP_LOGI(TAG, "Client connected %s (%u/%u)", remote_addr_(client).c_str(), (unsigned) active,
           (unsigned) max_clients_);

  // Wire up client callbacks
  client->onData(
      [](void *arg, AsyncClient *c, void *data, size_t len) {
        auto *cs = static_cast<ClientState *>(arg);
        cs->ring.write(static_cast<uint8_t *>(data), len);
        cs->last_rx_byte_time = millis();
      },
      slot);

  client->onDisconnect(
      [](void *arg, AsyncClient *c) {
        auto *cs = static_cast<ClientState *>(arg);
        cs->connected = false;
        ESP_LOGI(TAG, "Client disconnected %s", remote_addr_(cs->client).c_str());
      },
      slot);

  client->onError(
      [](void *arg, AsyncClient *c, int8_t error) {
        auto *cs = static_cast<ClientState *>(arg);
        ESP_LOGW(TAG, "Client error %s: %d", remote_addr_(cs->client).c_str(), error);
        cs->connected = false;
      },
      slot);

  return slot;
}

void UARTTCPServerComponent::merge_rx_() {
  for (auto *cs : clients_) {
    if (!cs->connected)
      continue;
    // Drain per-client ring into merged ring
    uint8_t tmp[128];
    while (cs->ring.available() > 0) {
      size_t n = cs->ring.read(tmp, std::min(cs->ring.available(), sizeof(tmp)));
      merged_ring_.write(tmp, n);
    }
  }
}

void UARTTCPServerComponent::loop() {
  merge_rx_();

  // Idle timeout
  if (idle_timeout_ms_ > 0) {
    for (auto *cs : clients_) {
      if (cs->connected && cs->last_rx_byte_time > 0) {
        uint32_t idle = millis() - cs->last_rx_byte_time;
        if (idle > idle_timeout_ms_) {
          ESP_LOGI(TAG, "Client %s idle for %ums, disconnecting", remote_addr_(cs->client).c_str(), idle);
          cs->client->close();
          cs->connected = false;
        }
      }
    }
  }

  // Clean up disconnected client ring residuals
  for (auto *cs : clients_) {
    if (!cs->connected)
      cs->ring.clear();
  }
}

void UARTTCPServerComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "UART TCP Server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  Max clients: %u", (unsigned) max_clients_);
  ESP_LOGCONFIG(TAG, "  Client mode: %s", client_mode_ == CLIENT_MODE_FANOUT ? "fanout" : "exclusive");
  ESP_LOGCONFIG(TAG, "  RX buffer: %u bytes (ring capacity: %u)", (unsigned) rx_buffer_size_,
                (unsigned) merged_ring_.capacity());
  ESP_LOGCONFIG(TAG, "  Idle timeout: %ums", idle_timeout_ms_);
  size_t active = 0;
  for (auto *cs : clients_)
    if (cs->connected) active++;
  ESP_LOGCONFIG(TAG, "  Active clients: %u", (unsigned) active);
  ESP_LOGCONFIG(TAG, "  Lifetime: accepted=%u rejected=%u", (unsigned) total_clients_accepted_,
                (unsigned) total_clients_rejected_);
}

// ---- UARTComponent overrides ----

void UARTTCPServerComponent::write_array(const uint8_t *data, size_t len) {
  size_t sent_count = 0;
  for (auto *cs : clients_) {
    if (!cs->connected)
      continue;
    size_t written = cs->client->write((const char *) data, len);
    if (written < len) {
      ESP_LOGW(TAG, "Client %s: only wrote %u/%u bytes", remote_addr_(cs->client).c_str(),
               (unsigned) written, (unsigned) len);
    }
    sent_count++;
  }
  if (sent_count == 0 && len > 0) {
    ESP_LOGD(TAG, "write_array: no connected clients, dropping %u bytes", (unsigned) len);
  }
}

size_t UARTTCPServerComponent::available() {
  merge_rx_();
  return merged_ring_.available() + (has_peek_ ? 1 : 0);
}

bool UARTTCPServerComponent::read_array(uint8_t *data, size_t len) {
  merge_rx_();
  size_t offset = 0;
  if (has_peek_) {
    data[offset++] = peek_buffer_;
    has_peek_ = false;
  }
  size_t got = merged_ring_.read(data + offset, len - offset);
  return (offset + got) == len;
}

bool UARTTCPServerComponent::peek_byte(uint8_t *data) {
  if (has_peek_) {
    *data = peek_buffer_;
    return true;
  }
  if (merged_ring_.read(&peek_buffer_, 1) != 1) {
    return false;
  }
  has_peek_ = true;
  *data = peek_buffer_;
  return true;
}

uart::UARTFlushResult UARTTCPServerComponent::flush() {
  // TCP is full-duplex. No hardware FIFO to drain.
  return uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS;
}

}  // namespace esphome::uart_tcp_server
