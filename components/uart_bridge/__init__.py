import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@nebulous"]
MULTI_CONF = True
AUTO_LOAD = ["uart"]

CONF_UART_A = "uart_a"
CONF_UART_B = "uart_b"
CONF_BUFFER_SIZE = "buffer_size"
CONF_DIRECTION = "direction"

uart_bridge_ns = cg.esphome_ns.namespace("uart_bridge")
UARTBridge = uart_bridge_ns.class_("UARTBridge", cg.Component)
Direction = uart_bridge_ns.enum("Direction")

DIRECTIONS = {
    "bidirectional": Direction.DIRECTION_BIDIRECTIONAL,
    "a_to_b": Direction.DIRECTION_A_TO_B,
    "b_to_a": Direction.DIRECTION_B_TO_A,
}

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(UARTBridge),
    cv.Required(CONF_UART_A): cv.use_id(uart.UARTComponent),
    cv.Required(CONF_UART_B): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_BUFFER_SIZE, default=512): cv.All(cv.validate_bytes, cv.Range(min=64, max=8192)),
    cv.Optional(CONF_DIRECTION, default="bidirectional"): cv.enum(DIRECTIONS, lower=True),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    uart_a = await cg.get_variable(config[CONF_UART_A])
    uart_b = await cg.get_variable(config[CONF_UART_B])
    cg.add(var.set_uart_a(uart_a))
    cg.add(var.set_uart_b(uart_b))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(var.set_direction(config[CONF_DIRECTION]))
    await cg.register_component(var, config)
