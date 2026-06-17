import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID
from logging import getLogger

_LOGGER = getLogger(__name__)

CODEOWNERS = ["@nebulous"]
MULTI_CONF = True
AUTO_LOAD = ["uart", "uart_common"]

CONF_UART_A = "uart_a"
CONF_UART_B = "uart_b"
CONF_BUFFER_SIZE = "buffer_size"
CONF_DIRECTION = "direction"
CONF_UARTS = "uarts"
CONF_UART = "uart"
CONF_FLOW = "flow"

uart_bridge_ns = cg.esphome_ns.namespace("uart_bridge")
UARTBridge = uart_bridge_ns.class_("UARTBridge", uart.UARTComponent, cg.Component)
Flow = uart_bridge_ns.enum("Flow")
Direction = uart_bridge_ns.enum("Direction")

FLOWS = {
    "both": Flow.FLOW_BOTH,
    "to_bridge": Flow.FLOW_TO_BRIDGE,
    "from_bridge": Flow.FLOW_FROM_BRIDGE,
}

OLD_DIRECTIONS = {
    "bidirectional": Direction.DIRECTION_BIDIRECTIONAL,
    "a_to_b": Direction.DIRECTION_A_TO_B,
    "b_to_a": Direction.DIRECTION_B_TO_A,
}

# Per-UART entry: bare use_id (an esphome ID object) or dict with uart + optional flow
UART_ENTRY = cv.Any(
    cv.use_id(uart.UARTComponent),
    cv.Schema({
        cv.Required(CONF_UART): cv.use_id(uart.UARTComponent),
        cv.Optional(CONF_FLOW, default="both"): cv.enum(FLOWS, lower=True),
    }),
)


def _uart_member(entry):
    """Split a uarts[] entry into (uart_id, flow).

    A bare entry is an esphome ID object produced by cv.use_id() (flow defaults
    to FLOW_BOTH); a dict entry carries an explicit 'flow'.
    """
    if isinstance(entry, dict):
        return entry[CONF_UART], entry[CONF_FLOW]
    return entry, Flow.FLOW_BOTH


def _validate_no_duplicates(config):
    """Ensure no UART appears twice in the members list."""
    if CONF_UARTS in config:
        seen = []
        for entry in config[CONF_UARTS]:
            uart_id, _ = _uart_member(entry)
            # cv.use_id() yields an esphome ID object whose .id is the name
            key = getattr(uart_id, "id", uart_id)
            if key in seen:
                raise cv.Invalid(
                    f"UART '{key}' appears more than once in the bridge. "
                    f"Each UART can only be a member of a bridge once."
                )
            seen.append(key)
    elif "__legacy_uart_a" in config:
        if config["__legacy_uart_a"] == config["__legacy_uart_b"]:
            raise cv.Invalid(
                "Self-loop detected: uart_a and uart_b must reference different "
                "UART components. Bridging a UART to itself creates an infinite loop."
            )
    return config


def _is_old_syntax(config):
    return CONF_UART_A in config or CONF_UART_B in config


OLD_SYNTAX_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(UARTBridge),
    cv.Required(CONF_UART_A): cv.use_id(uart.UARTComponent),
    cv.Required(CONF_UART_B): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_BUFFER_SIZE, default=512): cv.All(
        cv.validate_bytes, cv.Range(min=64, max=8192)
    ),
    cv.Optional(CONF_DIRECTION, default="bidirectional"): cv.enum(OLD_DIRECTIONS, lower=True),
}).extend(cv.COMPONENT_SCHEMA)


def _migrate_old_syntax(config):
    """Accept uart_a/uart_b with a deprecation warning, translate to new format."""
    if not _is_old_syntax(config):
        return config

    _LOGGER.warning(
        "uart_bridge: uart_a/uart_b syntax is deprecated and will be removed in a "
        "future release. Use uarts: [%s, %s] instead.",
        config[CONF_UART_A], config[CONF_UART_B],
    )

    # Translate direction to new per-member flow
    direction = config.get(CONF_DIRECTION, Direction.DIRECTION_BIDIRECTIONAL)

    new_config = {
        CONF_ID: config[CONF_ID],
        CONF_BUFFER_SIZE: config.get(CONF_BUFFER_SIZE, 512),
        # Store legacy members for codegen to use the old C++ API
        "__legacy_uart_a": config[CONF_UART_A],
        "__legacy_uart_b": config[CONF_UART_B],
        "__legacy_direction": direction,
    }
    return new_config


# --- New syntax ---

NEW_SYNTAX_SCHEMA = cv.Schema({
    cv.Optional(CONF_ID): cv.declare_id(UARTBridge),
    cv.Required(CONF_UARTS): cv.All(cv.ensure_list(cv.Any(UART_ENTRY)), cv.Length(min=2)),
    cv.Optional(CONF_BUFFER_SIZE, default=512): cv.All(
        cv.validate_bytes, cv.Range(min=64, max=8192)
    ),
}).extend(cv.COMPONENT_SCHEMA)


CONFIG_SCHEMA = cv.All(
    cv.Any(OLD_SYNTAX_SCHEMA, NEW_SYNTAX_SCHEMA),
    _migrate_old_syntax,
    _validate_no_duplicates,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))

    # Set name for logging (use the id if available)
    if CONF_ID in config and config[CONF_ID] is not None:
        cg.add(var.set_name(str(config[CONF_ID])))

    if "__legacy_uart_a" in config:
        # Old uart_a/uart_b API
        cg.add(var.set_direction(config["__legacy_direction"]))
        uart_a = await cg.get_variable(config["__legacy_uart_a"])
        uart_b = await cg.get_variable(config["__legacy_uart_b"])
        cg.add(var.set_uart_a(uart_a))
        cg.add(var.set_uart_b(uart_b))
    else:
        # New uarts: list API
        for entry in config[CONF_UARTS]:
            member_id, flow = _uart_member(entry)
            uart_var = await cg.get_variable(member_id)
            cg.add(var.add_member(uart_var, flow))

    await cg.register_component(var, config)
