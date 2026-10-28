# esp-skyscope

Live aircraft over a map on a small ESP32-S3 touch screen, entirely on its own: the screen fetches the planes from a
public ADS-B exchange and the map tiles from public tile servers, decodes and draws everything itself. No Home Assistant,
no server, no app on a PC.

- **Maps**: a radar scope drawn on the screen (nothing downloaded), street maps (CARTO dark or light, OpenStreetMap),
  satellite imagery (Esri), the USGS topographic map, and the FAA's VFR sectional and IFR low and high enroute charts,
  or any 256-pixel PNG/JPEG tile server of your own. Range rings and a compass rose over any of them.
- **Aircraft**: from adsb.lol or adsb.fi, every few seconds. Plane, arrow or dot markers, coloured by
  altitude; labels kept apart from each other; tracks of the last seconds; planes glide between reports; a radar beam
  that paints them as it passes. Watched aircraft (by registration, callsign or ICAO address) always show, in a
  colour of their own. Altitude limits, ground traffic on or off, the most planes shown.
- **On the glass**: tap a plane for its card (type, registration, altitude, climb, speed, heading, operator, squawk);
  tap the map to see the screen's address and range in the status line. The range and everything else is set on the
  web page: the screen has no zoom or pan keys, which are too slow on a small embedded display.
- **Set up like any ESPHome device**: Wi-Fi through the screen's own setup network (captive portal) or over USB with
  Improv; everything else on the screen's own web page.

## Hardware

| Board | File |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.41 (Rev 2.0) | `boards/waveshare-amoled-241.yaml` |

A board needs an ESP32-S3 with PSRAM (the map takes two frames of the screen, the tile cache 3 MB) and an LVGL-capable
display. Adding one is a new file in `boards/` with its display, touch, fonts and the `skyscope:` block's `dpi`.

## Install

You need [ESPHome](https://esphome.io) 2026.9 or newer.

```sh
git clone https://github.com/<you>/esp-skyscope
cd esp-skyscope
esphome run skyscope.yaml          # over USB the first time
```

Later builds can go over Wi-Fi: `esphome run skyscope.yaml --device <the screen's address>`.

## First start

1. The screen shows **Set up Wi-Fi**. On a phone, join the Wi-Fi network **Skyscope Setup**. A page opens (or go to
   `http://192.168.4.1`): choose your network and enter its password. (Or use Improv from
   [ESPHome Web](https://web.esphome.io) while the screen is on USB.)
2. The screen joins your network and shows **Where are you?** with its address. Open that address in a browser on the
   same network.
3. Search for a place (an airport, a town) or enter its latitude and longitude, choose a range and a map, and press
   **Save**. The planes appear within seconds; the map fills in tile by tile.

Every setting lives on that page and is kept by the screen across restarts and updates.

**Rotation** is set on that page too (the screen restarts to turn). 0° is the panel's own way up and the least
expensive: every other angle turns each frame in software, and 90° and 270° cost the most (on the AMOLED 2.41, 0° is
portrait and 90° landscape).

## The web page and API

The screen serves its settings page at `/` and a small JSON API beside it:

| Request | What it does |
|---|---|
| `GET /api/settings` | every setting |
| `POST /api/settings` | changes the settings in a JSON object (only the keys given); a wrong value answers 400 with `{"error": "<key>"}` and changes nothing |
| `GET /api/status` | Wi-Fi, free memory, aircraft, the feed's last error, map progress, frame timing, rotation |
| `GET /api/aircraft` | the planes of the last report |
| `POST /api/reboot` | restarts the screen |

```sh
curl -X POST http://<screen>/api/settings -H 'Content-Type: application/json' \
  -d '{"lat": 51.4700, "lon": -0.4543, "range_nm": 15, "map": "carto_dark"}'
```

The page and the API are open to anyone on your network, as an ESPHome device's own web server is.

## Data sources

Everything the screen shows comes from free public services. Please keep to their terms; the defaults do.

| Source | What | Terms |
|---|---|---|
| [adsb.lol](https://adsb.lol) | aircraft (the default) | open data, ODbL; no key |
| [adsb.fi](https://adsb.fi) | aircraft | free for personal, non-commercial use; at most one request a second |
| [CARTO basemaps](https://carto.com/basemaps) | streets | free up to 75,000 tiles a month, non-commercial |
| [OpenStreetMap](https://operations.osmfoundation.org/policies/tiles/) | streets | light use only, with a User-Agent and caching (both done) |
| [Esri World Imagery](https://www.esri.com) | satellite | Esri's terms of use |
| [USGS National Map](https://www.usgs.gov/programs/national-geospatial-program/national-map) | topographic | public domain, United States |
| [FAA aeronautical charts](https://www.faa.gov/air_traffic/flight_info/aeronav) | sectional, IFR | public domain, United States; not for navigation |

The screen asks a feed every 10 seconds by default (3 at the least), caches tiles in memory for a day and asks again
only when the place, range or map changes. Each tile server and feed is credited at the bottom of the screen.

**Not for navigation.** ADS-B exchanges show what volunteers' receivers hear: planes without ADS-B, or out of a
receiver's range, are missing, and positions may be late.

## Development

- `components/skyscope/`: the firmware, an ESPHome external component. `skyscope.h` explains how its two threads
  share the work. The parts that need no screen (`geo.h`, `settings.h`, `feed.h`, `tiles.h`, `overlay.h`, `plan.h`)
  are plain C++.
- `tests/run.sh`: builds and runs the host tests of those parts (needs a C++20 compiler and fetches ArduinoJson's one
  header). Pass a saved feed reply to read it too: `tests/run.sh reply.json`.

## License

MIT, see LICENSE. A firmware image built from it includes ESPHome's GPL-3.0 runtime, so a compiled firmware you
give to others comes under the GPL-3.0 (see NOTICE).
