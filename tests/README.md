# uart-link tests

Three tiers, cheapest first. All commands run from the repository root and
need `esphome` on PATH (`pip install 'esphome==2026.9.0'`).

## Config validation (tier 0)

`tests/valid/` must validate; `tests/invalid/` must be rejected with the
error sentence in the sibling `.expect` file.

    ./tests/validate.sh

## Compile fixtures (tier 0)

One representative config per platform in `tests/compile/`. Each chains all
three components through one bridge.

    esphome compile tests/compile/esp32_arduino.yaml
    esphome compile tests/compile/esp32_idf.yaml
    esphome compile tests/compile/esp8266.yaml

Note: on Apple Silicon Macs the ESP8266 toolchain needs Rosetta 2
(`softwareupdate --install-rosetta --agree-to-license`); CI runners are
x86_64 and unaffected.

## Host runtime tests (tier 1)

`tests/host/run_tests.py` compiles each scenario for ESPHome's `host:`
platform, runs the firmware binary, and checks it against plain Python
socket peers:

    python3 tests/host/run_tests.py            # all
    python3 tests/host/run_tests.py reconnect  # one scenario

Covered: client echo, reconnect after peer restart, uart_bridge
end-to-end between two TCP clients.

**Blocked: server-side scenarios.** Fanout, exclusive mode, and idle
timeout have no host coverage because `uart_tcp_server` builds on
`AsyncServer` from the ESP async_tcp libraries, and the socket fallback
that makes `host:` possible provides `AsyncClient` only. Covering these on
host means giving the server a socket-based backend on non-ESP platforms
(a second server implementation; see QUESTIONS.md Q3). Until then those
behaviors are compile-tested only.

## Unit tests

`SPSCRingBuffer` logic, no ESPHome dependency:

    ./tests/unit/run.sh

Contract notes the tests pin down: capacity rounds up to a power of two,
one slot is reserved (a 4-slot ring holds 3), and overflow drops the
oldest bytes.

## CI

`.github/workflows/ci.yml` runs all tiers on push and pull request against
`esphome==2026.9.0`. `.github/workflows/esphome_latest.yml` recompiles the
per-platform fixtures against the newest released esphome every Monday so
upstream API drift surfaces early.
