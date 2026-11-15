# What the screen shows

Pictures taken from the screens themselves (`tools/screenshots.py`, which asks a screen for what its glass shows: `GET /api/screenshot`). The maps, airspace and weather are the real ones; the planes are made up, so no real flight is shown, and the glass credits them as **simulated**. Back to the [README](../README.md).

[Waveshare ESP32-P4 4C](#waveshare-esp32-p4-4c) · [Waveshare AMOLED 2.41](#waveshare-amoled-241)

## Waveshare ESP32-P4 4C

Pictures of the round 4-inch 720 × 720 screen, with its own radio. Its planes are those its own radio hears (**simulated radio**), but for a plane's card and a plane followed, which show what a receiver adds: the type and registration.

<table>
<tr>
<td width="33%" valign="top"><img src="screens/p4/splash.png" alt="At the start" width="100%"><br><b>At the start</b><br>The emblem, a radar scope whose beam is a propeller, turning to rest over the screen's name and version; on the round screen the scope's rim is the edge of the glass.</td>
<td width="33%" valign="top"><img src="screens/p4/streets-light.png" alt="Streets, light" width="100%"><br><b>Streets, light</b><br>Esri's light gray map with range rings and the compass rose, as a new screen starts. Arrivals line up on the final approach; each plane is drawn as what it is.</td>
<td width="33%" valign="top"><img src="screens/p4/locate.png" alt="Where is the screen?" width="100%"><br><b>Where is the screen?</b><br>Locate, under the place's field in the menu: where the planes the screen's own radio hears put it, and the town its internet address is in (asked only then); neither is used until it is picked.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/streets-dark.png" alt="Streets, dark, with airspace" width="100%"><br><b>Streets, dark, with airspace</b><br>The same map dark, with the FAA's Class B, C and D airspace and the runways' extended centerlines.</td>
<td width="33%" valign="top"><img src="screens/p4/card.png" alt="A plane's card" width="100%"><br><b>A plane's card</b><br>Tap a plane: its type, registration, operator, altitude with the climb or descent, speed and squawk, its route (drawn on the map), which way to look for it, and its photo with a QR code to the photo's page. Here NASA's 747 with Enterprise on its back, circling the Statue of Liberty, and a NASA photo standing in for Planespotters.net's.</td>
<td width="33%" valign="top"><img src="screens/p4/follow.png" alt="Following a plane" width="100%"><br><b>Following a plane</b><br>The map moves with a plane followed, at a range for its speed, with its track since it was found and keys down the left edge for its route and the range. NASA 905 over the harbor on Esri's imagery.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/tour.png" alt="The tour" width="100%"><br><b>The tour</b><br>A new screen's first map comes with a tour of what a finger does, one touch at a time.</td>
<td width="33%" valign="top"><img src="screens/p4/weather-page.png" alt="Aviation weather" width="100%"><br><b>Aviation weather</b><br>A swipe up from the bottom edge: the station's METAR, decoded, with its flight category in color; the TAF, MOS and advisories are a tab away.</td>
<td width="33%" valign="top"><img src="screens/p4/sectional.png" alt="VFR sectional" width="100%"><br><b>VFR sectional</b><br>The FAA's sectional chart around San Francisco, with the Class B shelves over it.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/ifr-low-dark.png" alt="IFR low, dark, with data blocks" width="100%"><br><b>IFR low, dark, with data blocks</b><br>The IFR enroute low chart made dark (its paper and ink turned around, its colors kept), and every label a controller's data block.</td>
<td width="33%" valign="top"><img src="screens/p4/radar.png" alt="Radar scope" width="100%"><br><b>Radar scope</b><br>The scope drawn on the screen itself, nothing downloaded: its glow, bearing lines and the beam painting the planes as it passes, with Atlanta's airspace in the phosphor.</td>
<td width="33%" valign="top"><img src="screens/p4/phosphor.png" alt="The phosphor in any color" width="100%"><br><b>The phosphor in any color</b><br>The menu's color picker: a swatch for each named phosphor, a slider for each of red, green and blue, and a small scope lit in the color.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/radar-bare.png" alt="A controller's bare map" width="100%"><br><b>A controller's bare map</b><br>The scope switched off: the planes, data blocks and airspace alone on the dark ground, in amber.</td>
<td width="33%" valign="top"><img src="screens/p4/satellite.png" alt="Satellite" width="100%"><br><b>Satellite</b><br>Esri's imagery of Las Vegas with its place names over it.</td>
<td width="33%" valign="top"><img src="screens/p4/topo.png" alt="USGS topo" width="100%"><br><b>USGS topo</b><br>The USGS topographic map around Salt Lake City, light.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/ifr-high.png" alt="IFR high enroute" width="100%"><br><b>IFR high enroute</b><br>The IFR high enroute chart at 80 NM, the jets passing over Denver.</td>
<td width="33%" valign="top"><img src="screens/p4/alert.png" alt="An alert" width="100%"><br><b>An alert</b><br>A plane squawking 7700 in range shows along the top of the glass, its edge in the emergency's red; a tap on it opens the plane's card. A watched plane coming into range, or one passing close by, shows the same way.</td>
<td width="33%" valign="top"><img src="screens/p4/osm.png" alt="OpenStreetMap, dark" width="100%"><br><b>OpenStreetMap, dark</b><br>OpenStreetMap around Heathrow, drawn dark with its hues kept, and a watched plane in a color of its own.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/swipe.png" alt="Another map with a swipe" width="100%"><br><b>Another map with a swipe</b><br>A swipe along the bottom edge goes to the next map, its name shown large with a dot for each; nothing is downloaded until the swipes stop.</td>
<td width="33%" valign="top"><img src="screens/p4/weather-radar.png" alt="Weather radar" width="100%"><br><b>Weather radar</b><br>The NEXRAD composite over the dark streets map, where it is raining now.</td>
<td width="33%" valign="top"><img src="screens/p4/today.png" alt="Today's planes" width="100%"><br><b>Today's planes</b><br>A swipe down from the top edge: the planes seen in range today, in range now and the most at once, a bar for each hour, the day's records and its most common types and operators.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/p4/menu.png" alt="The menu" width="100%"><br><b>The menu</b><br>A long press opens a menu with every setting the web page has; a tap on a value types it on a keyboard on the glass.</td>
<td width="33%" valign="top"><img src="screens/p4/map-card.png" alt="The map card" width="100%"><br><b>The map card</b><br>With a microSD card in, every map tile is kept on it. The menu shows the card, its space and what the screen keeps on it, map by map.</td>
</tr>
</table>

## Waveshare AMOLED 2.41

Pictures of the 450 × 600 AMOLED.

<table>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/splash.png" alt="At the start" width="100%"><br><b>At the start</b><br>The emblem, a radar scope whose beam is a propeller, turning to rest over the screen's name and version while it joins its network and fetches its map.</td>
<td width="33%" valign="top"><img src="screens/amoled/streets-light.png" alt="Streets, light" width="100%"><br><b>Streets, light</b><br>Esri's light gray map with range rings and the compass rose, as a new screen starts. Arrivals line up on the final approach; each plane is drawn as what it is.</td>
<td width="33%" valign="top"><img src="screens/amoled/locate.png" alt="Where is the screen?" width="100%"><br><b>Where is the screen?</b><br>Locate, under the place's field in the menu, offers the town the screen's internet address is in (asked only then); neither is used until it is picked.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/streets-dark.png" alt="Streets, dark, with airspace" width="100%"><br><b>Streets, dark, with airspace</b><br>The same map dark, with the FAA's Class B, C and D airspace and the runways' extended centerlines.</td>
<td width="33%" valign="top"><img src="screens/amoled/card.png" alt="A plane's card" width="100%"><br><b>A plane's card</b><br>Tap a plane: its type, registration, operator, altitude with the climb or descent, speed and squawk, its route (drawn on the map), which way to look for it, and its photo with a QR code to the photo's page. Here NASA's 747 with Enterprise on its back, circling the Statue of Liberty, and a NASA photo standing in for Planespotters.net's.</td>
<td width="33%" valign="top"><img src="screens/amoled/follow.png" alt="Following a plane" width="100%"><br><b>Following a plane</b><br>The map moves with a plane followed, at a range for its speed, with its track since it was found and keys down the left edge for its route and the range. NASA 905 over the harbor on Esri's imagery.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/tour.png" alt="The tour" width="100%"><br><b>The tour</b><br>A new screen's first map comes with a tour of what a finger does, one touch at a time.</td>
<td width="33%" valign="top"><img src="screens/amoled/weather-page.png" alt="Aviation weather" width="100%"><br><b>Aviation weather</b><br>A swipe up from the bottom edge: the station's METAR, decoded, with its flight category in color; the TAF, MOS and advisories are a tab away.</td>
<td width="33%" valign="top"><img src="screens/amoled/sectional.png" alt="VFR sectional" width="100%"><br><b>VFR sectional</b><br>The FAA's sectional chart around San Francisco, with the Class B shelves over it.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/ifr-low-dark.png" alt="IFR low, dark, with data blocks" width="100%"><br><b>IFR low, dark, with data blocks</b><br>The IFR enroute low chart made dark (its paper and ink turned around, its colors kept), and every label a controller's data block.</td>
<td width="33%" valign="top"><img src="screens/amoled/radar.png" alt="Radar scope" width="100%"><br><b>Radar scope</b><br>The scope drawn on the screen itself, nothing downloaded: its glow, bearing lines and the beam painting the planes as it passes, with Atlanta's airspace in the phosphor.</td>
<td width="33%" valign="top"><img src="screens/amoled/phosphor.png" alt="The phosphor in any color" width="100%"><br><b>The phosphor in any color</b><br>The menu's color picker: a swatch for each named phosphor, a slider for each of red, green and blue, and a small scope lit in the color.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/radar-bare.png" alt="A controller's bare map" width="100%"><br><b>A controller's bare map</b><br>The scope switched off: the planes, data blocks and airspace alone on the dark ground, in amber.</td>
<td width="33%" valign="top"><img src="screens/amoled/satellite.png" alt="Satellite" width="100%"><br><b>Satellite</b><br>Esri's imagery of Las Vegas with its place names over it.</td>
<td width="33%" valign="top"><img src="screens/amoled/topo.png" alt="USGS topo" width="100%"><br><b>USGS topo</b><br>The USGS topographic map around Salt Lake City, light.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/ifr-high.png" alt="IFR high enroute" width="100%"><br><b>IFR high enroute</b><br>The IFR high enroute chart at 80 NM, the jets passing over Denver.</td>
<td width="33%" valign="top"><img src="screens/amoled/alert.png" alt="An alert" width="100%"><br><b>An alert</b><br>A plane squawking 7700 in range shows along the top of the glass, its edge in the emergency's red; a tap on it opens the plane's card. A watched plane coming into range, or one passing close by, shows the same way.</td>
<td width="33%" valign="top"><img src="screens/amoled/osm.png" alt="OpenStreetMap, dark" width="100%"><br><b>OpenStreetMap, dark</b><br>OpenStreetMap around Heathrow, drawn dark with its hues kept, and a watched plane in a color of its own.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/swipe.png" alt="Another map with a swipe" width="100%"><br><b>Another map with a swipe</b><br>A swipe along the bottom edge goes to the next map, its name shown large with a dot for each; nothing is downloaded until the swipes stop.</td>
<td width="33%" valign="top"><img src="screens/amoled/weather-radar.png" alt="Weather radar" width="100%"><br><b>Weather radar</b><br>The NEXRAD composite over the dark streets map, where it is raining now.</td>
<td width="33%" valign="top"><img src="screens/amoled/today.png" alt="Today's planes" width="100%"><br><b>Today's planes</b><br>A swipe down from the top edge: the planes seen in range today, in range now and the most at once, a bar for each hour, the day's records and its most common types and operators.</td>
</tr>
<tr>
<td width="33%" valign="top"><img src="screens/amoled/menu.png" alt="The menu" width="100%"><br><b>The menu</b><br>A long press opens a menu with every setting the web page has; a tap on a value types it on a keyboard on the glass.</td>
</tr>
</table>
