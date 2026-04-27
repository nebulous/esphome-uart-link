# esphome-uart-link

ESPHome external components for UART transport — bridge hardware serial ports to TCP networks and each other. Transport-agnostic: any UART consumer sees the standard `available()` / `read_array()` / `write_array()` interface regardless of whether bytes come from GPIO pins, a TCP socket, or another UART.

## Components

| Component | Purpose |
|---|---|
| **uart_common** | Header-only SPSC ring buffer shared by the TCP components |
| **uart_tcp_client** | Outbound TCP client that acts as a `UARTComponent` |
| **uart_tcp_server** | TCP server that acts as a `UARTComponent` |
| **uart_bridge** | Bidirectional byte forwarder between any two UARTs |

```mermaid
graph TD
    bridge["uart_bridge<br/>(A ◄──► B)"]
    bridge --- gpio["uart<br/>(GPIO)"]
    bridge --- tcp_client["uart_tcp_client<br/>(connect)"]
    bridge --- tcp_server["uart_tcp_server<br/>(listen)"]

    tcp_client --- common["uart_common<br/>SPSCRingBuffer"]
    tcp_server --- common
```

## Installation

Add to your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/nebulous/esphome-uart-link
```

## Component Reference

### `uart_common`

Internal utility — no YAML configuration. Provides `SPSCRingBuffer`, a single-producer / single-consumer ring buffer with power-of-2 capacity and overflow-drops-oldest semantics. Used by `uart_tcp_client` and `uart_tcp_server` for thread-safe buffering between TCP callbacks and the main loop.

### `uart_tcp_client`

Connects to a remote TCP server and presents the connection as a `UARTComponent`. Any UART consumer wired to it will read/write over TCP.

```yaml
uart_tcp_client:
  id: remote_serial
  host: 192.168.1.100
  port: 5000
  rx_buffer_size: 4096       # ring buffer size (default 4096)
  reconnect_interval: 5s     # auto-reconnect on disconnect (default 5s)
```

Includes stall detection — if no bytes arrive for 15 seconds, it forces a reconnect.

### `uart_tcp_server`

Listens on a TCP port. Accepted clients read and write bytes through the `UARTComponent` interface. Each client gets its own ring buffer; bytes from all clients are merged into a single read stream.

```yaml
uart_tcp_server:
  id: tcp_serial
  port: 5000
  max_clients: 2             # simultaneous connections (default 2, max 16)
  rx_buffer_size: 4096       # per-client ring buffer (default 4096)
  client_mode: fanout        # fanout (default) or exclusive
  idle_timeout: 0ms          # kick idle clients (0 = disabled)
```

**Client modes:**
- `fanout` — all connected clients see the same TX stream. Good for multi-monitor/tap scenarios.
- `exclusive` — only one client at a time. New connections disconnect the previous client. Better for command-response protocols.

### `uart_bridge`

Bidirectional byte forwarder between two UART references. Works with any combination of hardware UART, TCP client, TCP server, USB CDC ACM.

```yaml
uart:
  - id: rs485_bus
    tx_pin: GPIO17
    rx_pin: GPIO18
    baud_rate: 38400

uart_tcp_server:
  id: tcp_bus
  port: 5000

uart_bridge:
  uart_a: rs485_bus
  uart_b: tcp_bus
  buffer_size: 512           # internal copy buffer (default 512)
  direction: bidirectional    # bidirectional | a_to_b | b_to_a
```

**Supported topologies:**

| A | B | Use Case |
|---|---|---|
| hardware UART | hardware UART | RS485 ↔ RS232 protocol converter |
| hardware UART | tcp_server | Serial-to-network bridge (replaces socat/ser2net) |
| tcp_client | hardware UART | Remote serial port consumer |
| tcp_client | tcp_server | Network serial proxy/repeater |
| tcp_server | tcp_server | Multi-party bus tap |

## Example: Serial-to-Network Bridge

Replace an external socat/ser2net toolchain entirely within ESPHome:

```yaml
uart:
  - id: serial_port
    tx_pin: GPIO4
    rx_pin: GPIO5
    baud_rate: 9600

uart_tcp_server:
  id: network_port
  port: 5000
  client_mode: exclusive

uart_bridge:
  uart_a: serial_port
  uart_b: network_port
```

Then from any machine on the network: `telnet esp-device.local 5000`

## Example: Remote UART Consumer

Wire a UART-based component to a remote TCP endpoint instead of local GPIO:

```yaml
uart_tcp_client:
  id: remote_uart
  host: 192.168.1.100
  port: 5000

# Any standard UART consumer works unchanged:
modbus_controller:
  uart_id: remote_uart
  # ...
```

## Design Notes

### Thread safety

`uart_tcp_client` and `uart_tcp_server` receive data in TCP callbacks that fire from a TCP thread (ESP32) or the main loop (ESP8266). The SPSC ring buffer in `uart_common` handles the producer/consumer split: TCP callback writes, main loop reads. No mutex needed.

### Backpressure

`uart_bridge` has no flow control. If a destination can't keep up, bytes buffer in its transport layer (DMA/FIFO for hardware UART, AsyncClient send buffer for TCP). The bridge assumes both sides can keep up. For very high baud rates, increase `buffer_size`.

### Poll-based limitation

ESPHome's UART API is purely poll-based (`available()` / `read_array()`). There are no RX callbacks. The bridge must live in `loop()`, which fires every few ms. At 115200 baud (~11.5 bytes/ms) and below, loop timing shouldn't be the bottleneck on ESP32 or ESP8266 — the UART FIFO and driver-level buffering handle it comfortably. At higher rates (460800+), the gap between `loop()` invocations can exceed the hardware FIFO depth, and you may need to shrink the loop interval or increase `buffer_size`.

## License

MIT
