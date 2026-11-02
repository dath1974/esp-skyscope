#!/usr/bin/env python3
"""The ESP-Hosted version the installed ESPHome builds a P4's side of the Wi-Fi link with: it pins espressif/esp_hosted
in its esp32_hosted component. The Wi-Fi chip's firmware a board carries (boards/esp-hosted/) must be that version.

Run it with the Python ESPHome is installed in (the Makefile finds it from the `esphome` command).
"""

from pathlib import Path
import re
import sys

import esphome.components.esp32_hosted as hosted

source = Path(hosted.__file__).read_text()
found = re.search(r'name="espressif/esp_hosted",\s*ref="([^"]+)"', source)
if not found:
    sys.exit("ESPHome's esp32_hosted no longer pins espressif/esp_hosted where this looks")
print(found.group(1))
