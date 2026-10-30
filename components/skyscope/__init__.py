"""Skyscope: live aircraft over a map, fetched and drawn by an ESP32-S3 screen on its own."""

import esphome.codegen as cg
from esphome.components import esp32, font, output, time, wifi
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = []
DEPENDENCIES = ["esp32", "lvgl", "wifi", "psram"]
AUTO_LOAD = ["json", "web_server_base", "font"]

# A build without `esphome: project: version:` (skyscope.yaml sets it, and the update check compares it).
DEV_VERSION = "dev"

CONF_DPI = "dpi"
CONF_TILE_SCALE = "tile_scale"
CONF_FONT_SMALL = "font_small"
CONF_FONT_LABEL = "font_label"
CONF_FONT_TITLE = "font_title"
CONF_USER_AGENT = "user_agent"
CONF_SETUP_NETWORK = "setup_network"
CONF_BRIGHTNESS_OUTPUT = "brightness_output"
CONF_PANEL_TE_PIN = "panel_te_pin"
CONF_TIME_ID = "time_id"
CONF_UPDATE_ID = "update_id"
CONF_TEST_API = "test_api"

skyscope_ns = cg.esphome_ns.namespace("skyscope")
Skyscope = skyscope_ns.class_("Skyscope", cg.Component)

lvgl_ns = cg.esphome_ns.namespace("lvgl")
LvglComponent = lvgl_ns.class_("LvglComponent")
CONF_LVGL_ID = "lvgl_id"

web_server_base_ns = cg.esphome_ns.namespace("web_server_base")
WebServerBase = web_server_base_ns.class_("WebServerBase")

http_request_ns = cg.esphome_ns.namespace("http_request")
HttpRequestUpdate = http_request_ns.class_("HttpRequestUpdate")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Skyscope),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(WebServerBase),
        # The LVGL component whose rotation the settings page sets.
        cv.GenerateID(CONF_LVGL_ID): cv.use_id(LvglComponent),
        # The glass's density: every size on the screen is worked out from it.
        cv.Optional(CONF_DPI, default=200.0): cv.float_range(min=80, max=600),
        # How many pixels of the glass one map tile pixel takes by default: 2 on a dense glass, so a map's words can be read.
        cv.Optional(CONF_TILE_SCALE, default=1): cv.int_range(min=1, max=2),
        cv.Required(CONF_FONT_SMALL): cv.use_id(font.Font),
        cv.Required(CONF_FONT_LABEL): cv.use_id(font.Font),
        cv.Required(CONF_FONT_TITLE): cv.use_id(font.Font),
        # Tile servers and ADS-B exchanges ask that a client says who it is; esp-skyscope/<version> unless set.
        cv.Optional(CONF_USER_AGENT): cv.string_strict,
        # The name of the Wi-Fi network the screen opens to be set up (wifi: ap: ssid), for the screen to show.
        cv.Optional(CONF_SETUP_NETWORK, default=""): cv.string,
        cv.Optional(CONF_BRIGHTNESS_OUTPUT): cv.use_id(output.FloatOutput),
        # A panel with its own memory (a QSPI AMOLED) whose tearing signal reaches a GPIO: each frame waits for it, so
        # the radar's beam never shows in steps (panel_sync.h).
        cv.Optional(CONF_PANEL_TE_PIN, default=-1): cv.int_range(min=-1, max=48),
        # The clock bedtime goes by (an sntp time with `timezone: UTC0`; the zone is a setting, applied at any time).
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        # Firmware from the internet (an http_request update): the web page shows it and installs it.
        cv.Optional(CONF_UPDATE_ID): cv.use_id(HttpRequestUpdate),
        # Touches and what the glass shows over the API (probe.cpp), for tests/device_test.py. Anyone on the network
        # can use them, as they can the settings.
        cv.Optional(CONF_TEST_API, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    # The LVGL parts the screen builds in C++, which ESPHome otherwise leaves out when no YAML widget names them. Done
    # while validating, before LVGL writes its configuration.
    from esphome.components.lvgl import defines as lvgl_defines

    lvgl_defines.add_lv_use("canvas", "image", "label", "flex", "obj", "line")
    for key in (CONF_FONT_SMALL, CONF_FONT_LABEL, CONF_FONT_TITLE):
        lvgl_defines.get_esphome_fonts_used().add(config[key])
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _version():
    return CORE.config.get("esphome", {}).get("project", {}).get("version", DEV_VERSION)


async def to_code(config):
    version = _version()
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_server_base(base))
    cg.add(var.set_lvgl(await cg.get_variable(config[CONF_LVGL_ID])))
    small = await cg.get_variable(config[CONF_FONT_SMALL])
    label = await cg.get_variable(config[CONF_FONT_LABEL])
    title = await cg.get_variable(config[CONF_FONT_TITLE])
    cg.add(var.set_fonts(small, label, title))
    cg.add(var.set_dpi(config[CONF_DPI]))
    cg.add(var.set_tile_scale(config[CONF_TILE_SCALE]))
    cg.add(var.set_user_agent(config.get(CONF_USER_AGENT, f"esp-skyscope/{version}")))
    cg.add(var.set_setup_network(config[CONF_SETUP_NETWORK]))
    cg.add(var.set_version(version))
    cg.add(var.set_panel_te_pin(config[CONF_PANEL_TE_PIN]))
    if CONF_TIME_ID in config:
        cg.add(var.set_clock(await cg.get_variable(config[CONF_TIME_ID])))
    if config[CONF_TEST_API]:
        cg.add_define("SKYSCOPE_TEST_API")
    if CONF_UPDATE_ID in config:
        cg.add(var.set_firmware(await cg.get_variable(config[CONF_UPDATE_ID])))
    # The menu's Wi-Fi list: told of every scan, and ESPHome keeps every network a scan found (not only its own).
    wifi.request_wifi_scan_results_listener()
    wifi.request_wifi_scan_results()
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
    # Ordinary allocations of 4 KB and more in PSRAM: ESPHome leaves PSRAM to code that asks for it by name, so every
    # list, string and decoder's state shared some 300 KB of internal RAM, and with 35 planes on a map an allocation
    # failed and aborted the screen. Smaller allocations stay internal (fast); task stacks, Wi-Fi, lwIP and TLS allocate
    # internal RAM by name and keep it, and 32 KB of it is held back for them.
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_CAPS_ALLOC", False)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_MALLOC", True)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL", 4096)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL", 32768)
    # Sockets: ESPHome's 10 are shared by the web server's sessions, OTA, mDNS, SNTP, the feed's and tiles' kept
    # connections and the update check, and with one more open (an ESPHome firmware download that failed leaves its
    # connection open until a restart) every new connection, the page's too, was turned away for minutes.
    esp32.add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 16)
    # TLS in PSRAM too: some 40 KB a connection, which internal RAM had to hold for every connection kept open.
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC", False)
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC", True)
    cg.add_define("SKYSCOPE_VERSION", version)
