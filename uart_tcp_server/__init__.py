import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@nebulous"]
AUTO_LOAD = ["uart", "async_tcp", "uart_common"]
DEPENDENCIES = ["network"]

CONF_RX_BUFFER_SIZE = "rx_buffer_size"
CONF_MAX_CLIENTS = "max_clients"
CONF_CLIENT_MODE = "client_mode"
CONF_IDLE_TIMEOUT = "idle_timeout"

uart_tcp_server_ns = cg.esphome_ns.namespace("uart_tcp_server")
UARTTCPServerComponent = uart_tcp_server_ns.class_(
    "UARTTCPServerComponent", uart.UARTComponent, cg.Component
)
ClientMode = uart_tcp_server_ns.enum("ClientMode")

CLIENT_MODES = {
    "fanout": ClientMode.CLIENT_MODE_FANOUT,
    "exclusive": ClientMode.CLIENT_MODE_EXCLUSIVE,
}

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(UARTTCPServerComponent),
    cv.Required(CONF_PORT): cv.uint16_t,
    cv.Optional(CONF_MAX_CLIENTS, default=2): cv.All(cv.uint8_t, cv.Range(min=1, max=16)),
    cv.Optional(CONF_RX_BUFFER_SIZE, default=4096): cv.All(cv.validate_bytes, cv.uint16_t),
    cv.Optional(CONF_CLIENT_MODE, default="fanout"): cv.enum(CLIENT_MODES, lower=True),
    cv.Optional(CONF_IDLE_TIMEOUT, default="0ms"): cv.update_interval,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_clients(config[CONF_MAX_CLIENTS]))
    cg.add(var.set_rx_buffer_size(config[CONF_RX_BUFFER_SIZE]))
    cg.add(var.set_client_mode(config[CONF_CLIENT_MODE]))
    cg.add(var.set_idle_timeout(config[CONF_IDLE_TIMEOUT]))
    await cg.register_component(var, config)
