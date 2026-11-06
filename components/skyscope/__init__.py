"""Skyscope: live aircraft over a map, fetched and drawn by an ESP32-S3 screen on its own."""

import esphome.codegen as cg
from esphome.components import esp32, font, ota, output, time, wifi
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
CONF_ROUND = "round"
CONF_TIME_ID = "time_id"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_UPDATE_ID = "update_id"
CONF_COPROCESSOR_UPDATE_ID = "coprocessor_update_id"
CONF_RADIO = "radio"
CONF_GAIN = "gain"
CONF_PPM = "ppm"
CONF_TEST_API = "test_api"
CONF_ROUTES = "routes"
CONF_CARD = "card"
CONF_SLOT = "slot"
CONF_WIDTH = "width"
CONF_CLK_PIN = "clk_pin"
CONF_CMD_PIN = "cmd_pin"
CONF_DATA_PINS = "data_pins"
CONF_HOLD_HIGH_PIN = "hold_high_pin"
CONF_POWER_LDO = "power_ldo"
CONF_FREQUENCY = "frequency"

skyscope_ns = cg.esphome_ns.namespace("skyscope")
Skyscope = skyscope_ns.class_("Skyscope", cg.Component)
# The Wi-Fi networks the screen knows, first choice first (networks.h): a component of its own, started just after
# Wi-Fi.
Networks = skyscope_ns.class_("Networks", cg.Component)
CONF_NETWORKS_ID = "networks_id"

lvgl_ns = cg.esphome_ns.namespace("lvgl")
LvglComponent = lvgl_ns.class_("LvglComponent")
CONF_LVGL_ID = "lvgl_id"

web_server_base_ns = cg.esphome_ns.namespace("web_server_base")
WebServerBase = web_server_base_ns.class_("WebServerBase")

touchscreen_ns = cg.esphome_ns.namespace("touchscreen")
Touchscreen = touchscreen_ns.class_("Touchscreen")

http_request_ns = cg.esphome_ns.namespace("http_request")
HttpRequestUpdate = http_request_ns.class_("HttpRequestUpdate")

update_ns = cg.esphome_ns.namespace("update")
UpdateEntity = update_ns.class_("UpdateEntity")

def _validate_card(config):
    pins = config.get(CONF_DATA_PINS, [])
    given = CONF_CLK_PIN in config or CONF_CMD_PIN in config or pins
    if given and not (CONF_CLK_PIN in config and CONF_CMD_PIN in config and len(pins) == config[CONF_WIDTH]):
        raise cv.Invalid(
            f"A card's pins are clk_pin, cmd_pin and {config[CONF_WIDTH]} data_pins (one per bit of width)"
        )
    own_pins = config[CONF_SLOT] == 0 and esp32.get_esp32_variant() == esp32.VARIANT_ESP32P4
    if not given and not own_pins:
        raise cv.Invalid("Only the ESP32-P4's slot 0 has pins of its own: give clk_pin, cmd_pin and data_pins")
    return config


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Skyscope),
        cv.GenerateID(CONF_NETWORKS_ID): cv.declare_id(Networks),
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
        # Glass that is a circle the size of the display: the map fills it, and everything else stays inside it.
        cv.Optional(CONF_ROUND, default=False): cv.boolean,
        # The clock bedtime goes by (an sntp time with `timezone: UTC0`; the zone is a setting, applied at any time).
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        # The touch panel itself, for two fingers at once (zoom and pan): LVGL is told of only the first.
        cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(Touchscreen),
        # Firmware from the internet (an http_request update): the web page shows it and installs it.
        cv.Optional(CONF_UPDATE_ID): cv.use_id(HttpRequestUpdate),
        # The Wi-Fi chip's own firmware, carried in this one (an esp32_hosted update, type embedded): installed at once
        # whenever the chip runs another, with the screen saying so.
        cv.Optional(CONF_COPROCESSOR_UPDATE_ID): cv.use_id(UpdateEntity),
        # An RTL-SDR stick (RTL2832U with an R820T or R820T2 tuner) on the board's USB port, listening at 1090 MHz
        # (radio.h): a source of planes of the screen's own. It needs a high-speed USB host, which only the ESP32-P4
        # has. `gain` is the tuner's in dB (up to 49.6; "auto" for its own AGC), `ppm` the stick's crystal error.
        cv.Optional(CONF_RADIO): cv.All(
            cv.Schema(
                {
                    cv.Optional(CONF_GAIN, default=49.6): cv.Any(
                        cv.one_of("auto", lower=True), cv.float_range(min=0, max=49.6)
                    ),
                    cv.Optional(CONF_PPM, default=0): cv.int_range(min=-200, max=200),
                }
            ),
            esp32.only_on_variant(supported=[esp32.VARIANT_ESP32P4]),
        ),
        # The board's SD card slot, for the map card (card.h): every tile fetched is kept on it, and areas of the FAA's
        # charts and the USGS map are downloaded to it. `slot` is the SD/MMC host's: slot 0 of the ESP32-P4 has pins of
        # its own (give none); otherwise the clock, command and data pins (one, or four for `width: 4`) are given.
        # `hold_high_pin` is driven high before the card starts (DAT3 of a 1-bit bus, which selects SD mode);
        # `power_ldo` is the P4's LDO channel that powers the slot.
        cv.Optional(CONF_CARD): cv.All(
            cv.Schema(
                {
                    cv.Optional(CONF_SLOT, default=1): cv.int_range(min=0, max=1),
                    cv.Optional(CONF_WIDTH, default=1): cv.one_of(1, 4, int=True),
                    cv.Optional(CONF_CLK_PIN): cv.int_range(min=0, max=54),
                    cv.Optional(CONF_CMD_PIN): cv.int_range(min=0, max=54),
                    cv.Optional(CONF_DATA_PINS): cv.All(
                        cv.ensure_list(cv.int_range(min=0, max=54)), cv.Length(min=1, max=4)
                    ),
                    cv.Optional(CONF_HOLD_HIGH_PIN): cv.int_range(min=0, max=54),
                    cv.Optional(CONF_POWER_LDO): cv.All(
                        cv.int_range(min=1, max=4), esp32.only_on_variant(supported=[esp32.VARIANT_ESP32P4])
                    ),
                    cv.Optional(CONF_FREQUENCY, default="20MHz"): cv.All(
                        cv.frequency, cv.Range(min=400e3, max=40e6)
                    ),
                }
            ),
            _validate_card,
        ),
        # Touches and what the glass shows over the API (probe.cpp), for tests/device_test.py. Anyone on the network
        # can use them, as they can the settings.
        cv.Optional(CONF_TEST_API, default=True): cv.boolean,
        # The followed plane's route from adsbdb (follow.h). Off unless asked for: adsbdb's route data may not be copied
        # without its owner's leave, so it stays out of the published firmware.
        cv.Optional(CONF_ROUTES, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    # The LVGL parts the screen builds in C++, which ESPHome otherwise leaves out when no YAML widget names them. Done
    # while validating, before LVGL writes its configuration.
    from esphome.components.lvgl import defines as lvgl_defines

    lvgl_defines.add_lv_use("canvas", "image", "label", "flex", "obj", "line")
    # A picture of the glass as two fingers land on it, which they then zoom and pan (preview.cpp).
    lvgl_defines.add_define("LV_USE_SNAPSHOT", "1")
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
    networks = cg.new_Pvariable(config[CONF_NETWORKS_ID])
    await cg.register_component(networks, {})
    cg.add(var.set_networks(networks))
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
    cg.add(var.set_round(config[CONF_ROUND]))
    if CONF_TOUCHSCREEN_ID in config:
        cg.add(var.set_touchscreen(await cg.get_variable(config[CONF_TOUCHSCREEN_ID])))
    if CONF_TIME_ID in config:
        cg.add(var.set_clock(await cg.get_variable(config[CONF_TIME_ID])))
    if config[CONF_TEST_API]:
        cg.add_define("SKYSCOPE_TEST_API")
    if config[CONF_ROUTES]:
        cg.add_define("SKYSCOPE_ROUTES")
    if CONF_UPDATE_ID in config:
        cg.add(var.set_firmware(await cg.get_variable(config[CONF_UPDATE_ID])))
    # Told when firmware comes in by ESPHome's own ways too (sent from a computer), to say it is updating.
    ota.request_ota_state_listeners()
    if CONF_COPROCESSOR_UPDATE_ID in config:
        cg.add(var.set_coprocessor(await cg.get_variable(config[CONF_COPROCESSOR_UPDATE_ID])))
    if radio := config.get(CONF_RADIO):
        gain = radio[CONF_GAIN]
        cg.add_define("USE_SKYSCOPE_RADIO")
        cg.add(var.set_radio(-1 if gain == "auto" else int(round(gain * 10)), radio[CONF_PPM]))
        # ESP-IDF's USB host (in ESP-IDF itself before 6.0), its transfers' buffers in PSRAM, where the P4's USB DMA
        # reaches: the stream's would otherwise take internal RAM.
        if esp32.idf_version() >= cv.Version(6, 0, 0):
            esp32.add_idf_component(name="espressif/usb", ref="1.4.1")
        esp32.add_idf_sdkconfig_option("CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM", True)
    if card := config.get(CONF_CARD):
        pins = card.get(CONF_DATA_PINS, []) + [-1] * 4
        cg.add_define("USE_SKYSCOPE_CARD")
        cg.add(
            var.set_card(
                card[CONF_SLOT],
                card[CONF_WIDTH],
                card.get(CONF_CLK_PIN, -1),
                card.get(CONF_CMD_PIN, -1),
                *pins[:4],
                card.get(CONF_HOLD_HIGH_PIN, -1),
                card.get(CONF_POWER_LDO, -1),
                int(card[CONF_FREQUENCY] / 1000),
            )
        )
        # ESP-IDF's SD/MMC host and FAT, which ESPHome leaves out unless asked, and the directory functions (mkdir,
        # rename, readdir, rmdir), which it otherwise builds as stubs that always fail.
        for component in ("fatfs", "sdmmc", "esp_driver_sdmmc", "wear_levelling"):
            esp32.include_builtin_idf_component(component)
        esp32.require_fatfs()
        esp32.require_vfs_dir()
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
    # The decoders for map tiles: PNGdec (PNG, a row at a time; decode.cpp), pngle (the PNGs PNGdec will not take:
    # interlaced, or 16 bits a channel) and JPEGDEC (JPEG, with esp-dsp for the S3's SIMD).
    cg.add_library("PNGdec", "1.1.7", "https://github.com/bitbank2/PNGdec#1.1.7")
    # PNGdec takes Arduino.h on every platform but a few it names, ESP-IDF not among them (JPEGDEC names it): one of
    # those it names gives it the plain C headers. Nothing else in the build asks for it but JPEGDEC, which then also
    # compiles its reading from files, unused.
    cg.add_build_flag("-D__MCUXPRESSO")
    # JPEGDEC uses the S3's vector instructions (esp-dsp's, below) only where it sees Arduino's name for the ESP32;
    # nothing else in the build looks for that name (ESP-IDF, ESPHome, LVGL, esp-dsp, pngle). With it a JPEG decodes
    # 25 to 45% quicker: on the AMOLED, a sectional chart's map in 662 ms rather than 867, an IFR low one's in 260 rather
    # than 440 (POST /api/bench). A PNG is no quicker.
    cg.add_build_flag("-DARDUINO_ARCH_ESP32")
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
