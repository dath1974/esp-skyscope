# esp-skyscope

Live aircraft tracking on a self-contained ESP32-S3 touch screen: the ESP32-S3 fetches traffic information from a
public ADS-B exchange and the map tiles from public tile servers, decodes and draws everything itself.

- **Maps**: a radar scope drawn on the screen (nothing downloaded), streets (Esri's gray maps, dark or light),
  OpenStreetMap, satellite imagery (Esri), the USGS topographic map, and the FAA's VFR sectional, IFR area (low) and
  IFR high enroute charts, or any 256-pixel PNG/JPEG tile server of your own. One **appearance** switch, dark or light:
  maps without a dark version of their own are drawn dark by rules made for each (the IFR charts turn only their paper
  and ink round and keep their colours; the sectional is dimmed; topo and OpenStreetMap are inverted with their hues
  kept). Place names over the streets and the imagery (optional). Range rings and a compass rose over any map.
- **Aircraft**: from adsb.fi or adsb.lol, or a receiver of your own (readsb, tar1090, dump1090-fa: its
  `aircraft.json`), every polling interval, switching to another if one is unavailable. Each
  aircraft drawn as what it is (airliner, wide-body, four-engined, business jet, turboprop, light aircraft,
  helicopter, fast jet, glider, balloon, airship, drone, ground vehicle: from its type, else the class its transponder
  sends, else its description; `make markers` draws them all on a sheet), or as one plane, an arrow or a dot, coloured
  by altitude; labels kept apart from each other; tracks of the last seconds; planes
  optionally glide between reports; a "radar beam" that paints them as it passes. Watched aircraft (by registration,
  callsign or ICAO address) always show, in a colour of their own. Altitude limits, ground traffic on or off, the most planes shown.
- **On the glass**: drag up or down along the right edge to set the brightness; the screen can turn itself off after a
  while without a touch, and at a bedtime every night, on again at a time or a tap (while it is off no planes or
  tiles are fetched). A long press on the map opens a settings menu, which also shows the firmware and installs a new
  release. Tap a plane for its card
  (type, registration, altitude, climb, speed, heading, operator, squawk);
  tap the map to see the screen's address and range in the status line. The range and everything else is set on the
  web page: the screen has no zoom or pan keys, which are too slow on a small embedded display.
- **Set up like any ESPHome device**: Wi-Fi through the screen's own setup network (captive portal) or over USB with
  Improv; everything else on the screen's own web page or built in configuration panel.

## Hardware

| Board | File |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.41 (Rev 2.0) | `boards/waveshare-amoled-241.yaml` |

A board needs an ESP32-S3 with PSRAM (the map takes two frames of the screen, the tile cache 3 MB) and an LVGL-capable
display. Adding one is a new file in `boards/` with its display, touch, fonts and the `skyscope:` block's `dpi`.  Any
new board will require ample testing, noting the goal of this project is a physically small display size.

## Install

You need [ESPHome](https://esphome.io) 2026.9 or newer.

```sh
git clone https://github.com/dath1974/esp-skyscope
cd esp-skyscope
esphome run skyscope.yaml          # over USB the first time
```

Later builds can go over Wi-Fi: `esphome run skyscope.yaml --device <the screen's address>`.

## Updates

Once it is on Wi-Fi the screen updates itself from the project's releases, with no computer. Every six hours it asks
for the newest one (a manifest on GitHub Pages), and its web page shows it under **Firmware**, with **Install** and
**Check now**. Nothing installs by itself. Installing shows **Updating** on the screen, downloads the release into the
half of the flash the screen is not running from, checks it and restarts into it, in a minute or two. The settings
are kept. A download that breaks off or fails its check changes nothing: the screen keeps the firmware it has.

A build of your own offers the releases too, whenever its version differs. To leave them out, remove the `update:`
block and the `skyscope:` block's `update_id` from `skyscope.yaml`.

Releasing (maintainers): set `version` in `skyscope.yaml`, commit, and push an annotated tag `v<version>`
(`git tag -a v0.3.0 -m "What changed, in one line"`). `.github/workflows/release.yml` builds it, makes the GitHub
release and publishes the firmware with its manifest (`tools/release_site.py`). The tag's message is the summary the
page shows.

## First start

1. The screen shows **Set up Wi-Fi**. On a phone or other device, join the Wi-Fi network **Skyscope Setup**. A page
   opens (or go to `http://192.168.4.1`): choose your network and enter its password. (Or use Improv from
   [ESPHome Web](https://web.esphome.io) while the screen is on USB.)
2. The screen joins your network and shows **Where are you?** with its address. Open that address in a browser on the
   same network.
3. Search for a place (an airport, a town) or enter its latitude and longitude, choose a range and a map, and press
   **Save**. The display should draw the map and begin showing traffic within a few seconds.

All settings are on that page, and the screen keeps them across restarts and updates. Once it is set up and running,
tapping the status bar at the top shows the screen's address for a moment.

**Rotation** is set on that page too (the screen restarts to turn). 0° is the panel's own way up and the least
expensive: every other angle turns each frame in software; 90° and 270° cost the most (on the AMOLED 2.41, 0° is
portrait and 90° landscape).

## The web page and API

The screen serves its settings page at `/` and a small JSON API beside it:

| Request | What it does |
|---|---|
| `GET /api/settings` | every setting |
| `POST /api/settings` | changes the settings in a JSON object (only the keys given); a wrong value answers 400 with `{"error": "<key>"}` and changes nothing |
| `GET /api/status` | Wi-Fi, free memory, aircraft, the feed's last error, map progress, rotation, and timing: frames (drawing, waiting for the panel, sending), slow frames by what the worker was doing, a report on the loop and on the worker |
| `GET /api/aircraft` | the planes of the last report |
| `GET /api/firmware` | the version running, the newest release, and an install's progress |
| `POST /api/firmware` | `{"check": true}` asks for the newest release again; `{"install": true}` installs it |
| `POST /api/reboot` | restarts the screen |
| `POST /api/touch` | a touch on the glass for tests: `{"gesture": "tap", "x": 200, "y": 300}`, or `"hold"` (with `"ms"`), or `"drag"` to `"x1"`, `"y1"`; at the screen's own coordinates |
| `GET /api/ui` | what the glass shows, for tests: the status line, a plane's card, the brightness level, the menu (rows, values, and where each key is), and where each plane is |
| `POST /api/debug` | measurement only, never stored: `{"planes": false}` hides the planes while the map and the beam go on |

```sh
curl -X POST http://<screen>/api/settings -H 'Content-Type: application/json' \
  -d '{"lat": 51.4700, "lon": -0.4543, "range_nm": 15, "map": "dark_gray"}'
```

The page and the API are open to anyone on your network, as an ESPHome device's own web server is. The two test endpoints
(`/api/touch`, `/api/ui`) are left out of a build with `test_api: false` in the `skyscope:` block.

## Data sources

Everything the screen shows comes from free public services. Please keep to their terms; the defaults do.

| Source | What | Terms |
|---|---|---|
| [adsb.fi](https://adsb.fi) | aircraft (the default) | free for personal, non-commercial use; at most one request a second |
| [adsb.lol](https://adsb.lol) | aircraft | open data, ODbL; no key yet (it plans to ask feeders for one); limits that change with its load |
| [Esri gray canvas](https://www.esri.com) | dark and light gray streets | Esri's terms of use |
| [OpenStreetMap](https://operations.osmfoundation.org/policies/tiles/) | streets | light use only, with a User-Agent and caching (both done) |
| [Esri World Imagery](https://www.esri.com) | satellite | Esri's terms of use |
| [USGS National Map](https://www.usgs.gov/programs/national-geospatial-program/national-map) | topographic | public domain, United States |
| [FAA aeronautical charts](https://www.faa.gov/air_traffic/flight_info/aeronav) | sectional, IFR | public domain, United States; not for navigation |

The screen asks for planes every 10 seconds by default (3 at the least). Each exchange sets its own limits and may change
them with its load, so the screen keeps to what each one announces: never more often than its `x-ratelimit-limit` header
allows, and after a refusal not before the time it gives (`x-ratelimit-reset` or `Retry-After`), else twice as long each
time it refuses again (2 minutes up to 30; other failures 15 seconds up to 15 minutes). Meanwhile it asks the other
exchange, unless that is switched off, and goes back to the preferred one as soon as its wait is over. The status line
and the page count down to the next try. Tiles are cached in memory for a day and asked again only when the place, range
or map changes.

**Not for navigation.** ADS-B exchanges show what volunteers' receivers hear: planes without ADS-B, or out of a
receiver's range, are missing, and positions may be late.

## Development

- `components/skyscope/`: the firmware, an ESPHome external component. `skyscope.h` explains how its two threads
  share the work. The parts that need no screen (`geo.h`, `settings.h`, `feed.h`, `tiles.h`, `overlay.h`, `plan.h`,
  `pixels.h`, `firmware.h`, `schedule.h`, `menu_rows.h`) are plain C++.
- `make test` runs everything that needs no screen, as CI does on every push and before every release:
  - `make host`: the C++ host tests (`tests/test_host.cpp`, with the address and undefined-behaviour sanitizers; needs
    a C++20 compiler and fetches ArduinoJson's one header). `tests/run.sh reply.json` reads a saved feed reply too.
  - `make python`: the release manifest (`tests/test_release_site.py`) against what ESPHome's update check reads.
  - `make page`: the web page's script (`tests/test_page.mjs`, Node 18 or newer): the airport search's query, the
    firmware section's words, and that the whole script parses.
- With a screen on the network (`tests/device_test.py`, Python's standard library only; every setting it changes is
  put back, and it prints nothing of the screen's place):
  - `make device DEVICE=<address>`: the page, settings (round trip, wrong values refused), status and memory,
    aircraft, the update check against the published release, and touches through `/api/touch` (the menu opened,
    stepped, cancelled and saved, paged, recoloured; Check and Install's second tap; a plane's card; the address;
    a drag on the brightness strip).
  - `make device-full DEVICE=<address>`: also every map drawn whole without a tile error, a restart that keeps the
    settings, and the screen going off and waking at a tap that does nothing else.
  - `make device-mock DEVICE=<address>`: the screen pointed for a while at a mock receiver this machine serves
    (`tests/mock_feed.py`, port 8095, at a test place): the planes it lists kept and placed where they should be, the
    filters and the card, 600 planes at once, and every failure a feed can have (refusals, a pace, server errors,
    answers cut short, broken or too big, too slow, none), each waited out as long as it should be. The screen must be
    able to reach this machine: under WSL, set `networkingMode=mirrored` in `.wslconfig` and allow inbound
    connections to it (see below).
  - `make soak DEVICE=<address> SOAK=60 CHURN=2`: memory sampled for an hour, the map and range changed every two
    minutes, failing if memory keeps falling, if it is lower each time the same view comes round, or if a record
    that grows as the screen runs passes its bound.

### A screen reaching a test server under WSL

The mock receiver runs on the machine that runs the tests, and the screen has to reach it. WSL's default network is a
private one behind Windows, which nothing on the network reaches. In `%UserProfile%\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored
```

then `wsl --shutdown` and open WSL again: it shares the PC's own address. Windows' firewall for WSL still refuses
connections from the network; in a PowerShell run as administrator, allow the mock's port:

```powershell
New-NetFirewallHyperVRule -Name skyscope-mock -DisplayName "Skyscope mock feed" -Direction Inbound `
  -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol TCP -LocalPorts 8095
```

## License

MIT, see LICENSE. A firmware image built from it includes ESPHome's GPL-3.0 runtime, so a compiled firmware you
give to others comes under the GPL-3.0 (see NOTICE).
