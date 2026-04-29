import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_PORT

CONF_HOST = "host"

CODEOWNERS = ["@nebulous"]
AUTO_LOAD = ["uart", "async_tcp", "uart_common"]
DEPENDENCIES = ["network"]

CONF_RX_BUFFER_SIZE = "rx_buffer_size"
CONF_RECONNECT_INTERVAL = "reconnect_interval"

uart_tcp_client_ns = cg.esphome_ns.namespace("uart_tcp_client")
UARTTCPClientComponent = uart_tcp_client_ns.class_(
    "UARTTCPClientComponent", uart.UARTComponent, cg.Component
)

MULTI_CONF = True

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(UARTTCPClientComponent),
    cv.Required(CONF_HOST): cv.string,
    cv.Required(CONF_PORT): cv.uint16_t,
    cv.Optional(CONF_RX_BUFFER_SIZE, default=4096): cv.All(cv.validate_bytes, cv.uint16_t),
    cv.Optional(CONF_RECONNECT_INTERVAL, default="5s"): cv.update_interval,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_rx_buffer_size(config[CONF_RX_BUFFER_SIZE]))
    cg.add(var.set_reconnect_interval(config[CONF_RECONNECT_INTERVAL]))
    await cg.register_component(var, config)
