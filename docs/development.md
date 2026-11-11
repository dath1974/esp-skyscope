# Development

How the code is laid out, how to build and test it, and how releases are made. Back to the [README](../README.md).

## Layout

- `skyscope.yaml`: the device config. `boards/`: one file per board, picked by the `board` substitution.
- `components/skyscope/`: the firmware, an ESPHome external component. `skyscope.h` explains how its two threads
  share the work. The headers that need no screen are plain C++ with no ESPHome or ESP-IDF includes, so the host tests
  can build them; `AGENTS.md` lists them.
- `components/rgb_panel/`: the LCD 2.8B's display driver.
- `tests/`: the host tests, the page's tests, and the tests against a real screen with the mock they use.
- `tools/`: the release manifest, the aircraft sheet, the emblem, the screenshots, and the wake category table.

## Building

- `make firmware` builds for `BOARD` (the AMOLED 2.41 unless told another), with the ESPHome version in the `Makefile`
  (`make firmware-esphome` installs it). `make firmware-all` builds every board, as a release does.
- `make firmware-flash DEVICE=<address or serial port>` builds and installs. The screen shows **Updating** while a
  build sent over Wi-Fi comes in.
- `make` builds the firmware and runs `make test` (and `make test-esp32` too, given `DEVICE`).
- `make help` lists every target; `make clean` removes what the build and tests leave, keeping ESPHome's toolchain.

## Tests that need no screen

`make test` runs these, as CI does on every push and before every release. It needs a C++20 compiler, Python 3 and
Node 18 or newer.

- `make test-host`: the C++ host tests (`tests/test_host.cpp`), with the address and undefined-behavior sanitizers.
  `tests/run.sh reply.json` reads a saved feed reply too.
- `make test-python`: the release manifest, the mock the device tests use, and `tools/screenshots.py`.
- `make test-page`: the web page's script (`tests/test_page.mjs`).
- `make test-markers` draws every aircraft shape on a sheet (`tests/build/markers.png`); CI keeps it with each run.
- `make emblem` draws the emblem (`components/skyscope/emblem.h`) as the screen's own code does, on a sheet to look at
  (`tests/build/emblem.png`), and writes `docs/emblem.svg` and the web page's copy (`tests/build/emblem-page.svg`,
  pasted into `page.h` after a change).

## Tests against a real screen

These need a screen on the network running a firmware with the test API (`test_api: true`, the default). Every setting
they change is put back, and they never print the screen's place. While a run goes on, fingers on the glass do nothing
(it says `Test running: touch is off`), so a touch cannot upset it; the run's own touches go through the test API.
The lock ends with the run, or by itself 15 minutes after it was last renewed if the run dies.

All but `test-esp32-public` run against a mock this machine serves (`tests/mock_feed.py`, port 8095, or `PORT=`; each
screen tested at once needs its own). The screen is sent to the mock (it restarts), every request it makes of a public
service goes there, and it is sent back at the end. So a run goes the same way every time and on every board, and
asks nothing of the public services. `tests/mock_services.py` says what the mock answers for each service, and a run
fails on any request the mock did not know. The screen must be able to reach this machine
([under WSL, see below](#a-screen-reaching-a-test-server-under-wsl)).

- `make test-esp32 DEVICE=<address>`: the page, settings, status and memory, aircraft, airspace, and the glass through
  `/api/touch`: the menu and its keyboard, the Wi-Fi list, firmware, a plane's card, swiping between maps, the weather
  page and radar, and the brightness strip.
- `make test-esp32-full DEVICE=<address>`: also every map drawn whole, a restart that keeps the settings, and sleep
  and waking.
- `make test-esp32-mock DEVICE=<address>`: the mock as a receiver (filters, range, crowded skies, bad entries, and
  every way a feed can fail) and as the update server (installs that must fail leave the firmware as it is). With
  `INSTALL=.esphome/build/skyscope-<board>/build/firmware.ota.bin`, that build is installed from the mock.
- `make test-esp32-soak DEVICE=<address> SOAK=60 CHURN=2`: memory watched for an hour under 80 planes, the view
  changed every two minutes, failing on a leak or a restart.
- `make test-esp32-public DEVICE=<address>`: the real public services, asked at a test place (New York JFK), to check
  they still answer as the screen expects. Not repeatable, since the sky and the services change.

## Pictures

`make screenshots DEVICE=<address>` (`ONLY=streets-dark,radar` for some) takes the pictures in
[screens.md](screens.md) from the screen's own glass, at public airports, never the screen's own place. The maps,
airspace and weather are real; the planes are made up and served from this machine as a receiver, credited as
**simulated** on the glass. It writes `docs/screens.md` and puts every setting back. It needs the test API and, like
the device tests, a screen that can reach this machine.

## A screen reaching a test server under WSL

WSL's default network is a private one behind Windows, which nothing on the network can reach. In
`%UserProfile%\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored
```

then `wsl --shutdown` and open WSL again: it shares the PC's own address. Windows' firewall for WSL still refuses
connections from the network; in a PowerShell run as administrator, allow the mock's ports (8095, and one more for each
screen tested at once):

```powershell
New-NetFirewallHyperVRule -Name skyscope-mock -DisplayName "Skyscope mock feed" -Direction Inbound `
  -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol TCP -LocalPorts 8095-8099
```

(A rule made before for 8095 alone: `Set-NetFirewallHyperVRule -Name skyscope-mock -LocalPorts 8095-8099`.)

## Releasing

Maintainers only. Pushing a tag publishes firmware that every screen is offered.

1. `make release-version VERSION=x.y.z` sets `version` in `skyscope.yaml` and commits it.
2. `make release-tag MSG="What changed, in one line"` checks that main is committed and current, builds every board,
   runs the tests and makes the annotated tag `v<version>`. The tag's message is the summary the screen's page shows.
3. `make release` asks, then pushes main and the tag. `.github/workflows/release.yml` builds every board, makes the
   GitHub release and publishes each board's firmware with its own manifest (`tools/release_site.py`).
