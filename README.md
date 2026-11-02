# esp-skyscope

Live aircraft tracking on a self-contained ESP32 touch screen: the ESP32 (an S3 or a P4) fetches traffic information
from a public ADS-B exchange and the map tiles from public tile servers, decodes and draws everything itself.

- **Maps**: a radar scope drawn on the screen (nothing downloaded), streets (Esri's gray maps, dark or light),
  OpenStreetMap, satellite imagery (Esri), the USGS topographic map, and the FAA's VFR sectional, IFR area (low) and
  IFR high enroute charts, or any 256-pixel PNG/JPEG tile server of your own. One **appearance** switch, dark or light:
  maps without a dark version of their own are drawn dark by rules made for each (the IFR charts turn only their paper
  and ink round and keep their colours; the sectional is dimmed; topo and OpenStreetMap are inverted with their hues
  kept). Place names over the streets and the imagery (optional). Range rings and a compass rose over any map.
- **Airspace** (optional, United States): the FAA's Class B, C and D airspace outlines and runways, with dashed
  centerlines 10 NM out from the long runways of towered airports, over any map, the way a controller's scope draws
  them: in the phosphor on the radar, in the charts' blue and magenta over a map. On the radar the scope itself (its
  glow, bearing lines, range rings and rose, and the beam) can be switched off, leaving the planes and the airspace
  alone on the dark ground in the chosen phosphor (green, amber, blue or white), like a controller's bare map. Asked
  from the FAA once for each place and range (runways up to 80 NM). These are built from the FAA's public data, not
  the maps real ATC scopes use: the FAA does not publish those.
- **Aircraft**: from adsb.fi or adsb.lol, a receiver of your own (readsb, tar1090, dump1090-fa: its
  `aircraft.json`), or on the P4 board the screen's own radio (an RTL-SDR stick on its USB port, below), every polling
  interval, switching to another if one is unavailable. Each
  aircraft drawn as what it is (airliner, wide-body, four-engined, business jet, turboprop, light aircraft,
  helicopter, fast jet, glider, balloon, airship, drone, ground vehicle: from its type, else the class its transponder
  sends, else its description; `make test-markers` draws them all on a sheet), or as one plane, an arrow or a dot,
  coloured by altitude; labels kept apart from each other; tracks of the last seconds; planes
  optionally glide between reports; a "radar beam" that paints them as it passes. Watched aircraft (by registration,
  callsign or ICAO address) always show, in a colour of their own. Altitude limits, ground traffic on or off, the most planes shown.
- **On the glass**: drag up or down along the right edge to set the brightness; the screen can turn itself off after a
  while without a touch, and at a bedtime every night, on again at a time or a tap (while it is off no planes or
  tiles are fetched). A long press on the map opens a settings menu with every setting the web page has (see
  [The menu](#the-menu)): its arrows step a value, and a tap on a value's field types it on a keyboard on the glass. It
  also finds a place, changes the Wi-Fi network, and shows the firmware and installs a new release. Tap a plane for
  its card (type, registration, altitude, climb, speed, heading, operator, squawk); tap the map to see the screen's
  address and range in the status line. Pinch to zoom and drag to pan (see [Zoom and pan](#zoom-and-pan)): a preview,
  kept with **Save**. Swipe along the bottom edge to change the map (see
  [Changing the map with a swipe](#changing-the-map-with-a-swipe)).
- **Aviation weather** (see [Aviation weather](#aviation-weather)): a swipe up from the bottom edge shows a page of the
  nearest station's METAR (decoded too), its TAF and GFS MOS, and the SIGMETs, G-AIRMETs and pilot reports round the
  place, with its flight category in colour. The NEXRAD radar can go over any map of tiles, as the latest scan or a loop
  of the last hour.
- **Set up like any ESPHome device**: Wi-Fi through the screen's own setup network (captive portal) or over USB with
  Improv, or on the glass itself; everything else on the screen's own web page or its menu.

## Hardware

| Board | File |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.41 (Rev 2.0) | `boards/waveshare-amoled-241.yaml` (the default) |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4C (round, 720 × 720; not yet tried on a real one) | `boards/waveshare-p4-lcd-4c.yaml` |

`skyscope.yaml` builds the board its `board` substitution names: `esphome -s board waveshare-p4-lcd-4c run
skyscope.yaml`, or `make firmware BOARD=waveshare-p4-lcd-4c`. Each board builds in a folder of its own
(`.esphome/build/skyscope-<board>`), and a release publishes each board's firmware with a manifest of its own, which
only screens of that board ask for (below, Updates).

The P4 board takes its Wi-Fi from the ESP32-C6 beside it, which runs Espressif's ESP-Hosted firmware. The P4's
firmware carries the C6's (`boards/esp-hosted/`, checked by its SHA-256): at start, a C6 running any other version is
updated over the link between the two chips, with **Updating the Wi-Fi chip** on the glass, so the C6 needs only some
ESP-Hosted to begin with, as it has from Waveshare (`boards/esp-hosted/README.md` says what to do if not). Its file is
for production P4 silicon (v3), which the 4C ships with; a board with an early chip (its boot log says `chip revision:
v1.x`) would need `engineering_sample: true` in the file and a build of its own. The
glass is round, and the screen keeps everything inside the circle: the pages and the keyboard in the largest square it
holds, the status, the credit and a plane's card moved in from the edge as far as their width needs, and no plane or
label in the corners beyond it (`skyscope:` `round: true`, `glass.h`).

A board needs an ESP32-S3 or ESP32-P4 with PSRAM (the map takes up to five frames of the screen, the tile cache 3 MB)
and an LVGL-capable display. Adding one is a new file in `boards/` with its display, touch, fonts and the `skyscope:`
block's `dpi`, and the board in the build matrix of `.github/workflows/tests.yml` (a test checks it is there); a release
builds and publishes every file in `boards/` by itself. Two boards may have the same chip. Any new board will require
ample testing, noting the goal of this project is a physically small display size.

### The screen's own radio (ESP32-P4)

On the P4 board an RTL-SDR stick on its USB-A port makes the screen a receiver of its own: the FlightAware Pro Stick
Plus, or any stick with an RTL2832U and an R820T or R820T2 tuner. Nothing else is needed: the screen tunes it to
1090 MHz, reads two million samples a second from it, finds the Mode S replies in them and their checksums, and works
out each plane's identity, altitude, speed, track, squawk and place from them, as dump1090 does on a computer. It is
**Radio** among the sources, on the page and in the menu (only on a screen built with one), and with falling back
allowed the exchanges stand in while no stick is plugged in. The page's status shows how it is doing: messages a second,
planes heard and placed, and samples dropped if the demodulator ever falls behind.

The board file's `skyscope:` block turns it on and sets the tuner:

```yaml
  radio:
    gain: 49.6   # dB, 0 to 49.6, or "auto" for the tuner's own; lower it for a stick swamped by planes close by
    ppm: 0       # the stick's crystal error, if known
```

The radio has not yet run on a board: the stick's driver (`rtlsdr.cpp`) follows the registers the osmocom rtl-sdr
project documented, and the decoding (`modes.h`, `adsb.h`) is tested on a PC against samples built from published
example messages. Surface positions are worked out from the screen's place, so they are right only with the place set
to where the screen is; airborne ones need no place. Planes heard only by their Mode S replies, without ADS-B, have no
position and are not shown.

## Install

You need [ESPHome](https://esphome.io) 2026.9 or newer; releases are built with the version in the `Makefile`
(`ESPHOME_VERSION`), which `make firmware-esphome` installs.

```sh
git clone https://github.com/dath1974/esp-skyscope
cd esp-skyscope
esphome run skyscope.yaml          # over USB the first time; add -s board <name> for another board
```

Later builds can go over Wi-Fi: `esphome run skyscope.yaml --device <the screen's address>`, or
`make firmware-flash DEVICE=<the screen's address>` (with `BOARD=<name>` for another board), which builds with the
Makefile's ESPHome version first (`DEVICE` can be a serial port too).

## Updates

Once it is on Wi-Fi the screen updates itself from the project's releases, with no computer. Every six hours it asks for
the newest one (its board's manifest on GitHub Pages, `<board>/manifest.json`, so it is only ever offered firmware for
its own board), and its web page shows it under **Firmware**, with **Install** and **Check now**. Nothing installs by
itself. Installing shows **Updating** on the screen, downloads the release into the half of the flash the screen is not
running from, checks it and restarts into it, in a minute or two. The settings are kept. A download that breaks off or
fails its check changes nothing: the screen keeps the firmware it has.

A build of your own offers the releases too, whenever its version differs. To leave them out, remove the `update:`
block and the `skyscope:` block's `update_id` from `skyscope.yaml`.

Releasing (maintainers): `make release-version VERSION=0.4.0` sets `version` in `skyscope.yaml` and commits it;
`make release-tag MSG="What changed, in one line"` checks that main is committed and current, builds the firmware
for every board, runs the tests and makes the annotated tag `v<version>`; `make release` asks, then pushes main and the
tag.
`.github/workflows/release.yml` builds it for every board, makes the GitHub release and publishes each board's firmware
with its manifest (`tools/release_site.py`). The tag's message is the summary the page shows. The AMOLED 2.41's manifest
is published at the top as well (`manifest.json`), where screens on 0.5.0 and before ask; once updated they ask in their
board's folder.

## First start

1. The screen shows **Set up Wi-Fi**. On a phone or other device, join the Wi-Fi network **Skyscope Setup**. A page
   opens (or go to `http://192.168.4.1`): choose your network and enter its password. (Or use Improv from
   [ESPHome Web](https://web.esphome.io) while the screen is on USB, or hold a finger on the screen and choose the
   network in its menu, under **Wi-Fi**.)
2. The screen joins your network and shows **Where are you?** with its address. Open that address in a browser on the
   same network, or hold a finger on the screen for its menu.
3. Search for a place (an airport, a town) or enter its latitude and longitude, choose a range and a map, and press
   **Save**. The display should draw the map and begin showing traffic within a few seconds.

All settings are on that page and in the menu, and the screen keeps them across restarts and updates. Once it is set up and running,
tapping the status bar at the top shows the screen's address for a moment.

**Rotation** is set there too (the screen restarts to turn). 0° is the panel's own way up and the least
expensive: every other angle turns each frame in software; 90° and 270° cost the most (on the AMOLED 2.41, 0° is
portrait and 90° landscape).

## The menu

A long press on the map (or on the setup screen) opens the menu. Its rows are grouped as on the web page (Where, Map,
Aircraft, Source, Screen) and hide where they do not apply, as the page's do: the phosphor on the radar only, the
bedtime's times with bedtime on. A value between two arrows steps through common choices; a value in a field opens
the keyboard at a tap, and a few (the range, the brightness) have both. Typed values are checked as the page's are;
one the screen cannot take is refused, with what it may be shown under the text. Changes show in the accent and wait
for **Save**; **Cancel** drops them.

- **The keyboard** has letters, numbers and a page of symbols (every printable ASCII character), **Aa** for capitals,
  arrows to move the cursor and **Del**. Numbers and times get a pad of digits.
- **Place** finds a place as the page does, but from the screen itself: an airport code (KJFK, JFK) in the FAA's
  airport data, an airport's name there too, then anything in OpenStreetMap's Nominatim, at most once a second and
  only when **Search** is pressed. Pick one of what it found; an airport is named by its code. Latitude and longitude
  can be typed instead.
- **Watched** takes the aircraft with a comma between: `N12345 red, BAW123 blue` (red where no colour is given).
- **Wi-Fi** shows the network the screen is on; a tap lists the networks it knows, first choice first, then the others
  in reach (the screen scans for them, or uses ESPHome's own scans while it has no network), strongest first, and
  **Other network** for a hidden one by its name. A known network has keys to move it up or down and **Forget** (a
  second tap forgets it; the last one stays). A tap on a known network joins it with the password it keeps; on another,
  after its password, **Join**. Joining keeps the menu's changes, makes the network the first choice and restarts the
  screen to join it (ESPHome changes networks only by losing the one it is on). The screen knows up to eight networks,
  and at every start (or when it loses its network) joins the first of them in reach, so it can be moved between places.
  A network given to ESPHome's setup page or Improv joins them as the first choice. With none in reach it opens
  **Skyscope Setup** after a minute, and a network can be chosen there or in the menu.

## Zoom and pan

Two fingers on the map zoom it (spread or pinched) and pan it (moved together), as on a phone; one finger dragged
across the map pans it too (a tap that moves less than 3 mm is still a tap, and a long press for the menu is a finger
held still). The touch panel often takes two fingers close together for one, so a pan works either way. As they land,
the screen freezes what it shows, planes and labels included, and while they move it stretches and shifts that picture
to follow them; nothing is downloaded. Three seconds after the fingers lift, the screen plans that view once, and asks
for its map tiles and planes as for any other place and range (tiles it already holds come from its cache). More
gestures before then cost nothing more. The frozen picture stays until the new map is whole, and the live planes come
back with it.

A bar along the bottom edge shows the range and a 60-second countdown, with **Cancel** and **Save**. **Save** keeps
the view as the place and range (the place keeps its name if the middle hardly moved); **Cancel**, or the countdown
running out, goes back to the view saved before. Nothing is stored until **Save**. Opening the menu or the screen
going to sleep drops the preview too.

The range is any whole number from 1 to 250 NM, by a pinch or typed in the menu's Range field. A drawn map's scale
moves in whole tile levels (each twice the last), so after the three seconds a pinched map may settle a little larger
or smaller than the fingers left it; the rings and the planes asked for are at the exact range. The radar has no tiles
and shows any range as it is.

## Changing the map with a swipe

A swipe left or right along the bottom edge of the map (its lowest 6 mm) changes the map: to the left the next one,
to the right the one before, in the menu's order (Custom only where a tile server is set). Each swipe shows the map's
name at once, large in the middle, with a row of dots above the edge, one a map and the one chosen lit. Nothing is
downloaded until the swipes have stopped for two seconds, so going through several costs only the last one; the map
is then drawn and saved, as from the menu. A swipe back undoes it. A drag that starts in that strip never pans, and a
tap there still picks a plane.

## Aviation weather

A swipe up from the bottom edge of the map (or **Show the weather** in the menu's Weather group) opens the weather
page, for a station: the one set in the menu or on the web page, or with none set the nearest to the place that
reports. Its title is the station and its flight category, in the charts' colours (VFR green, MVFR blue, IFR red,
LIFR magenta). Its tabs:

- **METAR**: the latest, as sent and in words (wind, visibility, sky, ceiling, temperature and dew point, altimeter).
- **TAF**: a line for each change (from, temporarily, becoming, with a probability).
- **MOS**: the GFS MOS, three hours a line: temperature and dew point, wind, sky, the flight category its ceiling and
  visibility make, and the chance of precipitation. Stations in the United States only.
- **Area**: the SIGMETs (convective ones too) and G-AIRMETs within the range (at least 50 NM) of the place, and the
  pilot reports there in the last two hours. The FAA's area forecasts are now issued only for Alaska, Hawaii and the
  Gulf, and the G-AIRMETs take their place elsewhere.

The page asks for the weather only while it is shown: when it opens with nothing from the last ten minutes, at
**Refresh**, and every ten minutes while it stays open. Not for flight planning.

**Radar** (in the menu's Weather group, or on the web page) draws the NWS's NEXRAD composite over any map of tiles,
part see-through, with the time of its scan in the credits; the United States only. The screen asks for the latest
scan every five minutes, and draws it over a copy of the map kept without it, so the map is never drawn again for it.
**Radar loop** plays the last hour, a scan every 0.7 seconds and the latest for 2.5: each scan's tiles are fetched once
(they are asked for by the scan's time, and the tile cache keeps them), and what each scan changed is kept (in PSRAM,
1.2 MB at most), so after the first time round the loop asks for nothing but each new scan.

## The web page and API

The screen serves its settings page at `/` and a small JSON API beside it:

| Request | What it does |
|---|---|
| `GET /api/settings` | every setting |
| `POST /api/settings` | changes the settings in a JSON object (only the keys given); a wrong value answers 400 with `{"error": "<key>"}` and changes nothing |
| `GET /api/status` | Wi-Fi, free memory, aircraft, the feed's last error, map progress, the tile cache (`tile_cache`: tiles and KB held, tiles fetched and their KB, tiles from the cache, maps shown whole again), the weather radar's scan shown (`radar_scan`), the airspace shown (areas, runways, bytes, last error), the screen's own radio (`radio`: what it is doing, messages a second and in all, one-bit errors mended, checksums failed, planes heard and placed, positions thrown away, samples dropped, its gain and noise floor), rotation, and timing: frames (drawing, waiting for the panel, sending), slow frames by what the worker was doing, a report on the loop and on the worker |
| `GET /api/aircraft` | the planes of the last report |
| `GET /api/firmware` | the version running, the newest release, an install's progress, and the manifest it asks (`source`) |
| `POST /api/firmware` | `{"check": true}` asks for the newest release again; `{"install": true}` installs it |
| `GET /api/networks` | the Wi-Fi networks the screen knows, first choice first: their names only, never a password |
| `POST /api/networks` | with the test API only: `{"move": [i, step]}`, `{"forget": i}` or `{"join": {"name", "password"}}` (no restart) |
| `POST /api/reboot` | restarts the screen |
| `POST /api/touch` | a touch on the glass for tests: `{"gesture": "tap", "x": 200, "y": 300}`, or `"hold"` (with `"ms"`), or `"drag"` to `"x1"`, `"y1"`; at the screen's own coordinates |
| `GET /api/ui` | what the glass shows, for tests: the status line, a plane's card, the brightness level, the menu (rows, values, and where each key is), its keyboard (text, hint and keys) and the list over it (places or networks), the map a swipe went to, the weather page (title, line, tab, lines, keys), and where each plane is |
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
| [FAA ADDS](https://adds-faa.opendata.arcgis.com) | airspace and runways (optional) | public domain, United States; not for navigation |
| [Aviation Weather Center](https://aviationweather.gov/data/api/) | METARs, TAFs, SIGMETs, G-AIRMETs, pilot reports (the weather page) | US government; at most 100 requests a minute, with a User-Agent; the page asks a few every ten minutes while it is shown |
| [Iowa Environmental Mesonet](https://mesonet.agron.iastate.edu) | the GFS MOS, and the NEXRAD radar composite (optional) | Iowa State University; free, credited on the glass; each radar tile fetched once |
| [FAA airports](https://adds-faa.opendata.arcgis.com) | the menu's place search: airports by code or name | public domain, United States |
| [Nominatim](https://operations.osmfoundation.org/policies/nominatim/) | the menu's place search: anything else | at most one request a second, with a User-Agent, none as you type, OpenStreetMap credited (all done) |

The screen asks for planes every 10 seconds by default (3 at the least). Each exchange sets its own limits and may
change them with its load, so the screen keeps to what each one announces: never more often than its `x-ratelimit-limit`
header allows, and after a refusal not before the time it gives (`x-ratelimit-reset` or `Retry-After`), else twice as
long each time it refuses again (2 minutes up to 30; other failures 15 seconds up to 15 minutes). Meanwhile it asks the
other exchange, unless that is switched off, and goes back to the preferred one as soon as its wait is over. The status
line and the page count down to the next try. Tiles are cached in memory for a day (3 MB, as they came from their
server, the least recently drawn going first), and a tile is asked for only when a map needs one it does not hold. Each
map is drawn from its tiles, one at a time, so a map from the cache still fills in over a second or two; the last map
drawn whole is also kept as it was drawn, so going back to it (a swipe back, a preview cancelled) shows it whole at
once. `/api/status` counts both (`tile_cache`). The airspace is asked once for each place and range while it is switched
on; a request that fails waits 30 seconds, then twice as long each time, up to 15 minutes, since the FAA's server is
shared by everyone who uses it.

**Not for navigation.** ADS-B exchanges and the screen's own radio show what receivers hear: planes without ADS-B, or out of a
receiver's range, are missing, and positions may be late.

## Development

- `components/skyscope/`: the firmware, an ESPHome external component. `skyscope.h` explains how its two threads
  share the work. The parts that need no screen (`geo.h`, `glass.h`, `settings.h`, `feed.h`, `tiles.h`, `overlay.h`,
  `plan.h`, `pixels.h`, `firmware.h`, `schedule.h`, `menu_rows.h`, `keyboard.h`, `places.h`, `pinch.h`, `swipe.h`,
  `wifi_list.h`, `weather.h`, `radar.h`, `modes.h`, `adsb.h`) are plain C++.
- `make` builds the firmware (for `BOARD`, the AMOLED 2.41 unless told another) and runs `make test` (and
  `make test-esp32` too, given `DEVICE=<address>`). `make firmware-all` builds it for every board, as a release does.
  `make help` lists every target; `make clean` removes what the build and tests leave behind, keeping the toolchain
  ESPHome downloaded.
- `make test` runs everything that needs no screen, as CI does on every push and before every release. A push also
  builds the firmware for every board and draws the aircraft sheet (`make test-markers`), kept with the run as
  `markers`. `make test` covers:
  - `make test-host`: the C++ host tests (`tests/test_host.cpp`, with the address and undefined-behaviour sanitizers;
    needs a C++20 compiler and fetches ArduinoJson's one header). `tests/run.sh reply.json` reads a saved feed reply too.
  - `make test-python`: the release manifest (`tests/test_release_site.py`) against what ESPHome's update check reads,
    and the mock receiver and update server (`tests/test_mock_feed.py`) that the tests on a real screen use.
  - `make test-page`: the web page's script (`tests/test_page.mjs`, Node 18 or newer): the airport search's query, the
    firmware section's words, and that the whole script parses.
- With a screen on the network (`tests/device_test.py`, Python's standard library only; every setting it changes is
  put back, and it prints nothing of the screen's place):
  - `make test-esp32 DEVICE=<address>`: the page, settings (round trip, wrong values refused), status and memory,
    aircraft, the update check against the published release, and touches through `/api/touch` (the menu opened,
    stepped, cancelled and saved, paged, recoloured; a range typed on its keyboard; the Wi-Fi list; Check and
    Install's second tap; the known Wi-Fi networks (a made-up one moved and forgotten, never joined); a plane's card;
    the address; a swipe between maps and back; the weather page and its tabs; the weather radar and its loop;
    a drag on the brightness strip).
  - `make test-esp32-full DEVICE=<address>`: also every map drawn whole without a tile error, a restart that keeps the
    settings, and the screen going off and waking at a tap that does nothing else.
  - `make test-esp32-mock DEVICE=<address>`: the screen pointed for a while at a mock receiver and update server this
    machine serves (`tests/mock_feed.py`, port 8095, at a test place). The receiver: the planes it lists kept and
    placed where they should be, the filters, the range and the card; no planes, a moving one, 600, and 4500 in 600 KB;
    entries no receiver should send (wrong types, duplicates, UTF-8 past a field's length); every failure a feed can
    have (refusals, a pace, server errors, answers cut short, broken, too big or not JSON, too slow, none), each
    waited out as long as it should be; answers chunked or redirected; an address that cannot be found, then
    corrected; falling back to an exchange and back. The update server: a release offered, its failures said, and
    installs that must fail (no image, cut short, a wrong digest, not an image) leaving the firmware as it is. With
    `INSTALL=.esphome/build/skyscope-<board>/build/firmware.ota.bin`, that build installed from the mock. The
    screen must be able to reach this machine: under WSL, set `networkingMode=mirrored` in `.wslconfig` and allow
    inbound connections to it (see below).
  - `make test-esp32-soak DEVICE=<address> SOAK=60 CHURN=2`: memory sampled for an hour, the map and range changed every
    two minutes, failing if memory keeps falling, if it is lower each time the same view comes round, or if a record
    that grows as the screen runs passes its bound.

### A screen reaching a test server under WSL

The mock runs on the machine that runs the tests, and the screen has to reach it. WSL's default network is a
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
