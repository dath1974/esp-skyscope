# The web page and API

The screen serves its settings page at `/` and a small JSON API beside it. Both are open to anyone on your network, as
an ESPHome device's own web server is, unless a password is set (**Password for this page** on the page, or **Web
password** in the menu's Screen group): then the page and every request ask for it, as HTTP Basic with the user name
`admin`. Back to the [README](../README.md).

```sh
curl -X POST http://<screen>/api/settings -H 'Content-Type: application/json' \
  -d '{"lat": 51.4700, "lon": -0.4543, "range_nm": 15, "map": "dark_gray"}'
curl -u admin:<password> http://<screen>/api/stats    # with a password set
```

## Requests

| Request | What it does |
|---|---|
| `GET /api/settings` | every setting, but the password: `password_set` says whether there is one |
| `POST /api/settings` | changes the settings given in a JSON object, leaving the rest. A wrong value answers 400 with `{"error": "<key>"}` and changes nothing. `{"follow": "BAW286"}` follows a plane, `{"follow": ""}` stops; `{"password": "..."}` sets the password, `{"password": ""}` removes it |
| `GET /api/status` | how the screen is doing ([below](#get-apistatus)) |
| `GET /api/aircraft` | the planes of the last report |
| `GET /api/stats` | what the screen has seen in its range today ([below](#get-apistats)) |
| `GET /api/log` | the latest visits of planes to the range (up to 400), as CSV; `?format=json` for JSON, newest first |
| `GET /metrics` | for Prometheus: planes in range and today, memory, sockets, the loop, the feed and the tile cache |
| `GET /api/firmware` | the version running, the newest release, an install's progress, and the manifest it asks (`source`) |
| `POST /api/firmware` | `{"check": true}` asks for the newest release again; `{"install": true}` installs it |
| `GET /api/networks` | the Wi-Fi networks the screen knows, first choice first: their names only, never a password |
| `GET /api/card` | the map card: mounted or why not, its size and free space, tiles read, written and failed, old tiles still being deleted, whether it needs formatting (`unformatted`), and the download in progress |
| `POST /api/card` | the map card's actions ([below](#post-apicard)) |
| `POST /api/reboot` | restarts the screen |
| `POST /api/debug` | for measuring only, never stored: `{"planes": false}` hides the planes |

A watched plane in the settings is `{"id": "N12345", "color": "red"}`. The screen also accepts the British spelling,
`"colour"`, which older firmware saved its settings with.

### GET /api/status

- **Network and memory**: Wi-Fi, free memory, rotation.
- **Aircraft**: how many, where they came from (`feed_source`, and `feed_simulated` for a receiver's made-up planes),
  and the feed's last error.
- **The map**: how far it has drawn, the tile cache (`tile_cache`: tiles and KB held, fetched, and from the cache), and
  how long the map takes (`tile_times`: each step's `count`, `mean_ms` and `most_ms`, and the last map's
  `last_frame_ms`).
- **Overlays**: the weather radar's scan shown (`radar_scan`), and the airspace shown (areas, runways, bytes, last
  error, and `airspace_kept` when it came from flash).
- **Following** (`follow`): what was asked, its `state` (looking, live, lost, landed or gave_up), the status line's
  words, and once found the plane's hex, callsign, registration, position, whether that is `estimated`, and seconds
  since it was last heard.
- **The radio** (`radio`, P4 only): what it is doing, messages a second and in all, errors mended and checksums failed,
  planes heard and placed, samples dropped, its gain and noise floor.
- **The map card** (`card`), as `GET /api/card`.
- **Timing**: frames (drawing, waiting for the panel, sending), the LCD 2.8B's panel refills (`refills`), slow frames
  by what the worker was doing, and reports on the loop and the worker.

### GET /api/stats

The planes within the range of the place (up to 250 NM), since midnight by the screen's clock, told apart by their
transponders' addresses; only reports around the place count, none while a plane is followed elsewhere. The tally is
kept over a restart.

- `date`, `planes`, `visits` (a plane heard again after ten minutes away is a new visit), `in_range` now, `busiest`
  (the most at once), `watched` and `emergencies` (visits that raised an alert).
- `hours`: the planes first seen in each hour, 0:00 to 23:00.
- `types` and `operators`: the commonest, each `{"name", "count"}`.
- `closest_nm`, `highest_ft`, `fastest_kt`: the day's records, each `{"name", "value", "at"}` (Unix seconds).
- `days`: the planes of the last 14 days, today's last.

A line of the log (and of the card's own, a file a day in its `log` folder) is a visit: when it began and ended (the
screen's local time), the plane's hex, callsign, registration, type and operator, its nearest to the place (NM, or km
on a metric screen), its lowest and highest altitudes, and whether it was watched or squawked an emergency.

### POST /api/card

An area is `{"map", "lat", "lon", "radius_nm", "z_max"}`, its map one of `sectional`, `ifr_low`, `ifr_high` or `topo`.

| Body | What it does |
|---|---|
| `{"estimate": area}` | the tiles an area takes, its megabytes to download (`mb`) and on the card (`card_mb`), and the card's free space |
| `{"download": area}` | downloads the area |
| `{"stop": true}` | stops the download |
| `{"mount": true}` | looks for the card again |
| `{"clear": true}` | deletes the screen's tiles |
| `{"format": true}` | formats the card FAT32, erasing it |

## Test endpoints

These are for the tests and the pictures. A build with `test_api: false` in the `skyscope:` block leaves them out,
along with the `source` field and the network changes.

| Request | What it does |
|---|---|
| `POST /api/networks` | `{"move": [i, step]}`, `{"forget": i}` or `{"join": {"name", "password"}}` (no restart) |
| `POST /api/touch` | a touch on the glass at the screen's own coordinates: `{"gesture": "tap", "x": 200, "y": 300}`, or `"hold"` (with `"ms"`), or `"drag"` to `"x1"`, `"y1"` |
| `GET /api/ui` | what the glass shows: the status line, a plane's card, an alert along the top, the brightness, the menu and its keyboard, the map a swipe went to, the weather page, the day's tally (`today`: its `line`, its three numbers in `stats`, its `lines` and its hours' `bars`), whether the emblem of the start is still up (`splash`) and how the last one ran (`splash_frames`, and `splash_gap_ms`, its longest wait between two frames), and where each plane is |
| `GET /api/screenshot` | what the glass shows, as a BMP of its own pixels (16-bit RGB565, the screen's way up) |
| `POST /api/test` | `{"divert": "http://<host>:<port>"}` sends every request to a public service to that server instead (as `/<server>/<path>`), from the restart it makes until power off or `{"divert": ""}`. `GET /api/status` says where (`divert`). `{"lock_touch_s": 900}` takes no notice of fingers on the glass for 900 seconds (and over a restart), so a touch cannot upset a test, while `/api/touch` goes on; the glass says so, and `{"lock_touch_s": 0}` ends it. `GET /api/status` says how long is left (`touch_locked_s`). `{"splash": true}` shows the emblem of the start again |
| `POST /api/bench` | `{"runs": 3}` times decoding and drawing every tile of the map shown, each way of drawing in turn. `GET /api/bench` says whether it is `busy` and gives the `result` |
