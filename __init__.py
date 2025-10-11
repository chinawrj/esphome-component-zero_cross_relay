"""
ESPHome Zero-Cross Detection Solid State Relay Component

Features:
- Monitors GPIO3 zero-cross detection signal (active HIGH)
- Outputs control signal to GPIO4 at zero-crossing points (solid state relay)
- Provides interrupt counting, frequency statistics and monitoring capabilities

Author: GitHub Copilot
Date: 2025-10-10
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import (
    CONF_ID,
    UNIT_HERTZ,
    ICON_PULSE,
    DEVICE_CLASS_FREQUENCY,
    STATE_CLASS_MEASUREMENT,
)

# Define namespace
zero_cross_relay_ns = cg.esphome_ns.namespace("zero_cross_relay")
ZeroCrossRelayComponent = zero_cross_relay_ns.class_(
    "ZeroCrossRelayComponent", cg.Component
)

# Configuration key definitions
CONF_ZERO_CROSS_PIN = "zero_cross_pin"
CONF_RELAY_OUTPUT_PIN = "relay_output_pin"

# Component configuration schema
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ZeroCrossRelayComponent),
        cv.Optional(CONF_ZERO_CROSS_PIN, default="GPIO3"): pins.gpio_input_pin_schema,
        cv.Optional(CONF_RELAY_OUTPUT_PIN, default="GPIO4"): pins.gpio_output_pin_schema,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    """Generate C++ code"""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Configure zero-cross detection input pin
    zero_cross_pin = await cg.gpio_pin_expression(config[CONF_ZERO_CROSS_PIN])
    cg.add(var.set_zero_cross_pin(zero_cross_pin))

    # Configure relay output pin
    relay_pin = await cg.gpio_pin_expression(config[CONF_RELAY_OUTPUT_PIN])
    cg.add(var.set_relay_output_pin(relay_pin))
