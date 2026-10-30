# AGENTS.md

Guidance for coding agents working in esp-skyscope: an ESP32-S3 touch screen that fetches ADS-B traffic and map tiles
from public services and draws them itself. The firmware is an ESPHome external component. `README.md` is the user
guide; read it for what the screen does and how it is set up.

## Layout

- `skyscope.yaml`: the device config (version, update server, packages). `boards/`: one file per board.
- `components/skyscope/`: the firmware. `skyscope.h` explains the two threads; start there.
  - Plain C++ with no ESPHome or ESP-IDF includes, so the host tests can build them: `geo.h`, `settings.h`, `feed.h`,
    `tiles.h`, `overlay.h`, `plan.h`, `pixels.h`, `firmware.h`, `schedule.h`, `menu_rows.h`, `keyboard.h`, `places.h`
    (namespace `sky`). Keep
    them that way; logic that can be tested on a PC belongs here.
  - On the device only: `skyscope.cpp`, `worker.cpp`, `view.cpp`, `menu.cpp`, `editor.cpp`, `web.cpp`, `http.cpp`,
    `probe.cpp`
    (namespace `esphome::skyscope`).
  - `page.h`: the whole settings page (HTML and script) as one raw string. No external resources.
  - `__init__.py`: the component's YAML schema and codegen.
- `tests/`: `test_host.cpp` (C++ on a PC), `test_page.mjs` (the page's script), `test_release_site.py`,
  `device_test.py` and `mock_feed.py` (against a real screen).
- `tools/`: `release_site.py` (the release manifest), `markers.cpp` (the aircraft sheet).

## Build and test

- `make test`: everything that needs no screen (host C++ with ASan/UBSan, Python, Node). This is what CI runs; run it
  after any change. Needs a C++20 compiler, Python 3, Node 18+.
- `make firmware`: compiles with the ESPHome in `ESPHOME_VERSION` (Makefile) and refuses another version;
  `make firmware-esphome` installs it. Slow; run it when device code, `__init__.py` or YAML changes.
- `make test-markers`: renders `tests/build/markers.png` after changing aircraft shapes (`shapes.h`, `sprites.h`).
- `make test-esp32*` need `DEVICE=<address>` and a real screen. Do not run them unless the user gives an address.
  Device tests must put back every setting they change and never print the screen's location.

## Things that bite

- **Threads.** The worker task (core 0) fetches the feed and tiles; ESPHome's loop owns LVGL and the glass; the web
  server's task only reads a copy of the settings or hands a new one to the loop. Shared state lives in `Shared` under
  `lock_`, the base frame under `frame_lock_`. Never touch LVGL off the loop.
- **Memory.** Internal RAM is scarce and has run out before (black screens, broken replies). Large buffers and TLS go
  in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`). Watch for leaks: the soak test exists
  because of one.
- **Public services.** adsb.fi, adsb.lol, Esri, OSM, USGS and FAA have terms; keep to them. Respect rate-limit headers
  and backoff in `feed.h`, keep the User-Agent and tile caching, and keep attribution as each source requires.
- **Test endpoints** (`/api/touch`, `/api/ui`) are compiled out with `test_api: false`; keep them behind that flag.
- **README.** Features, settings, the API table and the `make` targets are documented there. Update it in the same
  change.

## Style

- C++: 2-space indent, lines up to 120 columns, `snake_case` functions and variables, members with a trailing `_`.
  Match the surrounding file.
- Comments and docs are plain English in the project's own voice: say what the screen does in everyday words ("the
  glass", "a plane's card", "the screen keeps them") rather than jargon. Explain why, not what the code says.
- Commit messages are one descriptive line saying what changed, no prefixes, no trailing period, e.g.
  "Test touches that a slow frame cannot lose" or "Fix the internal RAM leak behind the black screens".

## Releases

Never tag, push or release without being asked. The steps are `make release-version VERSION=x.y.z`,
`make release-tag MSG="..."`, `make release`; pushing a tag publishes firmware that every screen is offered.
