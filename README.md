# esp-skyscope

Live aircraft tracking on a self-contained ESP32 touch screen: the ESP32 (an S3 or a P4) fetches traffic information
from a public ADS-B exchange and the map tiles from public tile servers, decodes and draws everything itself.

- **Maps**: a radar scope drawn on the screen (nothing downloaded), streets (Esri's gray maps, dark or light),
  OpenStreetMap, satellite imagery (Esri), the USGS topographic map, and the FAA's VFR sectional, IFR area (low) and
  IFR high enroute charts, or any 256-pixel PNG/JPEG tile server of your own. One **appearance** switch, dark or light:
  maps without a dark version of their own are drawn dark by rules made for each (the IFR charts turn only their paper
  and ink round and keep their colours; the sectional is dimmed; topo and OpenStreetMap are inverted with their hues
  kept). Place names over the streets and the imagery (optional). Range rings and a compass rose over any map. A new
  screen starts on the streets, light.
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
  coloured by altitude; labels kept apart from each other, their altitude in hundreds of feet as on a controller's
  scope (`025`, `350`, and `-001` below sea level) with an arrow after it for a climb or a descent of 200 ft a
  minute or more. A label shows the callsign, the altitude, or the type and speed as well, or is a controller's
  data block (`Scope`): the callsign over altitude, ground speed in tens of knots and the FAA's wake category
  (`350 43 B`: A super to I lower small, by type from FAA Order JO 7360.1, `tools/wake_table.py`). Tracks of the
  last seconds; planes
  optionally glide between reports; a "radar beam" that paints them as it passes. Watched aircraft (by registration,
  callsign or ICAO address) always show, in a colour of their own. Altitude limits, ground traffic on or off, the most planes shown.
- **Follow a plane** anywhere in the world (see [Following a plane](#following-a-plane)): by its registration,
  callsign or flight number, or from its card. The map moves with it, out to 5000 NM, and over an ocean, out of reach
  of the receivers, the screen shows where it is likely to be until it is heard again.
- **On the glass**: drag up or down along the right edge to set the brightness; the screen can turn itself off after a
  while without a touch, and at a bedtime every night, on again at a time or a tap (while it is off no planes or
  tiles are fetched). A long press on the map opens a settings menu with every setting the web page has (see
  [The menu](#the-menu)): its arrows step a value, and a tap on a value's field types it on a keyboard on the glass. It
  also finds a place, changes the Wi-Fi network, and shows the firmware and installs a new release. Tap a plane for
  its card (type, registration, altitude, an arrow up or down for a climb or descent with its rate to the nearest
  100 ft a minute, speed, heading, operator, squawk); tap the map to see the screen's
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
| Waveshare ESP32-S3-Touch-LCD-2.8B (Rev 1.1, 480 × 640) | `boards/waveshare-lcd-28b.yaml` |
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

The LCD 2.8B's panel takes its picture over a 16-bit RGB bus from a frame the ESP32-S3 keeps in PSRAM, a frame more
than the AMOLED needs; its setup and pins are those of Waveshare's own demo. That frame is read from PSRAM all the time,
so the panel runs slower than the demo's (a pixel clock of 12 MHz, not 30): faster, it takes so much of the PSRAM that
the picture jumps and drawing slows until the watchdog restarts the screen. The screen drives the panel with a driver
of its own (`components/rgb_panel`), since ESPHome's sets a pixel format the board's wiring does not match and every
colour comes out wrong. The frame reaches the panel through small buffers of internal RAM that leave the cache as
they found it, so the panel's constant reading of the PSRAM does not crowd out drawing; the board is best standing
(0°), since every other angle turns each frame in software.

A board needs an ESP32-S3 or ESP32-P4 with PSRAM (the map takes up to five frames of the screen, the tile cache up to
3 MB) and an LVGL-capable display. Adding one is a new file in `boards/` with its display, touch, fonts and the
`skyscope:` block's `dpi`, and the board in the build matrix of `.github/workflows/tests.yml` (a test checks it is
there); a release builds and publishes every file in `boards/` by itself. Two boards may have the same chip. Any new
board will require ample testing, noting the goal of this project is a physically small display size.

### The screen's own radio (ESP32-P4)

On the P4 board an RTL-SDR stick on its USB-A port makes the screen a receiver of its own: the FlightAware Pro Stick
Plus, or any stick with an RTL2832U and an R820T or R820T2 tuner. Nothing else is needed: the screen tunes it to
1090 MHz, reads two million samples a second from it, finds the Mode S replies in them and their checksums, and works
out each plane's identity, altitude, speed, track, squawk and place from them, as dump1090 does on a computer. It is
**Radio** among the sources, on the page and in the menu (only on a screen built with one), and with falling back
allowed the exchanges stand in while no stick is plugged in. The page's status shows how it is doing: messages a second,
planes heard and placed, and samples dropped if the demodulator ever falls behind.

**No network needed.** A screen with a radio works without Wi-Fi. When it has been off a network for 20 seconds (at
start, or after losing one), it shows the planes the radio hears, whatever source is set, and the status line ends with
**no Wi-Fi**. With a [map card](#the-map-card) in the slot the map chosen stays, drawn from the card: squares it does
not have stay empty, and the status line says how many. Without one it shows the radar, and swiping the strip at the
bottom changes nothing. Nothing is fetched meanwhile (no airspace or weather radar). The settings themselves stay as
they were, so everything chosen comes back as soon as the screen is on a network again; a connection lost for a few
seconds keeps the map. The place cannot be looked up offline: type its latitude and longitude in the menu (a long
press), or set it once while on a network and it is kept. With no network the clock is not set, so bedtime does not turn
the screen off.

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

### The map card

Every board has a TF (microSD) slot. With a card in it, formatted FAT32, every map tile the screen fetches is kept on
the card as well as in memory, so a map seen once comes back without downloading it again, after a restart too: quicker
on a slow network, and less asked of the tile servers. Each map's tiles are kept as long as its terms allow:
OpenStreetMap's and Esri's for as long as their servers say (`Cache-Control`, a week where they say nothing), the FAA's
charts for 28 days (each is published anew every 56) and the USGS map for 90. Then they are fetched again; an old one
is still shown if that fails. The custom tile server's and the weather radar's are never kept. The screen keeps its
tiles in a folder `tiles` on the card, one file each, and leaves everything else on it alone.

On the web page, **Map card** shows the card's free space and downloads an area ahead of time: the FAA sectional, the
IFR low or high charts, or the USGS topographic map, round the place, to a radius of up to 250 NM, every zoom of the map
down to the one chosen. The page estimates the tiles, the room they take on the card and the time first, and says when
an area is more than the card has free (a 100 NM sectional is some 950 tiles: 37 MB to download, about 52 MB of the
card, since every file fills whole blocks of the card, and a quarter of an hour). The screen asks for a tile at a time,
only while the map shown is whole, waits after a failure (2 seconds, then twice as long each time, at most 5 minutes)
and leaves a tile out after six tries. It keeps going while the screen is off and carries on after a restart; tiles the
card has already are skipped. Only these maps are downloaded ahead: they are works of the United States government, free
to copy, while OpenStreetMap's and Esri's ask that their tiles are fetched only as they are looked at. The card is
looked for when the screen starts (or at **Look for the card again** on the page); a card that stops answering is let
go.

**Formatting and clearing, on the screen.** A card must be FAT32. One that is not (a new card larger than 32 GB comes as
exFAT, or a blank one) is formatted by **Format the card** on the page, FAT32 whatever its size, with no computer; it
erases everything on the card. **Clear the screen's tiles** deletes only the screen's own: the folder of tiles is moved
aside at once and its files deleted a few at a time while the screen has nothing else to do (across restarts too), so a
large cache takes some minutes to go while the maps are already fetched afresh. Every file is written whole to one file
first (`tiles/write.tmp`) and then renamed into its place, so a crash or a power cut in the middle of a write never
leaves a broken tile to be read, and leaves at most that one file, which the next write replaces; a tile is written only
once its download is complete.

The board files set the slot (`card:` in the `skyscope:` block): on the P4, slot 0 of its SD/MMC host, four data lines,
powered by its LDO channel 4 (slot 1 of the same host is the Wi-Fi chip's); on the AMOLED 2.41, one data line on GPIO 4,
5 and 6; on the LCD 2.8B, one data line on GPIO 2, 1 and 42, the first two shared with the lines that set up its panel
(which is set up once at start, before the card is looked for), and the card's DAT3 held high by the board's I/O
expander.

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
Makefile's ESPHome version first (`DEVICE` can be a serial port too). The screen shows **Updating** while a build sent
over Wi-Fi comes in, then restarts into it.

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
with its manifest (`tools/release_site.py`). The tag's message is the summary the page shows.

## First start

1. The screen shows **Set up Wi-Fi**. Press **Choose a network here**, pick your network from those in reach (**Scan**
   looks again) and type its password on the screen's keyboard. Or, on a phone or other device, join the Wi-Fi
   network **Skyscope Setup**: a page opens (or go to `http://192.168.4.1`) to choose your network and enter its
   password. (Or use Improv from [ESPHome Web](https://web.esphome.io) while the screen is on USB.) While the screen
   tries a network it cannot join, the same button chooses another.
2. The screen joins your network and opens its menu, at the place, with its address at the top. Set it there, or open
   that address in a browser on the same network. (Cancelled, the screen shows **Where are you?**; hold a finger on it
   for the menu again.)
3. Search for a place (an airport, a town) or enter its latitude and longitude, choose a range and a map, and press
   **Save**. The display should draw the map and begin showing traffic within a few seconds.

All settings are on that page and in the menu, and the screen keeps them across restarts and updates. Once it is set up and running,
tapping the status bar at the top shows the screen's address for a moment.

**Rotation** is set there too (the screen restarts to turn). 0° is the panel's own way up, every board's default and the
least expensive: every other angle turns each frame in software; 90° and 270° cost the most (on the AMOLED 2.41 and the
LCD 2.8B, 0° is portrait and 90° landscape).

## The menu

A long press on the map (or on the setup screen) opens the menu. Its rows are grouped as on the web page (Where, Map,
Aircraft, Source, Screen) and hide where they do not apply, as the page's do: the phosphor on the radar only, the
bedtime's times with bedtime on. A value between two arrows steps through common choices; a value in a field opens
the keyboard at a tap, and a few (the range, the brightness) have both. Typed values are checked as the page's are;
one the screen cannot take is refused, with what it may be shown under the text. Changes show in the accent and wait
for **Save**; **Cancel** drops them.

- **The keyboard** has letters, numbers and a page of symbols (every printable ASCII character), laid out as on a
  phone: the up arrow for capitals, **?123** for numbers and punctuation, arrows to move the cursor and the backspace
  key. Numbers and times get a pad of digits.
- **Place** finds a place as the page does, but from the screen itself: an airport code (KJFK, JFK) in the FAA's
  airport data, an airport's name there too, then anything in OpenStreetMap's Nominatim, at most once a second and
  only when **Search** is pressed; an airport code the FAA knows gives just that airport. Pick one of what it found; an
  airport is named by its code. Places come with their English names, and one whose name is in a script the screen
  cannot write (its letters are Latin only) is left out. Latitude and longitude can be typed instead.
- **Follow** takes a plane to follow: `N12345`, `G-EUPT`, `BAW286` or `BA 286` (see
  [Following a plane](#following-a-plane)); empty to stop.
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

The range is any whole number from 1 to 5000 NM, by a pinch or typed in the menu's Range field. The exchanges answer
for 250 NM round a point at most, so beyond that the planes shown are those within 250 NM of the place (or of the plane
followed), and the airspace is not shown. A drawn map's scale moves in whole tile levels (each twice the last), so
after the three seconds a pinched map may settle a little larger or smaller than the fingers left it; the rings and the
planes asked for are at the exact range. The radar has no tiles and shows any range as it is.

While a plane is followed, a pinch or a drag previews the view round it in the same way: **Save** keeps the range (the
place stays as it was), and the view goes on following the plane from where the fingers left it.

## Following a plane

Type a plane in the menu's **Follow** field or the web page's **Follow a plane**, or tap a plane and press **Follow**
on its card (**Stop following** there, or an empty field, stops). It can be named by its registration (`N7209W`,
`G-EUPT`, with or without the dash), its callsign (`BAW286`) or the flight number on a ticket (`BA 286`): the larger
airlines' flight numbers are turned into the callsign their planes send (a table kept by hand in `follow.h`); for any
other airline type its callsign. The screen looks it up at adsb.lol or adsb.fi, which both find a plane anywhere in
the world; a plane not in the air yet is looked for again every two minutes, and the status line says
`Looking for BA286` meanwhile. Once found, it is followed by its transponder's address, kept with the settings, so
following carries on through a restart or the screen going to sleep.

- The map moves with it: once the plane is more than 60% of the way from the middle to an edge, the view steps on so
  the plane sits a little behind the middle, with most of the glass ahead of it. The old picture stays until the new
  map is whole. Your place is not changed; following stops and the view goes back to it.
- The planes round it are asked for instead of those round the place, as far as the view reaches from it, at most
  250 NM. The plane itself is ringed (in blue, unless it is watched in a colour of its own) and has a label of its own,
  chosen apart from the others' (**Its label**, a controller's data block, `Scope`, unless set otherwise; `auto` is as
  the others'), so the rest can be bare callsigns while it shows its altitude, speed and wake category.
- Its track is its whole flight since it was found (from the few minutes the screen had of it before), or as many
  minutes as **Its track** is set to, up to a day; the other planes keep their own track setting. A long flight keeps
  its shape in a couple of thousand places, the older part thinned. A stretch out of reach is a straight line across.
- A chart of the United States (the sectional, the IFR charts, USGS topo) gives way to the streets map while the view
  is outside the United States, and comes back over it; the map setting itself is not changed. Range rings are not
  drawn while following, as they measure from the place.
- Out of reach: the exchanges hear what their volunteers' receivers on the ground hear, so a plane crossing an ocean
  (the North Atlantic, three or four hours of it) or over a land with few receivers is lost to them. After a minute
  without a report the screen carries it on along the great circle it was flying, at its last speed, drawn faint and
  labelled `estimated` (the status line says `out of reach 42 min, estimated`), and asks for it by its address every
  30 s until it is heard again. A plane lost low and coming down is held where it was last seen instead.
- Its range: a plane is shown at its own range when following begins, chosen by its speed so the map moves on about
  once a minute (a whole zoom of the map either side of it, so between some 40 and 85 seconds): 1 NM for a plane at
  60 kt, 4 NM at 250 kt, 7 or 8 NM for an airliner at cruise. A plane whose speed is not known is shown 100 NM round
  it. The range the screen had before is kept, and comes back when following stops.
- Each step drawn ahead: a plane flying on as it is will cross into its next view at a place known in advance, so
  while it flies there the screen draws that view's map behind the one shown, a tile at a time once the map shown is
  whole. When the step comes, and the plane is about where it was expected (within some 15 % of half the glass), the
  map moves to that view at once; a step that comes before it is finished shows what it has and draws the rest. A plane
  that turns gets a new view planned within a second. Only that one view's tiles are fetched, a minute or so before
  they are looked at.
- Keys down the left edge while a plane is followed: **Plane** (a sight) puts the plane back at its own range for its
  speed now, **Own range** (a magnifier with a roof in its glass, shown while the range is not already that) at the
  range the screen had before following, still round the plane, and **Route** (in a build with routes, below) the
  whole route and the plane on it; the view then stays put until the plane would leave it. Each keeps its range, as a
  pinch's **Save** does.
- Its route, in a build with routes (`make firmware ROUTES=true`, or `routes: true` in the `skyscope:` block; off in
  the published firmware): the airports it flies from and to, asked of [adsbdb](https://www.adsbdb.com) by its callsign,
  and the great circle between them drawn under everything in a neutral line (the ink at half strength: light on a
  dark map, dark on a light one), its own track in its colour over it. Its card names them (`KPDX to EGLL`). This is no
  filed flight plan (those are not free to have), only the way a long flight roughly flies. adsbdb's route data is
  David Taylor's and Jim Mason's and may not be copied or published without their leave, which is why it is left out
  of the releases.
- Following stops by itself once the plane has landed (on the ground for ten minutes after flying, or lost low and
  coming down for as long), or after it has been out of reach for twelve hours. A plane found at its gate before it
  flies is waited for.

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
| `POST /api/settings` | changes the settings in a JSON object (only the keys given); a wrong value answers 400 with `{"error": "<key>"}` and changes nothing. `{"follow": "BAW286"}` follows a plane, `{"follow": ""}` stops |
| `GET /api/status` | Wi-Fi, free memory, aircraft, the feed's last error, map progress, the tile cache (`tile_cache`: tiles and KB held, tiles fetched and their KB, tiles from the cache, maps shown whole again), how long the map takes (`tile_times`: for a tile's download (`fetch`), its read from the map card (`card`), its decoding (`png`, `jpeg`) and its laying into the frame (`lay`), and for maps drawn from their tiles (`frames`), each a `count`, `mean_ms` and `most_ms`; the last map's `last_frame_ms`, `last_frame_tiles` drawn and `last_frame_from`: `tiles`, `kept`, `ahead`, or `ahead, part`; and `frames_ahead` and `tiles_ahead`, a followed plane's steps drawn ahead), the weather radar's scan shown (`radar_scan`), the airspace shown (areas, runways, bytes, last error), the plane followed (`follow`: what was asked, `state` one of looking, live, lost, landed or gave_up, the status line's words, and once found its hex, callsign, registration, where it is or is likely to be, whether that is `estimated`, and the seconds since it was last heard), the screen's own radio (`radio`: what it is doing, messages a second and in all, one-bit errors mended, checksums failed, planes heard and placed, positions thrown away, samples dropped, its gain and noise floor), the map card (`card`, as `GET /api/card`), rotation, and timing: frames (drawing, waiting for the panel, sending), the LCD 2.8B's panel refills (`refills`: in all, late in all and in the last five seconds, the longest copy and span against its budget, in µs; a late one is a mark on the glass), slow frames by what the worker was doing, a report on the loop and on the worker |
| `GET /api/aircraft` | the planes of the last report |
| `GET /api/firmware` | the version running, the newest release, an install's progress, and the manifest it asks (`source`) |
| `POST /api/firmware` | `{"check": true}` asks for the newest release again; `{"install": true}` installs it |
| `GET /api/networks` | the Wi-Fi networks the screen knows, first choice first: their names only, never a password |
| `POST /api/networks` | with the test API only: `{"move": [i, step]}`, `{"forget": i}` or `{"join": {"name", "password"}}` (no restart) |
| `GET /api/card` | the map card: mounted or why not, its size, free space and cluster size, tiles read from it, written and failed since the start, the frame's tiles not on it (offline), old tiles still being deleted (`clearing`, `cleared`), whether a card has no FAT32 (`unformatted`), and the download (map, radius, zoom, how far, fetched, skipped, left out, waiting) |
| `POST /api/card` | `{"estimate": area}` the tiles an area takes, its megabytes to download (`mb`) and on the card in whole clusters (`card_mb`), and the card's free space, `{"download": area}` downloads it, `{"stop": true}` stops, `{"mount": true}` looks for the card again, `{"clear": true}` deletes the screen's tiles, `{"format": true}` formats the card FAT32 (erasing it); an area is `{"map", "lat", "lon", "radius_nm", "z_max"}`, its map `sectional`, `ifr_low`, `ifr_high` or `topo` |
| `POST /api/reboot` | restarts the screen |
| `POST /api/touch` | a touch on the glass for tests: `{"gesture": "tap", "x": 200, "y": 300}`, or `"hold"` (with `"ms"`), or `"drag"` to `"x1"`, `"y1"`; at the screen's own coordinates |
| `GET /api/ui` | what the glass shows, for tests: the status line, a plane's card, the brightness level, the menu (rows, values, and where each key is), its keyboard (text, hint and keys) and the list over it (places or networks), the map a swipe went to, the weather page (title, line, tab, lines, keys), and where each plane is |
| `POST /api/test` | for tests: `{"divert": "http://<host>:<port>"}` sends every HTTPS request the screen makes (every public service: feeds, tiles, airspace, weather, the update server) to that server as `/<server>/<path>`, a plain `http://` address (a receiver) as it is, from the restart it makes until power off or `{"divert": ""}` (another restart); the map card is left alone meanwhile, and `GET /api/status` says where (`divert`) |
| `POST /api/bench` | for measuring: `{"runs": 3}` decodes and draws every tile of the map shown, each `runs` times after a first, once the old way (0.9.0) and once with each quickening added in turn (a filter table, PNGdec, opaque laying, lean decoding, JPEGs straight into the frame, the decoders in internal RAM when there is room); the glass keeps its last map meanwhile. `GET /api/bench` says whether it is `busy` and gives the `result`: the map, its tiles, whether JPEGs use the S3's vector instructions (`jpeg_simd`), and for each way the median `png_ms`, `jpeg_ms`, `lay_ms` and `map_ms` for the whole map |
| `POST /api/debug` | measurement only, never stored: `{"planes": false}` hides the planes while the map and the beam go on |

```sh
curl -X POST http://<screen>/api/settings -H 'Content-Type: application/json' \
  -d '{"lat": 51.4700, "lon": -0.4543, "range_nm": 15, "map": "dark_gray"}'
```

The page and the API are open to anyone on your network, as an ESPHome device's own web server is. The test endpoints
(`/api/touch`, `/api/ui`, `/api/test`, `/api/bench`, and `{"source"}` and the network changes above) are left out of a
build with `test_api: false` in the `skyscope:` block.

## Data sources

Everything the screen shows comes from free public services. Please keep to their terms; the defaults do.

| Source | What | Terms |
|---|---|---|
| [adsb.fi](https://adsb.fi) | aircraft (the default), and a plane followed looked up | free for personal, non-commercial use; at most one request a second (lookups and reports kept 1.1 s apart) |
| [adsbdb](https://www.adsbdb.com) | a followed plane's route, only in a build with `routes: true` | free; its route data (David Taylor, Jim Mason) may not be copied or published without their leave: asked for each flight followed, kept nowhere, and left out of the releases |
| [adsb.lol](https://adsb.lol) | aircraft, and a plane followed looked up | open data, ODbL; no key yet (it plans to ask feeders for one); limits that change with its load |
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
line and the page count down to the next try. Tiles are cached in memory for a day (up to 3 MB, as they came from their
server, the least recently drawn going first; fewer on a board short of PSRAM, the cache always leaving 1 MB free), and
a tile is asked for only when a map needs one it does not hold. Each map is drawn from its tiles, one at a time, so a
map from the cache still fills in over a second or two; the last map drawn whole is also kept as it was drawn, so going
back to it (a swipe back, a preview cancelled) shows it whole at once. `/api/status` counts both (`tile_cache`), and the
map card's own (`card`), and how long the map takes (`tile_times`, below). With a [map card](#the-map-card), tiles are
kept on it too, for as long as each map's terms allow. The airspace is asked once for each place and range while it is
switched on; a request that fails waits 30 seconds, then twice as long each time, up to 15 minutes, since the FAA's
server is shared by everyone who uses it.

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
    and the mock (`tests/test_mock_feed.py`) that the tests on a real screen use: its receiver, update server and
    stand-ins for the public services.
  - `make test-page`: the web page's script (`tests/test_page.mjs`, Node 18 or newer): the airport search's query, the
    firmware section's words, and that the whole script parses.
- With a screen on the network (`tests/device_test.py`, Python's standard library only; every setting it changes is
  put back, and it prints nothing of the screen's place). Every one of these but `test-esp32-public` runs against a
  mock this machine serves (`tests/mock_feed.py`, port 8095, or `PORT=`: screens tested at once each need their own)
  and asks nothing of a public service, so a run goes the
  same way every time and on every board: the screen is sent to the mock (it restarts), every request it makes of a
  public service goes there, and it is sent back at the end (another restart). The mock stands in for the ADS-B exchanges, every map's
  tiles, the FAA's airspace and airports, the weather, the weather radar, place search and the update server
  (`tests/mock_services.py` says what each answers, and the host tests read each answer with the screen's own code);
  a run checks that it answered every request. These need a firmware built with the test API (`test_api: true`, the
  default), and the screen must reach this machine: under WSL, set `networkingMode=mirrored` in `.wslconfig` and
  allow inbound connections to it (see below).
  - `make test-esp32 DEVICE=<address>`: the page, settings (round trip, wrong values refused), status and memory,
    aircraft, the mock's airspace read exactly, and touches through `/api/touch` (the menu opened, stepped, cancelled
    and saved, paged, recoloured; a range typed on its keyboard; the Wi-Fi list; Check and Install's second tap; the
    known Wi-Fi networks (a made-up one moved and forgotten, never joined); a plane's card; the address; a swipe
    between maps and back; the weather page and its tabs; the weather radar and its loop; a drag on the brightness
    strip).
  - `make test-esp32-full DEVICE=<address>`: also every map drawn whole without a tile error, each from its own
    server, a restart that keeps the settings, and the screen going off and waking at a tap that does nothing else.
  - `make test-esp32-mock DEVICE=<address>`: the mock as a receiver and as the update server. The receiver: the planes
    it lists kept and placed where they should be, the filters, the range and the card; no planes, a moving one, 600,
    and 4500 in 600 KB; entries no receiver should send (wrong types, duplicates, UTF-8 past a field's length); every
    failure a feed can have (refusals, a pace, server errors, answers cut short, broken, too big or not JSON, too
    slow, none), each waited out as long as it should be; answers chunked or redirected; an address that cannot be
    found, then corrected; falling back to an exchange and back. The update server: a release offered, its failures
    said, and installs that must fail (no image, cut short, a wrong digest, not an image) leaving the firmware as it
    is. With `INSTALL=.esphome/build/skyscope-<board>/build/firmware.ota.bin`, that build installed from the mock.
  - `make test-esp32-soak DEVICE=<address> SOAK=60 CHURN=2`: memory sampled for an hour under the mock's 80 planes of
    every kind circling the test place (each symbol through every heading, two planes watched), the view changed every
    two minutes (every map, near and far, dark and light), failing if memory keeps falling, if it is lower each time
    the same view comes round, if the screen restarts, or if a record that grows as the screen runs passes its bound
    (the plane symbols are held to a megabyte).
- `make test-esp32-public DEVICE=<address>`: the public services themselves, which the mock stands in for, each
  asked by the screen at a test place (New York JFK): adsb.fi and adsb.lol answer with planes, the published release
  is found, the FAA's airspace and runways are read, every map is drawn whole, the weather radar's scan and loop come,
  and the weather page fills in for KJFK. Run it to know the services still answer as the screen expects; it is not
  repeatable (the sky and the services change), and needs no test API but for the weather page.

### A screen reaching a test server under WSL

The mock runs on the machine that runs the tests, and the screen has to reach it. WSL's default network is a
private one behind Windows, which nothing on the network reaches. In `%UserProfile%\.wslconfig`:

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

## License

MIT, see LICENSE. A firmware image built from it includes ESPHome's GPL-3.0 runtime, so a compiled firmware you
give to others comes under the GPL-3.0 (see NOTICE).
