# AGENTS.md

Guidance for coding agents working in esp-skyscope: an ESP32-S3 touch screen that fetches ADS-B traffic and map tiles
from public services and draws them itself. The firmware is an ESPHome external component. `README.md` and `docs/`
are the user guide; read them for what the screen does and how it is set up.

## Layout

- `skyscope.yaml`: the device config (version, update server, packages); `home-assistant.yaml`, its package of
  ESPHome's API and the entities Home Assistant sees. `boards/`: one file per board, chosen by the `board`
  substitution (`make firmware BOARD=<name>`, `make firmware-all`); each builds in `.esphome/build/skyscope-<board>`,
  and a release publishes each board's firmware with its own manifest (`<board>/manifest.json`).
- `components/rgb_panel/`: the LCD 2.8B's display driver (an RGB panel fed from a frame in PSRAM), the project's own.
- `components/skyscope/`: the firmware. `skyscope.h` explains the two threads; start there.
  - Plain C++ with no ESPHome or ESP-IDF includes, so the host tests can build them: `geo.h`, `glass.h`,
    `settings.h`, `feed.h`, `tiles.h`, `overlay.h`, `plan.h`, `pixels.h`, `firmware.h`, `schedule.h`, `menu_rows.h`,
    `keyboard.h`, `places.h`, `pinch.h`, `swipe.h`, `wifi_list.h`, `weather.h`, `radar.h`, `modes.h`, `adsb.h`,
    `wake.h`, `offline.h`, `card.h`, `psram.h`, `follow.h`, `pngrow.h`, `sightings.h`, `today.h`, `emblem.h`,
    `ticks.h`, `tour.h`, `locate.h`, `sound.h`, `numbers.h` (namespace `sky`).
    Keep them that way; logic that can be tested on a PC belongs here.
  - On the device only: `skyscope.cpp`, `worker.cpp`, `view.cpp`, `menu.cpp`, `editor.cpp`, `preview.cpp`, `swipe.cpp`,
    `networks.cpp`, `wxpage.cpp`, `todaypage.cpp`, `splash.cpp`, `tour.cpp`, `web.cpp`, `http.cpp`, `probe.cpp`,
    `card.cpp` (the map card), `sound.cpp` (the P4's speaker), and the P4's radio, `rtlsdr.cpp` and `radio.cpp` (namespace `esphome::skyscope`).
  - `page.h`: the whole settings page (HTML and script) as one raw string. No external resources.
  - `__init__.py`: the component's YAML schema and codegen.
- `tests/`: `test_host.cpp` (C++ on a PC), `test_page.mjs` (the page's script), `test_release_site.py`,
  `test_web_stack.py` (no floats printed on the web server's task), `device_test.py`, `mock_feed.py` and `mock_services.py` (against a real screen).
- `tools/`: `release_site.py` (the release manifest), `markers.cpp` (the aircraft sheet), `emblem.cpp` (the emblem's
  sheet and `docs/emblem.svg`), `screenshots.py` (`docs/screens.md`'s pictures, from a real screen), `wake_table.py`
  (`wake.h`, each type's wake category from the FAA's list).

## Build and test

- `make test`: everything that needs no screen (host C++ with ASan/UBSan, Python, Node). This is what CI runs; run it
  after any change. Needs a C++20 compiler, Python 3, Node 18+.
- `make firmware`: compiles with the ESPHome in `ESPHOME_VERSION` (Makefile) and refuses another version;
  `make firmware-esphome` installs it. Slow; run it when device code, `__init__.py` or YAML changes.
- `make test-markers`: renders `tests/build/markers.png` after changing aircraft shapes (`shapes.h`, `sprites.h`).
- `make emblem`: after changing `emblem.h`, renders `tests/build/emblem.png`, writes `docs/emblem.svg`, and the page's
  copy to paste into `page.h`.
- `make test-esp32*` need `DEVICE=<address>` and a real screen. Do not run them unless the user gives an address.
  Device tests must put back every setting they change and never print the screen's location.

## Things that bite

- **Threads.** The worker task (core 0) fetches the feed and tiles; on the P4 the radio's three tasks (radio.h) run
  on the same core above it; ESPHome's loop owns LVGL and the glass; the web
  server's task only reads a copy of the settings or hands a new one to the loop. Shared state lives in `Shared` under
  `lock_`, the base frame under `frame_lock_`. Never touch LVGL off the loop.
- **The web server's stack.** Its task has 4.5 KB (ESPHome's), and a float printed with printf takes some 2 KB of it:
  the P4 restarted at "Download the log". Write numbers there with `numbers.h`, keep `handleRequest` to the password
  and the routing (the answers in their own `noinline` functions), and `tests/test_web_stack.py` holds web.cpp to it.
- **ArduinoJson and char arrays.** `o["name"] = some_struct.name` with a `char[N]` stores only the pointer (it is taken
  for a string literal); cast to `const char *` to have it copied. `/api/stats` once sent freed memory that way.
- **Memory.** Internal RAM is scarce and has run out before (black screens, broken replies). Large buffers and TLS go
  in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`). Watch for leaks: the soak test exists
  because of one.
- **Public services.** adsb.fi, adsb.lol (its planes and its routes), Esri, OSM, USGS, FAA, the Aviation Weather
  Center, the Iowa Environmental Mesonet and freeipapi.com have terms; keep to them. Respect rate-limit headers and
  backoff in `feed.h`, keep the User-Agent and tile caching, and keep attribution as each source requires.
- **Test endpoints** (`/api/touch`, `/api/ui`, `/api/screenshot`, `/api/test`) are compiled out with
  `test_api: false`; keep them behind that flag.
- **Tests against the mock.** The device tests (all but `--public`) send the screen to `tests/mock_feed.py`, which
  stands in for every public service (`tests/mock_services.py`), so a run is the same every time and on every board.
  A new service the screen asks needs its stand-in there, and its answer read in `test_mock_answers` (test_host.cpp);
  a run fails on any request the mock did not know. `--public` alone asks the real services.
- **Round glass.** On a board with `round: true` the corners are not there. Place anything new on the glass through
  `glass.h` and `ui.h` (`ui::pad_page` for a page, `ui::hug_edge` for a box along the top or bottom edge), never by
  the display's width and height alone.
- **Pixels are dear.** Drawing the splash on the P4 came to about a tenth of a microsecond for every pixel read or
  written, whatever was done with it: half a million pixels a frame was six frames a second. Anything that moves
  draws only what changed and tells LVGL only those boxes (`emblem.h`, `beam.h`), fewer than 32 of them: past that
  LVGL draws the whole glass.
- **The clock turns over.** `millis()` is 32 bits: a time more than 24.8 days gone reads as yet to come, and a screen
  on a wall is up that long. Taps were once ignored and routes never asked for because of it. A time that can wait
  unasked for weeks (a deadline of a layer that is off, the last touch) is kept behind the clock on every pass, or let
  go once it has come (`ticks.h`).
- **One order for the settings.** The menu's groups (`menu_rows.h`) and the web page's parts (`page.h`) have the same
  names in the same order, and `tests/test_page.mjs` holds them to it: a setting moved in one moves in the other.
- **Erasing.** A factory reset (`factory_reset_`, skyscope.cpp) erases all of ESPHome's preferences: whatever new
  thing the screen keeps in flash goes with them, and nothing may be stored on its way down (`resetting_`). A device
  test never answers its question yes: the screen would forget its network.
- **Docs.** `README.md` is the front page: what the screen does in a line each, install, first start, updates and the
  data sources. The detail lives in `docs/`: `using.md` (the glass, the menu, every feature), `hardware.md` (boards,
  the radio, the card's slot), `api.md` (every request) and `development.md` (`make` targets, tests, releases). Update
  the right one in the same change, and keep the README short: what a user needs, not how the code does it.
- **Spelling.** American English everywhere: code, comments, the page and the docs ("color", "center", "meter",
  "gray", "around" not "round"). The watched planes' `colour` key is still read for settings stored by older firmware.

## Style

- C++: 2-space indent, lines up to 120 columns, `snake_case` functions and variables, members with a trailing `_`.
  Match the surrounding file.
- Comments and docs are plain English in the project's own voice: say what the screen does in everyday words ("the
  glass", "a plane's card", "the screen keeps them") rather than jargon. Explain why, not what the code says.
- Commit messages: a first line that sums up the change, up to about 72 characters, no prefixes, no trailing period,
  e.g. "Test touches that a slow frame cannot lose" or "Fix the internal RAM leak behind the black screens". Then a
  blank line and a list of the changes, one `- ` item each (wrapped at 72 columns, continued lines indented two
  spaces), each saying what changed and why, led by its kind in brackets: `[feature]`, `[fix]`, `[tests]`, `[docs]`,
  `[terms]` (a service's terms or a license), `[memory]`, `[cleanup]`. A fix says what went wrong and how it showed
  ("both screens went dark at every restart"); a change made for a service's terms names the service and the rule.
  Close with anything a reader must know before building or releasing it (an option to turn off, a step still to
  take). Plain English in the project's voice, as the comments are.

## Releases

Never tag, push or release without being asked. The steps are `make release-version VERSION=x.y.z`,
`make release-tag MSG="..."`, `make release`; pushing a tag publishes firmware that every screen is offered.
