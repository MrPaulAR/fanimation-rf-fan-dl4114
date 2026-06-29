"""ESPHome custom component: rf_fan (Hampton Bay / Fanimation CC1101 fan driver).

This component is the ESPHome replacement for the `hampton-bay-fan-mqtt`
PlatformIO sketch. It owns the radio (a CC1101 on hardware SPI driven by
the LSatan SmartRC library and the vendored RCSwitch), encodes/decodes
the 12-bit Fanimation protocol with RCSwitch protocol **13**, and exposes
four entities to Home Assistant:

  * one ``fan`` entity (3 preset modes: I, II, III)
  * one ``light`` entity (down light, binary on/off)
  * one ``switch`` entity (top light toggle)
  * one ``switch`` entity (direction reverse toggle)

Class layout:

  * ``RFFan`` — ``Component`` + ``fan::Fan`` (entity #1). Owns the CC1101
    + RCSwitch + the state machine. Self-registers as the fan entity via
    ``fan.register_fan``.
  * ``RFLightOutput`` — small ``light::LightOutput`` (forwarded to
    ``RFFan::send_down_light_toggle()``). Wrapped in a code-generated
    ``light::LightState`` that becomes entity #2 (the down light).
  * ``RFSwitch`` (x2) — ``switch_::Switch`` + ``Component`` (entities #3,
    #4) with a "kind" enum distinguishing top-light-toggle from
    direction-reverse-toggle. Both call back into the parent RFFan.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import fan, light, switch
from esphome.const import CONF_ID, CONF_NAME
from esphome.core import CORE
from esphome import pins

# The SmartRC-CC1101-Driver-Lib `#include`s Arduino's `<SPI.h>`. ESPHome only
# adds the Arduino SPI library to the linker lines if the user declares a
# `spi:` block — we don't want to require that (the legacy sketch handled SPI
# itself via ELECHOUSE). So we declare the SPI library directly here.
DEPENDENCIES = []
AUTO_LOAD = ["fan", "light", "switch"]

# Configuration keys
CONF_CC1101_CS_PIN = "cc1101_cs_pin"
CONF_CC1101_GDO0_PIN = "cc1101_gdo0_pin"
CONF_CC1101_GDO2_PIN = "cc1101_gdo2_pin"
CONF_RX_FREQUENCY = "rx_frequency"
CONF_TX_FREQUENCY = "tx_frequency"
CONF_DIP_ID = "dip_id"
CONF_FADE = "fade"
CONF_RX_DEBOUNCE_MS = "rx_debounce_ms"
CONF_REPEAT_TRANSMIT = "repeat_transmit"
CONF_FAN = "fan"
CONF_LIGHT = "light"
CONF_TOP_LIGHT = "top_light"
CONF_DIRECTION = "direction"

# Pre-set mode strings the fan entity exposes. Match the physical
# remote's silkscreen: the DL-4114 (and other Fanimation-family
# remotes paired with receivers that only accept 3 speeds) has three
# buttons labelled 1/2/3.  The RF protocol still has codes for I..VI
# internally, so we map the 3 visible presets to the first 3 codes.
FAN_PRESET_MODES = ["I", "II", "III"]

rf_fan_ns = cg.esphome_ns.namespace("rf_fan")

# RFFan IS the fan entity (inherits fan::Fan) so it self-registers.
RFFan = rf_fan_ns.class_("RFFan", cg.Component, fan.Fan)

# RFLightOutput is the LightOutput wrapped in a LightState.
RFLightOutput = rf_fan_ns.class_("RFLightOutput", light.LightOutput)

# RFSwitch is a Switch subclass; one instance per toggle entity.
RFSwitch = rf_fan_ns.class_("RFSwitch", switch.Switch, cg.Component)


# Sub-block schemas:
# - Use the standard fan/light/switch schemas so the user gets all
#   ENTITY_BASE_SCHEMA options (name, icon, entity_category, etc.)
#   for free, and register_<entity>() later works without surprises.
LIGHT_SCHEMA = light.light_schema(RFLightOutput, light.LightType.BINARY)
SWITCH_SCHEMA = switch.switch_schema(RFSwitch)
FAN_SCHEMA = fan.fan_schema(RFFan)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RFFan),
            cv.Required(CONF_CC1101_CS_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_CC1101_GDO0_PIN): pins.gpio_input_pin_schema,
            cv.Required(CONF_CC1101_GDO2_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RX_FREQUENCY, default=303.870): cv.float_range(
                min=300.0, max=464.0
            ),
            cv.Optional(CONF_TX_FREQUENCY, default=303.870): cv.float_range(
                min=300.0, max=464.0
            ),
            cv.Required(CONF_DIP_ID): cv.int_range(min=0, max=15),
            cv.Optional(CONF_FADE, default=False): cv.boolean,
            cv.Optional(CONF_RX_DEBOUNCE_MS, default=300): cv.int_range(
                min=0, max=5000
            ),
            cv.Optional(CONF_REPEAT_TRANSMIT, default=7): cv.int_range(
                min=1, max=20
            ),
            cv.Optional(CONF_FAN): FAN_SCHEMA,
            cv.Optional(CONF_LIGHT): LIGHT_SCHEMA,
            cv.Optional(CONF_TOP_LIGHT): SWITCH_SCHEMA,
            cv.Optional(CONF_DIRECTION): SWITCH_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    """Emit RFFan + RFLightOutput + LightState + two RFSwitch instances."""
    # Single RFFan instance — drives the radio AND serves as the fan entity.
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cs_pin = await cg.gpio_pin_expression(config[CONF_CC1101_CS_PIN])
    gdo0_pin = await cg.gpio_pin_expression(config[CONF_CC1101_GDO0_PIN])
    gdo2_pin = await cg.gpio_pin_expression(config[CONF_CC1101_GDO2_PIN])
    cg.add(var.set_pins(cs_pin, gdo0_pin, gdo2_pin))
    cg.add(var.set_rx_frequency(config[CONF_RX_FREQUENCY]))
    cg.add(var.set_tx_frequency(config[CONF_TX_FREQUENCY]))
    cg.add(var.set_dip_id(config[CONF_DIP_ID]))
    cg.add(var.set_fade(config[CONF_FADE]))
    cg.add(var.set_rx_debounce_ms(config[CONF_RX_DEBOUNCE_MS]))
    cg.add(var.set_repeat_transmit(config[CONF_REPEAT_TRANSMIT]))

    # Fan sub-block: register ``var`` itself as the fan entity.
    # Pre-poulate CONF_ID on the sub-config with the outer RFFan id so
    # fan.register_fan uses our single RFFan instance instead of wrapping
    # its own.
    if CONF_FAN in config:
        fan_cfg = dict(config[CONF_FAN])
        fan_cfg[CONF_ID] = config[CONF_ID]
        await fan.register_fan(var, fan_cfg)
    else:
        # Fall back to a nameless entity if the user omitted the block,
        # so setup() still runs and the radio still works.
        await fan.register_fan(var, {CONF_ID: config[CONF_ID], CONF_NAME: "Fan"})

    # Light sub-block: create the RFLightOutput (LightOutput), then a
    # LightState that wraps it.  Pass the LightOutput into light.register_light,
    # which generates the LightState C++ object and finalizes its name/icon.
    if CONF_LIGHT in config:
        light_cfg = config[CONF_LIGHT]
        light_output = cg.new_Pvariable(light_cfg[light.CONF_OUTPUT_ID], var)
        await light.register_light(light_output, light_cfg)
        cg.add(var.set_light_output(light_output))

    # Top-light switch
    if CONF_TOP_LIGHT in config:
        top_cfg = config[CONF_TOP_LIGHT]
        top_switch = cg.new_Pvariable(top_cfg[CONF_ID])
        await cg.register_component(top_switch, top_cfg)
        await switch.register_switch(top_switch, top_cfg)
        cg.add(top_switch.set_parent(var))
        cg.add(top_switch.set_kind(0))  # 0 = top light
        cg.add(var.set_top_light_switch(top_switch))

    # Direction-reverse switch
    if CONF_DIRECTION in config:
        dir_cfg = config[CONF_DIRECTION]
        dir_switch = cg.new_Pvariable(dir_cfg[CONF_ID])
        await cg.register_component(dir_switch, dir_cfg)
        await switch.register_switch(dir_switch, dir_cfg)
        cg.add(dir_switch.set_parent(var))
        cg.add(dir_switch.set_kind(1))  # 1 = direction
        cg.add(var.set_direction_switch(dir_switch))

    cg.add_define("USE_RF_FAN")
    # SmartRC-CC1101-Driver-Lib `#include`s the Arduino `<SPI.h>` library.
    # ESPHome only ships SPI to the linker if the user declares a top-level
    # `spi:` block — we don't require users to do that (the legacy sketch
    # handled SPI itself via ELECHOUSE), so we pull it in directly, mirroring
    # esphome/components/spi/__init__.py:405.
    if CORE.using_arduino and not CORE.is_esp32:
        cg.add_library("SPI", None)
    cg.add_library(
        "SmartRC-CC1101-Driver-Lib",
        None,
        "https://github.com/LSatan/SmartRC-CC1101-Driver-Lib.git#master",
    )