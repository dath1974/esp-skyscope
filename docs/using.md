# Using the screen

What the screen shows and how to work it from the glass. Every setting here is also on the screen's web page. Back to
the [README](../README.md).

- [Maps](#maps)
- [Aircraft](#aircraft)
- [On the glass](#on-the-glass)
- [The menu](#the-menu)
- [Zoom and pan](#zoom-and-pan)
- [Changing the map with a swipe](#changing-the-map-with-a-swipe)
- [Following a plane](#following-a-plane)
- [Aviation weather](#aviation-weather)
- [Alerts and the day's tally](#alerts-and-the-days-tally)
- [Home Assistant](#home-assistant)
- [The map card](#the-map-card)

## Maps

- **Radar**: a scope drawn on the screen itself, nothing downloaded, with its glow, bearing lines, range rings and a
  beam that paints the planes as it passes, in a phosphor of your choice (green, amber, blue or white). The scope can
  be switched off, leaving the planes and the airspace alone on the dark ground, like a controller's bare map.
- **Streets** (Esri's gray maps), **OpenStreetMap**, **satellite** (Esri), the **USGS topographic** map, and the FAA's
  **VFR sectional**, **IFR low** and **IFR high** charts.
- **Custom**: any 256-pixel PNG or JPEG tile server of your own.

One **appearance** switch makes every map dark or light. A map without a dark version of its own is darkened in a way
made for it: the IFR charts swap their paper and ink and keep their colors, the sectional is dimmed, and topo and
OpenStreetMap are inverted with their hues kept. Place names can go over the streets and the satellite map, and range
rings and a compass rose over any map. A new screen starts on the light streets map.

**Airspace** (optional, United States only): the FAA's Class B, C and D outlines and runways, with dashed centerlines
10 NM out from the long runways of towered airports. On the radar they are drawn in the phosphor, over a map in the
charts' blue and magenta. They are built from the FAA's public data, not the maps real ATC scopes use, which the FAA
does not publish.

## Aircraft

Planes come from adsb.fi or adsb.lol, a receiver on your network (**Network receiver**: its `aircraft.json`, by the
address you give it), or on the P4 board the screen's own radio, an RTL-SDR stick in its USB port (**USB radio**;
[docs/hardware.md](hardware.md#the-screens-own-radio-esp32-p4)). If the chosen source is unavailable the screen can
fall back to another. The credit at the bottom of the glass names the source, and the status line says how many
seconds ago the planes last came in (orange once they are well out of date). A receiver that marks its planes as
simulated is credited as **simulated**, so made-up planes are never mistaken for real ones.

- **Markers**: each plane is drawn as what it is (airliner, wide-body, four-engined, business jet, turboprop, light
  aircraft, helicopter, fast jet, glider, balloon, airship, drone, ground vehicle), or every plane as one plane shape,
  an arrow or a dot. Planes are colored by altitude.
- **Labels**: the callsign, the altitude, or the type and speed as well, or a controller's data block (`Scope`): the
  callsign over the altitude, ground speed in tens of knots and the FAA's wake category, as in `350 43 B`. Altitudes
  are in hundreds of feet, as on a controller's scope (`025`, `350`), with an arrow for a climb or descent of 200 ft a
  minute or more.
- **Tracks** of the last few seconds, planes gliding smoothly between reports (optional), and the radar beam.
- **No ID**: a plane the exchange knows only by an address it made up (written `~2ba403` in its data), not by its
  transponder. In the United States these are mostly TIS-B: a plane without ADS-B that the FAA's radar sees and
  broadcasts by a track number. It sends no callsign, so its card has no photo and no Follow key.
- **Watched aircraft** (by registration, callsign or ICAO address) always show, in a color of their own.
- **Filters**: altitude limits, ground traffic on or off, and the most planes shown.

## On the glass

- **Tap a plane** for its card: type, registration, altitude with its climb or descent rate, speed, heading,
  operator and squawk, and its route where [adsb.lol](https://adsb.lol) knows one (`KJFK to EGLL`, drawn on the map as
  a great circle). A route is shown only when the plane is where it would be on that route, since a flight number is
  sometimes moved to another. Where adsb.lol has none, the route comes from [hexdb.io](https://hexdb.io), in a build
  with `hexdb_routes: true` (the default for now; see the README's data sources). Within 60 NM the card says which way to look for it from your place: `NE, 25° up, 8.4 NM`,
  and for one that will pass within 5 NM in the next half hour, `passes 1.2 NM off in 3 min` (or `overhead in 3 min`).
  The angle takes the ground at sea level, as the planes' altitudes are. Its **Follow** key follows it.

  With **Photos** on (the menu's Aircraft group, or the web page), the card also shows the plane's latest photo from
  [Planespotters.net](https://www.planespotters.net), with its photographer under it and a QR code beside it: scan it
  with a phone to open the photo's page, full size. That is what Planespotters.net asks of a screen that shows its
  photos. The photo is fetched as the card opens and let go when it closes; it is never kept on the map card.
- **Tap the map** to see the screen's address and range in the status line.
- **Drag up or down along the right edge** to set the brightness.
- **Long-press the map** for [the menu](#the-menu).
- **Pinch or drag** to [zoom and pan](#zoom-and-pan).
- **Swipe along the bottom edge** to [change the map](#changing-the-map-with-a-swipe), or **up from it** for
  [the weather](#aviation-weather).

**Sleep.** The screen can turn itself off after a while without a touch, and at a bedtime every night, coming back on
at a set time or at a tap. While it is off it fetches nothing.

**Rotation** is in the settings (the screen restarts to turn). 0° is the panel's own way up and the cheapest to draw,
since every other angle turns each frame in software. On the AMOLED 2.41 and the LCD 2.8B, 0° is portrait and 90°
landscape; on the round 4C, 0° keeps the USB ports at the bottom.

## The menu

A long press on the map (or on the setup screen) opens the menu. Its rows are grouped as on the web page (Where, Map,
Aircraft, Source, Screen) and hide where they do not apply. A value between two arrows steps through common choices;
a tap on a value in a field opens the keyboard. Changes show in the accent color and wait for **Save**; **Cancel**
drops them. A value the screen cannot take is refused, with what it accepts shown under it.

- **The keyboard** is laid out as on a phone: the up arrow for capitals, **?123** for numbers and punctuation, arrows
  to move the cursor. Numbers and times get a pad of digits.
- **Place** finds an airport by its code (KJFK, JFK) or name, or anything else through OpenStreetMap, when you press
  **Search**. Pick one of the results, or type a latitude and longitude instead. The screen writes Latin letters only,
  so a place named only in another script is left out.
- **Follow** takes a plane to follow ([below](#following-a-plane)); empty stops.
- **Watched** takes aircraft with a comma between: `N12345 red, BAW123 blue` (red if no color is given).
- **Wi-Fi** shows the network the screen is on. A tap lists the networks it knows (up to eight, first choice first)
  and the others in reach, and **Other network** for a hidden one. Known networks can be moved up or down or
  forgotten. Joining one makes it the first choice and restarts the screen to join it. At every start the screen joins
  the first known network in reach, so it can be moved between places. With none in reach it opens **Skyscope Setup**
  after a minute.

## Zoom and pan

Two fingers zoom (spread or pinch) and pan (move together), as on a phone; one finger dragged across the map pans it
too. While your fingers move, the screen stretches and shifts the picture it has, so nothing is downloaded. Three
seconds after they lift, it draws the new view properly.

A bar along the bottom shows the range and a 60-second countdown, with **Cancel** and **Save**. **Save** keeps the view
as the place and range; **Cancel**, or the countdown running out, goes back.

The range can be anything from 1 to 5000 NM. The exchanges answer for 250 NM around a point at most, so beyond that
only the planes within 250 NM are shown, and the airspace is not. Maps zoom in whole steps, so a pinched map may settle
a little larger or smaller than your fingers left it; the range rings and the planes are at the exact range.

While following a plane, a pinch or drag works the same way: **Save** keeps the range, and the view goes on following.

## Changing the map with a swipe

Swipe left or right along the bottom edge of the map (its lowest 6 mm) for the next or previous map, in the menu's
order. The map's name shows large in the middle with a row of dots, one for each map. Nothing is downloaded until the
swipes have stopped for two seconds, so going through several costs only the last one. A swipe back undoes it.

## Following a plane

Type a plane in the menu's **Follow** field or the web page's **Follow a plane**, or tap a plane and press **Follow**
on its card (**Stop following** there, or an empty field, stops). Name it by its registration (`N7209W`, `G-EUPT`), its
callsign (`BAW286`) or the flight number on a ticket (`BA 286`). Flight numbers are turned into callsigns for the larger
airlines only; for the others, type the callsign. The screen finds the plane anywhere in the world. One not in the air
yet is looked for again every two minutes, with `Looking for BA286` in the status line. Following carries on through a
restart or the screen going to sleep.

- **The map moves with it.** When the plane nears an edge, the view steps on so the plane sits a little behind the
  middle with most of the glass ahead of it. The screen draws each next view ahead of time, so the step is quick. Your
  place is not changed, and the view goes back to it when following stops.
- **Its range** is chosen from its speed, so the map steps on about once a minute: 1 NM for a plane at 60 kt, 4 NM at
  250 kt, 7 or 8 NM for an airliner at cruise. Keys down the left edge put it back at that range (**Plane**), at the
  range the screen had before (**Own range**), or show the whole route (**Route**, once its route is known).
- **The planes around it** are shown instead of those around your place. The followed plane is ringed (in blue,
  unless it is watched in another color) and has a label setting of its own (**Its label**, a data block by default).
- **Its track** is its whole flight since it was found, or as many minutes as **Its track** is set to, up to a day.
- **US charts** (the sectional, the IFR charts, topo) give way to the streets map while the view is outside the United
  States. Range rings are not drawn while following, since they measure from your place.
- **Out of reach**: the exchanges hear only what volunteers' receivers on the ground hear, so a plane over an ocean is
  lost to them. After a minute without a report the screen carries it on along its course at its last speed, drawn
  faint and labeled `estimated`, until it is heard again.
- **Its route**: the airports it flies from and to, from adsb.lol (or hexdb.io), with the great circle between them. This is not a
  filed flight plan, only the rough way a long flight goes.
- **Following stops by itself** once the plane has been on the ground for ten minutes after flying, or out of reach for
  twelve hours. A plane at its gate before it flies is waited for.

## Aviation weather

A swipe up from the bottom edge (or **Show the weather** in the menu's Weather group) opens the weather page, for the
station set in the settings or, with none set, the nearest one to the place. Its title shows the flight category in
the charts' colors (VFR green, MVFR blue, IFR red, LIFR magenta). Its tabs:

- **METAR**: the latest, as sent and in words.
- **TAF**: a line for each change.
- **MOS**: the GFS MOS, three hours a line (United States stations only).
- **Area**: the SIGMETs and G-AIRMETs within the range (at least 50 NM), and the pilot reports of the last two hours.

The page fetches the weather only while it is open: every ten minutes, and at **Refresh**. Not for flight planning.

**Radar** (in the menu's Weather group, or on the web page) draws the NWS's NEXRAD composite over any map of tiles,
United States only, refreshed every five minutes. **Radar loop** plays the last hour.

## Alerts and the day's tally

A watched plane coming into range, or a plane in range squawking an emergency (7500, 7600 or 7700), shows along the top
of the glass for 20 seconds: `N12345 is in range, 8.4 NM NE`. A tap on it opens the plane's card. A watched plane is
told of again once it has been out of range for ten minutes.

The screen counts the planes within the range of the place (up to 250 NM), from midnight by its clock: how many, by
the hour, the commonest types and operators, the most at once, and the day's nearest, highest and fastest. Only the
planes around the place count, so none are counted while a plane is followed elsewhere. The web page shows the day
under **Today in range**, with the days before, and keeps the latest 400 visits as a log to download (CSV). The tally
is kept over a restart; with a [map card](#the-map-card) in, every visit is also written to it, a file a day in its
`log` folder.

## Home Assistant

The firmware carries ESPHome's API, so Home Assistant finds the screen on the network by itself. Add it there and Home
Assistant gives it a key of its own. It then has:

- **Planes in range**, **Planes today**, **Nearest plane** (its name), **Nearest plane distance** and **altitude**,
  and **Followed plane** (looking, live, lost, landed or gave_up).
- **Alert**, an event of type `watched` or `emergency` for each alert, with its words in **Last alert**: an
  automation can send them to a phone.
- **Screen**, to turn the screen off and on, and **Brightness**.
- **Firmware**, the update the web page offers.

A screen never added to Home Assistant carries on as before. A firmware of your own without any of this: remove the
`home_assistant:` line from `skyscope.yaml`'s packages.

## The map card

Every board has a microSD slot. With a FAT32 card in it, every map tile the screen fetches is kept on the card, so a
map seen once comes back without downloading it again, even after a restart. Each map's tiles are kept as long as its
terms allow, and fetched again after that. The screen keeps its tiles in a `tiles` folder and its
[log](#alerts-and-the-days-tally) in a `log` folder, and leaves everything else on the card alone.

On the web page, **Map card** shows the card's free space and can **download an area ahead of time**: the sectional,
the IFR charts or the USGS map, around the place, up to 250 NM, every zoom down to the one chosen. The page estimates
the size and time first (a 100 NM sectional is about 950 tiles, 52 MB of the card and a quarter of an hour). The
download carries on while the screen is off and after a restart. Only these maps can be downloaded ahead: they are
works of the United States government, free to copy, while OpenStreetMap and Esri ask that their tiles are fetched
only as they are looked at.

**Format the card** formats it FAT32 whatever its size, with no computer (cards over 32 GB come as exFAT); it erases
everything on the card. **Clear the screen's tiles** deletes only the screen's own, a few at a time in the background.
