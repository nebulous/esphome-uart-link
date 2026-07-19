#include "uart_tcp_server.h"
#include "esphome/core/log.h"
#include <algorithm>

namespace esphome::uart_tcp_server {

std::string UARTTCPServerComponent::remote_addr_(AsyncClient *client) {
  char buf[24];
#if defined(USE_ESP8266)
  auto ip = client->remoteIP();
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], client->remotePort());
#else
  uint32_t addr = client->getRemoteAddress();
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (unsigned)(addr & 0xFF), (unsigned)((addr >> 8) & 0xFF),
           (unsigned)((addr >> 16) & 0xFF), (unsigned)((addr >> 24) & 0xFF), client->remotePort());
#endif
  return buf;
}

void UARTTCPServerComponent::setup() {
  merged_ring_.init(rx_buffer_size_);

  const char *id = name_.empty() ? "(no id)" : name_.c_str();
  tcp_server_ = new AsyncServer(port_);
  tcp_server_->onClient(
      [](void *arg, AsyncClient *client) {
        static_cast<UARTTCPServerComponent *>(arg)->accept_client_(client);
      },
      this);
  tcp_server_->begin();

  ESP_LOGI(TAG, "'%s' listening on port %u (max_clients=%u, mode=%s)", id, port_, (unsigned) max_clients_,
           client_mode_ == CLIENT_MODE_FANOUT ? "fanout" : "exclusive");
}

ClientState *UARTTCPServerComponent::accept_client_(AsyncClient *client) {
  // Exclusive mode: disconnect existing clients
  if (client_mode_ == CLIENT_MODE_EXCLUSIVE) {
    for (auto *cs : clients_) {
      if (cs->connected) {
        ESP_LOGI(TAG, "'%s' exclusive mode: disconnecting existing client %s",
                 name_.empty() ? "(no id)" : name_.c_str(), remote_addr_(cs->client).c_str());
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
      ESP_LOGD(TAG, "'%s' rejecting client %s: max_clients=%u reached",
               name_.empty() ? "(no id)" : name_.c_str(), remote_addr_(client).c_str(), (unsigned) max_clients_);
      client->close();
      total_clients_rejected_++;
      return nullptr;
    }
    slot = new ClientState();
    slot->ring.init(rx_buffer_size_);
    if (tx_buffer_size_ > 0)
      slot->tx_ring.init(tx_buffer_size_);
    clients_.push_back(slot);
  }

  slot->client = client;
  slot->connected = true;
  slot->ring.clear();
  slot->tx_ring.clear();
  slot->last_rx_byte_time = millis();
  slot->server = this;
  total_clients_accepted_++;

  size_t active = 0;
  for (auto *cs : clients_)
    if (cs->connected) active++;
  ESP_LOGI(TAG, "'%s' client connected %s (%u/%u)",
           name_.empty() ? "(no id)" : name_.c_str(),
           remote_addr_(client).c_str(), (unsigned) active,
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
        const char *id = cs->server->name_.empty() ? "(no id)" : cs->server->name_.c_str();
        ESP_LOGI(TAG, "'%s' client disconnected %s",
                 id, remote_addr_(cs->client).c_str());
      },
      slot);

  client->onError(
      [](void *arg, AsyncClient *c, int8_t error) {
        auto *cs = static_cast<ClientState *>(arg);
        const char *id = cs->server->name_.empty() ? "(no id)" : cs->server->name_.c_str();
        ESP_LOGW(TAG, "'%s' client error %s: %d",
                 id, remote_addr_(cs->client).c_str(), error);
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

void UARTTCPServerComponent::drain_tx_() {
  uint8_t tmp[512];
  for (auto *cs : clients_) {
    if (!cs->connected)
      continue;
    // Push queued bytes into the TCP send buffer as space opens. Bounded by
    // space() per pass; ACKs free more across later loops, so no spinning.
    while (cs->tx_ring.available() > 0) {
      size_t sp = cs->client->space();
      if (sp == 0)
        break;
      size_t n = std::min({cs->tx_ring.available(), sp, sizeof(tmp)});
      n = cs->tx_ring.read(tmp, n);  // consumes from the ring
      if (n == 0)
        break;
      // COPY flag: tmp is stack-local, so LWIP must duplicate it. ESP8266's
      // default is a no-copy reference, unsafe for a stack buffer.
      size_t written = cs->client->write((const char *) tmp, n, ASYNC_WRITE_FLAG_COPY);
      if (written < n) {
        // space() promised room but write() took less: the client is closing.
        // Bytes already consumed from the ring are lost; stop draining it.
        total_tx_dropped_ += (n - written);
        ESP_LOGW(TAG, "'%s' client %s: short write, dropped %u bytes",
                 name_.empty() ? "(no id)" : name_.c_str(),
                 remote_addr_(cs->client).c_str(), (unsigned) (n - written));
        break;
      }
    }
  }
}

void UARTTCPServerComponent::enqueue_tx_(ClientState *cs, const uint8_t *data, size_t len) {
  // One sentinel slot is reserved, so usable free space is capacity - 1.
  size_t free_space = (cs->tx_ring.capacity() - 1) - cs->tx_ring.available();
  if (len <= free_space) {
    cs->tx_ring.write(data, len);
    return;
  }
  // Buffer full: the client is slower than the stream. Accept what fits and
  // drop the rest rather than block or evict queued (in-flight) bytes.
  if (free_space > 0)
    cs->tx_ring.write(data, free_space);
  total_tx_dropped_ += (len - free_space);
  ESP_LOGW(TAG, "'%s' client %s: TX buffer full, dropped %u bytes",
           name_.empty() ? "(no id)" : name_.c_str(),
           remote_addr_(cs->client).c_str(), (unsigned) (len - free_space));
}

void UARTTCPServerComponent::loop() {
  merge_rx_();
  drain_tx_();

  // Idle timeout
  if (idle_timeout_ms_ > 0) {
    for (auto *cs : clients_) {
      if (cs->connected && cs->last_rx_byte_time > 0) {
        uint32_t idle = millis() - cs->last_rx_byte_time;
        if (idle > idle_timeout_ms_) {
          ESP_LOGI(TAG, "'%s' client %s idle for %ums, disconnecting",
                   name_.empty() ? "(no id)" : name_.c_str(),
                   remote_addr_(cs->client).c_str(), (unsigned) idle);
          cs->client->close();
          cs->connected = false;
        }
      }
    }
  }

  // Clean up disconnected client ring residuals
  for (auto *cs : clients_) {
    if (!cs->connected) {
      cs->ring.clear();
      cs->tx_ring.clear();
    }
  }
}

void UARTTCPServerComponent::dump_config() {
  const char *id = name_.empty() ? "(no id)" : name_.c_str();
  ESP_LOGCONFIG(TAG, "UART TCP Server '%s':", id);
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  Max clients: %u", (unsigned) max_clients_);
  ESP_LOGCONFIG(TAG, "  Client mode: %s", client_mode_ == CLIENT_MODE_FANOUT ? "fanout" : "exclusive");
  ESP_LOGCONFIG(TAG, "  RX buffer: %u bytes (ring capacity: %u)", (unsigned) rx_buffer_size_,
                (unsigned) merged_ring_.capacity());
  if (tx_buffer_size_ > 0)
    ESP_LOGCONFIG(TAG, "  TX buffer: %u bytes/client", (unsigned) tx_buffer_size_);
  else
    ESP_LOGCONFIG(TAG, "  TX buffer: disabled (drops on short write)");
  ESP_LOGCONFIG(TAG, "  Idle timeout: %ums", (unsigned) idle_timeout_ms_);
  size_t active = 0;
  for (auto *cs : clients_)
    if (cs->connected) active++;
  ESP_LOGCONFIG(TAG, "  Active clients: %u", (unsigned) active);
  ESP_LOGCONFIG(TAG, "  Lifetime: accepted=%u rejected=%u tx_dropped=%u",
                (unsigned) total_clients_accepted_, (unsigned) total_clients_rejected_,
                (unsigned) total_tx_dropped_);
}

// ---- UARTComponent overrides ----

void UARTTCPServerComponent::write_array(const uint8_t *data, size_t len) {
  size_t sent_count = 0;
  for (auto *cs : clients_) {
    if (!cs->connected)
      continue;
    size_t offset = 0;
    // Write straight to the TCP send buffer when there is no backlog (or no
    // TX buffer configured). COPY makes LWIP duplicate the data; ESP8266's
    // default is a no-copy reference, unsafe for a transient caller buffer.
    if (tx_buffer_size_ == 0 || cs->tx_ring.available() == 0) {
      offset = cs->client->write((const char *) data, len, ASYNC_WRITE_FLAG_COPY);
      if (offset > len)  // defensive clamp
        offset = len;
    }
    if (offset < len) {
      if (tx_buffer_size_ > 0) {
        // Queue the remainder; drained from loop() as ACKs free send-buffer
        // space, so this never blocks the main loop or stalls UART RX.
        enqueue_tx_(cs, data + offset, len - offset);
      } else {
        // No TX buffering: drop what the send buffer would not take.
        total_tx_dropped_ += (len - offset);
        ESP_LOGW(TAG, "'%s' client %s: send buffer full, dropped %u/%u bytes",
                 name_.empty() ? "(no id)" : name_.c_str(),
                 remote_addr_(cs->client).c_str(), (unsigned) (len - offset), (unsigned) len);
      }
    }
    sent_count++;
  }
  if (sent_count == 0 && len > 0) {
    ESP_LOGV(TAG, "'%s' write_array: no connected clients, dropping %u bytes",
             name_.empty() ? "(no id)" : name_.c_str(), (unsigned) len);
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
