# The Wi-Fi chip's firmware

`network_adapter_esp32c6-<version>.bin` is Espressif's [ESP-Hosted](https://github.com/espressif/esp-hosted-mcu)
firmware for the ESP32-C6 that gives the P4 board its Wi-Fi, exactly as ESPHome builds and publishes it
([esphome/esp-hosted-firmware](https://github.com/esphome/esp-hosted-firmware), Apache-2.0). It is copied here so a
build needs no download, and checked by its SHA-256 at every build.

The P4 board's firmware carries it (`boards/waveshare-p4-lcd-4c.yaml`, `update: platform: esp32_hosted`). At every
start the screen asks the C6 which version it runs, and if that is not this one it installs this one over the SDIO
link between the two chips, saying **Updating the Wi-Fi chip** on the glass, then restarts. So the C6 runs what this
firmware was built for, whatever the board came with.

## A new version

It must be the version ESPHome builds the P4's side of the link with (ESPHome pins `espressif/esp_hosted`; `make
firmware` refuses a board whose file names another). When `ESPHOME_VERSION` moves and that version with it:

1. Find the new version in ESPHome's manifest, https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json,
   with its URL and SHA-256.
2. Download it here as `network_adapter_esp32c6-<version>.bin`, check its SHA-256 against the manifest's, and remove
   the old one.
3. Put the new name and SHA-256 in the board's `update:` block.

## A C6 that cannot take the update

The update needs the C6 to run some ESP-Hosted already, as it does from Waveshare. If it runs something else, or an
interrupted install left it with nothing, write this image to it directly with a USB-serial adapter (3.3 V):

1. Join the adapter's ground, RX and TX to the board's C6 serial connector (`J4` on the schematic, the C6's own TX and
   RX, `C6_U0TXD` and `C6_U0RXD`; the adapter's RX to the C6's TX).
2. Hold the C6's boot button (`Key4`, on the C6's GPIO9) while the board powers up, so the C6 starts in its download
   mode.
3. This image is the C6's application only, with no bootloader or partition table, so it cannot be written on its own
   to an empty C6. Build ESP-Hosted's `slave` project at this version for the `esp32c6` with ESP-IDF instead and flash
   it whole (`idf.py -p <the adapter's port> flash`), as ESP-Hosted's own instructions say. The screen keeps it up to
   date from then on.

The connector and button are as read from Waveshare's schematic of the 3.4C and 4C
(https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-XC/ESP32-P4-WIFI6-Touch-LCD-XC-Schematic.pdf), not yet
tried on a board.
