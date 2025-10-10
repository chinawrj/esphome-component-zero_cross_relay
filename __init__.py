"""
ESPHome 过零检测固态继电器组件 (Zero-Cross Detection Solid State Relay)

功能说明：
- 监测GPIO3的过零检测信号（高电平有效）
- 在过零点时输出控制信号到GPIO4（固态继电器）
- 提供中断计数、频率统计等监控功能

作者：GitHub Copilot
日期：2025-10-10
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

# 定义命名空间
zero_cross_relay_ns = cg.esphome_ns.namespace("zero_cross_relay")
ZeroCrossRelayComponent = zero_cross_relay_ns.class_(
    "ZeroCrossRelayComponent", cg.Component
)

# 配置键定义
CONF_ZERO_CROSS_PIN = "zero_cross_pin"
CONF_RELAY_OUTPUT_PIN = "relay_output_pin"

# 组件配置架构
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ZeroCrossRelayComponent),
        cv.Optional(CONF_ZERO_CROSS_PIN, default="GPIO3"): pins.gpio_input_pin_schema,
        cv.Optional(CONF_RELAY_OUTPUT_PIN, default="GPIO4"): pins.gpio_output_pin_schema,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    """生成C++代码"""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # 配置过零检测输入引脚
    zero_cross_pin = await cg.gpio_pin_expression(config[CONF_ZERO_CROSS_PIN])
    cg.add(var.set_zero_cross_pin(zero_cross_pin))

    # 配置继电器输出引脚
    relay_pin = await cg.gpio_pin_expression(config[CONF_RELAY_OUTPUT_PIN])
    cg.add(var.set_relay_output_pin(relay_pin))
