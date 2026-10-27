"""Skyscope: live aircraft over a map, fetched and drawn by an ESP32-S3 screen on its own."""

import esphome.codegen as cg
from esphome.components import esp32, font, output
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32", "lvgl", "wifi", "psram"]
AUTO_LOAD = ["json", "web_server_base", "font"]

VERSION = "0.1.0"

CONF_DPI = "dpi"
CONF_TILE_SCALE = "tile_scale"
CONF_FONT_SMALL = "font_small"
CONF_FONT_LABEL = "font_label"
CONF_FONT_TITLE = "font_title"
CONF_USER_AGENT = "user_agent"
CONF_SETUP_NETWORK = "setup_network"
CONF_BRIGHTNESS_OUTPUT = "brightness_output"
CONF_PANEL_TE_PIN = "panel_te_pin"

skyscope_ns = cg.esphome_ns.namespace("skyscope")
Skyscope = skyscope_ns.class_("Skyscope", cg.Component)

web_server_base_ns = cg.esphome_ns.namespace("web_server_base")
WebServerBase = web_server_base_ns.class_("WebServerBase")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Skyscope),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(WebServerBase),
        # The glass's density: every size on the screen is worked out from it.
        cv.Optional(CONF_DPI, default=200.0): cv.float_range(min=80, max=600),
        # How many pixels of the glass one map tile pixel takes by default: 2 on a dense glass, so a map's words can be read.
        cv.Optional(CONF_TILE_SCALE, default=1): cv.int_range(min=1, max=2),
        cv.Required(CONF_FONT_SMALL): cv.use_id(font.Font),
        cv.Required(CONF_FONT_LABEL): cv.use_id(font.Font),
        cv.Required(CONF_FONT_TITLE): cv.use_id(font.Font),
        # Tile servers and ADS-B exchanges ask that a client says who it is.
        cv.Optional(CONF_USER_AGENT, default=f"esp-skyscope/{VERSION}"): cv.string_strict,
        # The name of the Wi-Fi network the screen opens to be set up (wifi: ap: ssid), for the screen to show.
        cv.Optional(CONF_SETUP_NETWORK, default=""): cv.string,
        cv.Optional(CONF_BRIGHTNESS_OUTPUT): cv.use_id(output.FloatOutput),
        # A panel with its own memory (a QSPI AMOLED) whose tearing signal reaches a GPIO: each frame waits for it, so
        # the radar's beam never shows in steps (panel_sync.h).
        cv.Optional(CONF_PANEL_TE_PIN, default=-1): cv.int_range(min=-1, max=48),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    # The LVGL parts the screen builds in C++, which ESPHome otherwise leaves out when no YAML widget names them. Done
    # while validating, before LVGL writes its configuration.
    from esphome.components.lvgl import defines as lvgl_defines

    lvgl_defines.add_lv_use("canvas", "image", "label", "flex", "obj")
    for key in (CONF_FONT_SMALL, CONF_FONT_LABEL, CONF_FONT_TITLE):
        lvgl_defines.get_esphome_fonts_used().add(config[key])
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_server_base(base))
    small = await cg.get_variable(config[CONF_FONT_SMALL])
    label = await cg.get_variable(config[CONF_FONT_LABEL])
    title = await cg.get_variable(config[CONF_FONT_TITLE])
    cg.add(var.set_fonts(small, label, title))
    cg.add(var.set_dpi(config[CONF_DPI]))
    cg.add(var.set_tile_scale(config[CONF_TILE_SCALE]))
    cg.add(var.set_user_agent(config[CONF_USER_AGENT]))
    cg.add(var.set_setup_network(config[CONF_SETUP_NETWORK]))
    cg.add(var.set_version(VERSION))
    cg.add(var.set_panel_te_pin(config[CONF_PANEL_TE_PIN]))
    if CONF_BRIGHTNESS_OUTPUT in config:
        out = await cg.get_variable(config[CONF_BRIGHTNESS_OUTPUT])
        cg.add(var.set_brightness_output(out))

    # HTTPS straight from the screen: ESP-IDF's HTTP client and TLS, which ESPHome leaves out unless asked, and the full
    # bundle of root certificates (tile servers and exchanges sit behind several CAs).
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")
    esp32.require_full_certificate_bundle()
    # The decoders for map tiles: pngle (PNG) and JPEGDEC (JPEG, with esp-dsp for the S3's SIMD).
    cg.add_library("pngle", "1.1.0")
    cg.add_library("JPEGDEC", "1.8.4", "https://github.com/bitbank2/JPEGDEC#1.8.4")
    esp32.add_idf_component(name="espressif/esp-dsp", ref="1.8.2")
    cg.add_define("SKYSCOPE_VERSION", VERSION)
