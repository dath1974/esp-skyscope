"""An RGB panel (an ST7701 and the like) whose picture the ESP32-S3 sends it all the time from a frame in PSRAM.

The panel is set up once over 3-wire SPI (9-bit words: a command, then its data) with the board's own sequence and
nothing added to it, so a panel left in its own pixel format stays in it; the SPI lines are let go afterwards (a card
may share them). Its picture then comes over 16 data lines from a frame in PSRAM: read there by the panel's DMA (the
default, `bounce_rows: 0`), or copied a few rows at a time into two buffers of internal RAM by an interrupt
(`bounce_rows`), which keeps up better but leaves the picture shifted when a copy comes late; each copy drops what it
read from the cache again. Pixels go out as LVGL draws them, in the chip's own byte order (lvgl: byte_order:
little_endian), the data lines given lowest bit first.
"""

from esphome import pins
import esphome.codegen as cg
from esphome.components import display, spi
from esphome.components.esp32 import VARIANT_ESP32S3, only_on_variant
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA_PINS,
    CONF_DIMENSIONS,
    CONF_HEIGHT,
    CONF_ID,
    CONF_INIT_SEQUENCE,
    CONF_RESET_PIN,
    CONF_WIDTH,
)
from esphome.core import TimePeriod

DEPENDENCIES = ["spi", "esp32"]

CONF_DE_PIN = "de_pin"
CONF_PCLK_PIN = "pclk_pin"
CONF_HSYNC_PIN = "hsync_pin"
CONF_VSYNC_PIN = "vsync_pin"
CONF_PCLK_FREQUENCY = "pclk_frequency"
CONF_PCLK_INVERTED = "pclk_inverted"
CONF_HSYNC_PULSE_WIDTH = "hsync_pulse_width"
CONF_HSYNC_BACK_PORCH = "hsync_back_porch"
CONF_HSYNC_FRONT_PORCH = "hsync_front_porch"
CONF_VSYNC_PULSE_WIDTH = "vsync_pulse_width"
CONF_VSYNC_BACK_PORCH = "vsync_back_porch"
CONF_VSYNC_FRONT_PORCH = "vsync_front_porch"
CONF_BOUNCE_ROWS = "bounce_rows"

# A delay in the sequence: its milliseconds, then this in place of a length.
DELAY_FLAG = 0xFF

rgb_panel_ns = cg.esphome_ns.namespace("rgb_panel")
RgbPanel = rgb_panel_ns.class_("RgbPanel", display.Display, spi.SPIDevice)


def sequence_item(value):
    """A command and its data bytes ([0x11], [0xC0, 0x4F, 0x00]), or "delay N ms"."""
    if isinstance(value, str):
        if not value.lower().startswith("delay "):
            raise cv.Invalid('A step is a list of bytes (a command and its data) or "delay N ms"')
        ms = cv.All(cv.positive_time_period_milliseconds, cv.Range(min=TimePeriod(milliseconds=1),
                                                                    max=TimePeriod(milliseconds=254)))(value[6:])
        return [int(ms.total_milliseconds), DELAY_FLAG]
    value = cv.All(cv.ensure_list(cv.hex_uint8_t), cv.Length(min=1, max=64))(value)
    return [value[0], len(value) - 1, *value[1:]]


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(RgbPanel),
            cv.Required(CONF_DIMENSIONS): cv.Schema(
                {cv.Required(CONF_WIDTH): cv.int_range(min=1, max=2048),
                 cv.Required(CONF_HEIGHT): cv.int_range(min=1, max=2048)}
            ),
            cv.Required(CONF_INIT_SEQUENCE): cv.ensure_list(sequence_item),
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_DATA_PINS): cv.All(
                cv.ensure_list(pins.internal_gpio_output_pin_schema), cv.Length(min=16, max=16)
            ),
            cv.Required(CONF_DE_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_PCLK_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_HSYNC_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_VSYNC_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_PCLK_FREQUENCY): cv.All(cv.frequency, cv.Range(min=4e6, max=40e6)),
            cv.Optional(CONF_PCLK_INVERTED, default=False): cv.boolean,
            cv.Required(CONF_HSYNC_PULSE_WIDTH): cv.int_range(min=1, max=255),
            cv.Required(CONF_HSYNC_BACK_PORCH): cv.int_range(min=0, max=255),
            cv.Required(CONF_HSYNC_FRONT_PORCH): cv.int_range(min=0, max=255),
            cv.Required(CONF_VSYNC_PULSE_WIDTH): cv.int_range(min=1, max=255),
            cv.Required(CONF_VSYNC_BACK_PORCH): cv.int_range(min=0, max=255),
            cv.Required(CONF_VSYNC_FRONT_PORCH): cv.int_range(min=0, max=255),
            cv.Optional(CONF_BOUNCE_ROWS, default=0): cv.int_range(min=0, max=80),
        }
    ).extend(spi.spi_device_schema(cs_pin_required=False, default_data_rate=4e6)),
    cv.only_on_esp32,
    only_on_variant(supported=[VARIANT_ESP32S3]),
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema("rgb_panel", require_miso=False, require_mosi=True)


def _validate_rows(config):
    # The panel's driver wants its refill buffers to divide the frame into whole pieces.
    rows, height = config[CONF_BOUNCE_ROWS], config[CONF_DIMENSIONS][CONF_HEIGHT]
    if rows and height % rows:
        raise cv.Invalid(f"bounce_rows ({rows}) must divide the panel's height ({height})", [CONF_BOUNCE_ROWS])
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _validate_rows)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    await spi.register_spi_device(var, config, write_only=True)
    dims = config[CONF_DIMENSIONS]
    cg.add(var.set_dimensions(dims[CONF_WIDTH], dims[CONF_HEIGHT]))
    sequence = [byte for step in config[CONF_INIT_SEQUENCE] for byte in step]
    cg.add(var.set_init_sequence(sequence))
    if reset := config.get(CONF_RESET_PIN):
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(reset)))
    for index, pin in enumerate(config[CONF_DATA_PINS]):
        cg.add(var.set_data_pin(index, await cg.gpio_pin_expression(pin)))
    cg.add(var.set_de_pin(await cg.gpio_pin_expression(config[CONF_DE_PIN])))
    cg.add(var.set_pclk_pin(await cg.gpio_pin_expression(config[CONF_PCLK_PIN])))
    cg.add(var.set_hsync_pin(await cg.gpio_pin_expression(config[CONF_HSYNC_PIN])))
    cg.add(var.set_vsync_pin(await cg.gpio_pin_expression(config[CONF_VSYNC_PIN])))
    cg.add(var.set_pclk(int(config[CONF_PCLK_FREQUENCY]), config[CONF_PCLK_INVERTED]))
    cg.add(var.set_hsync(config[CONF_HSYNC_PULSE_WIDTH], config[CONF_HSYNC_BACK_PORCH],
                         config[CONF_HSYNC_FRONT_PORCH]))
    cg.add(var.set_vsync(config[CONF_VSYNC_PULSE_WIDTH], config[CONF_VSYNC_BACK_PORCH],
                         config[CONF_VSYNC_FRONT_PORCH]))
    cg.add(var.set_bounce_rows(config[CONF_BOUNCE_ROWS]))
