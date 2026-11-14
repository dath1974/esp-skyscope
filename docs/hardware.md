# Hardware

The boards the screen runs on, and what is particular to each. Back to the [README](../README.md).

| Board | File |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.41 (Rev 2.0) | `boards/waveshare-amoled-241.yaml` (the default) |
| Waveshare ESP32-S3-Touch-LCD-2.8B (Rev 1.1, 480 × 640) | `boards/waveshare-lcd-28b.yaml` |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4C (round, 720 × 720) | `boards/waveshare-p4-lcd-4c.yaml` |

`skyscope.yaml` builds the board its `board` substitution names: `esphome -s board waveshare-p4-lcd-4c run
skyscope.yaml`, or `make firmware BOARD=waveshare-p4-lcd-4c`. A release publishes each board's firmware separately,
and a screen is only ever offered its own board's.

## Waveshare LCD 2.8B

The panel takes its picture over an RGB bus from a frame the ESP32-S3 keeps in PSRAM, so it uses more memory than the
AMOLED. It runs at a slower pixel clock than Waveshare's demo: faster, the panel's constant reading takes so much of the
PSRAM that the picture jumps. The screen drives it with a driver of its own (`components/rgb_panel`), since ESPHome's
sets a pixel format the board's wiring does not match. The board is best standing (0°), since every other angle turns
each frame in software.

## Waveshare P4 4C

The P4 gets its Wi-Fi from the ESP32-C6 beside it, which runs Espressif's ESP-Hosted firmware. The P4's firmware
carries the C6's: at start, a C6 running any other version is updated over the link between the chips, with
**Updating the Wi-Fi chip** on the glass. The C6 only needs some ESP-Hosted to begin with, as it has from Waveshare
(`boards/esp-hosted/README.md` says what to do if not). The board file is for production P4 silicon (v3), which the 4C
ships with. A board with an early chip (its boot log says `chip revision: v1.x`) needs `engineering_sample: true` and
a build of its own.

With 32 MB of PSRAM the P4 keeps up to 12 MB of map tiles in memory, where the other boards keep 3 MB, so a map
swiped back to or zoomed out again is drawn without the network or the card.

The glass is round, and the screen keeps everything inside the circle: pages and the keyboard in the largest square it
holds, the status, credit and plane cards moved in from the edge, and no plane or label in the corners
(`round: true` in the `skyscope:` block).

### The speaker

The board's speaker port (a 2-pin MX1.25 connector beside the USB-A port) takes a small speaker, 8 Ω and a watt or
two. Behind it are an ES8311 codec, on the touch screen's I2C bus at 0x18, and an NS4150B amplifier that GPIO53 turns
on; I2S runs on GPIO9 (data), GPIO10 (word select), GPIO12 (bit clock) and GPIO13 (master clock), as in Waveshare's
board support. The board file runs the codec at 48 kHz, 16-bit mono, behind ESPHome's mixer, which takes three
sources: the screen's own sounds (`speaker:` in the `skyscope:` block), and Home Assistant's announcements and media
(the `Speaker` media player, `media_player:` in the same block). What the screen does with it: [using.md](using.md#sound).

The media player's `volume_min` and `volume_max` (0.5 and 0.8, the codec's −32 dB to +6 dB) are the screen's own
volume range (`sound.h`), so a percentage is as loud whichever side sets it. A board without a speaker leaves both keys
out, and its menu and web page show no sound settings.

### The screen's own radio (ESP32-P4)

An RTL-SDR stick in the P4 board's USB-A port makes the screen a receiver of its own: the FlightAware Pro Stick Plus,
or any stick with an RTL2832U and an R820T or R820T2 tuner. The screen tunes it to 1090 MHz and decodes the planes
itself, as dump1090 does on a computer. It appears among the sources as **USB radio** on the glass and **This
screen's radio (USB stick)** on the web page (only on a screen built with one), and the web page's status shows its
messages a second and planes heard.

**No network needed.** When a screen with a radio has been off a network for 20 seconds, it shows the planes the radio
hears, whatever source is set, and the status line ends with **no Wi-Fi**. With a map card the chosen map stays, drawn
from the card; without one it shows the radar. Nothing is fetched meanwhile, and the settings stay as they were, so
everything comes back once the screen is on a network again. The place cannot be looked up offline: type its latitude
and longitude, or set it once while on a network. Without a network the clock is not set, so bedtime does not apply.

The board file's `skyscope:` block turns it on and sets the tuner:

```yaml
  radio:
    gain: 49.6   # dB, 0 to 49.6, or "auto" for the tuner's own; lower it for a stick swamped by planes close by
    ppm: 0       # the stick's crystal error, if known
```

The radio has not yet run on a real board. The decoding is tested on a PC against samples built from published example
messages. Planes on the ground are placed from the screen's own place, so they are right only with the place set to
where the screen is. Planes without ADS-B have no position and are not shown.

## The map card's slot

Every board has a microSD slot, set by `card:` in the board file's `skyscope:` block. What the card does is in
[docs/using.md](using.md#the-map-card).

- **P4**: slot 0 of its SD/MMC host, four data lines, powered by LDO channel 4 (slot 1 is the Wi-Fi chip's).
- **AMOLED 2.41**: one data line on GPIO 4, 5 and 6.
- **LCD 2.8B**: one data line on GPIO 2, 1 and 42. The first two are shared with the lines that set up the panel, which
  is done once at start before the card is looked for; the card's DAT3 is held high by the board's I/O expander.

A card that answers but garbles is asked again more gently, at 20 MHz and then on one data line, before the screen
gives up on it; the web page's **Map card** says why a card was not taken.

## Adding a board

A board needs an ESP32-S3 or ESP32-P4 with PSRAM (the map takes up to five frames of the screen, and the tile cache
3 MB or more) and a display LVGL can drive. Adding one is a new file in `boards/` with its display, touch, fonts and the
`skyscope:` block's `dpi`, and the board in the build matrix of `.github/workflows/tests.yml` (a test checks it is
there). A release builds every file in `boards/` by itself. A new board needs plenty of testing, and the project's aim
is a physically small screen.
