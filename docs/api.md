# The web page and API

The screen serves its settings page at `/` and a small JSON API beside it. Both are open to anyone on your network, as
an ESPHome device's own web server is. Back to the [README](../README.md).

```sh
curl -X POST http://<screen>/api/settings -H 'Content-Type: application/json' \
  -d '{"lat": 51.4700, "lon": -0.4543, "range_nm": 15, "map": "dark_gray"}'
```

## Requests

| Request | What it does |
|---|---|
| `GET /api/settings` | every setting |
| `POST /api/settings` | changes the settings given in a JSON object, leaving the rest. A wrong value answers 400 with `{"error": "<key>"}` and changes nothing. `{"follow": "BAW286"}` follows a plane, `{"follow": ""}` stops |
| `GET /api/status` | how the screen is doing ([below](#get-apistatus)) |
| `GET /api/aircraft` | the planes of the last report |
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
| `GET /api/ui` | what the glass shows: the status line, a plane's card, the brightness, the menu and its keyboard, the map a swipe went to, the weather page, and where each plane is |
| `GET /api/screenshot` | what the glass shows, as a BMP of its own pixels (16-bit RGB565, the screen's way up) |
| `POST /api/test` | `{"divert": "http://<host>:<port>"}` sends every request to a public service to that server instead (as `/<server>/<path>`), from the restart it makes until power off or `{"divert": ""}`. `GET /api/status` says where (`divert`) |
| `POST /api/bench` | `{"runs": 3}` times decoding and drawing every tile of the map shown, each way of drawing in turn. `GET /api/bench` says whether it is `busy` and gives the `result` |
