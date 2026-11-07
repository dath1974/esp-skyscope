#!/usr/bin/env python3
"""The pictures in docs/screens.md: a real screen photographed through its own API, over the real maps with a sky of
made-up planes.

    tools/screenshots.py <address> [--out docs/screens] [--only NAME,...] [--port 8095]
    tools/screenshots.py --list | --page

Each picture is a scene: a place (a public airport, never the screen's own), a map and its settings, and sometimes a
touch (a plane's card, the menu, the weather page). The screen asks the real tile servers, the FAA's airspace and the
weather as it always does; only its planes come from this machine, as a receiver of its own (tests/mock_feed.py with a
sky of its own: airliners on the runway's final approach and climbing out, jets passing high over, light aircraft,
helicopters and business jets, all moving), so no real flight is shown and every run has the same kind of traffic.
At the end it writes screens.md beside the pictures' folder (docs/screens.md): every picture there in a grid, each
with what it shows; --page writes it alone.

It needs the test API (`test_api: true`: GET /api/screenshot, /api/touch, /api/ui) and the screen to reach this machine
(WSL: networkingMode=mirrored). Every setting it changes is put back at the end, as tests/device_test.py does (and
`tests/device_test.py <address> --restore` puts them back if it could not). It never prints the screen's own place.

The FAA's runways near each airport are asked once and kept in tests/build/runways-<code>.json, so the approaches
line up with the runways the screen draws. The weather radar's scene goes where the Aviation Weather Center's METARs
report rain or snow now, of a list of large airports (once a run, one request).

Python's standard library only.
"""

import argparse
import json
import math
import random
import sys
import time
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import device_test  # noqa: E402
import mock_feed  # noqa: E402
from device_test import Glass, Screen, wait_for  # noqa: E402

AGENT = "esp-skyscope screenshots (https://github.com/dath1974/esp-skyscope)"

# ---------------------------------------------------------------- places

# Public airports: (latitude, longitude, field elevation in feet, the direction they land today, roughly, in degrees
# true; the runway nearest it is used, and None means no runways asked (outside the United States).
PLACES = {
    "KJFK": (40.6413, -73.7781, 13, 300, "us"),
    "KSFO": (37.6189, -122.3750, 13, 280, "us"),
    "KORD": (41.9786, -87.9048, 672, 270, "us"),
    "KDEN": (39.8561, -104.6737, 5434, 170, "us"),
    "KSLC": (40.7884, -111.9778, 4227, 340, "us"),
    "KATL": (33.6407, -84.4277, 1026, 270, "us"),
    "KLAS": (36.0840, -115.1537, 2181, 260, "us"),
    "KSEA": (47.4502, -122.3088, 433, 160, "us"),
    "EGLL": (51.4700, -0.4543, 83, 270, "uk"),
}
# Where to look for rain for the weather radar's scene: large airports across the United States.
RAIN_CANDIDATES = ["KATL", "KBOS", "KBNA", "KCLT", "KDCA", "KDEN", "KDFW", "KDTW", "KIAH", "KJFK", "KLAX", "KMCI",
                   "KMCO", "KMEM", "KMIA", "KMSP", "KMSY", "KORD", "KPDX", "KPHL", "KPHX", "KPIT", "KSEA", "KSFO",
                   "KSLC", "KSTL", "KTPA", "KOKC", "KOMA", "KBUF", "KCLE", "KCVG", "KIND", "KJAX", "KRDU", "KSAT"]

NM_PER_DEGREE = 60.0


def bearing_offset(lat, lon, bearing, nm):
    """The place `nm` away from lat, lon towards `bearing` (degrees true)."""
    b = math.radians(bearing)
    return mock_feed.offset(lat, lon, nm * math.cos(b), nm * math.sin(b))


def get_json(url: str, timeout: float = 20):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as r:
        return json.loads(r.read())


def runway_for(code: str):
    """The landing runway at `code`: (threshold lat, lon, the direction it lands) or None. Asked of the FAA (the
    screen's own query, airspace.h) once and kept; when the FAA is busy, tried again a minute later, twice."""
    lat, lon, _, flow, country = PLACES[code]
    if country != "us":
        return None
    kept = ROOT / "tests" / "build" / f"runways-{code}.json"
    if kept.exists():
        reply = json.loads(kept.read_text())
    else:
        box = f"{lon - 0.06:.3f},{lat - 0.05:.3f},{lon + 0.06:.3f},{lat + 0.05:.3f}"
        url = ("https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/Runways/FeatureServer/0/query?"
               "where=LENGTH%3E%3D7000&geometry=" + box + "&geometryType=esriGeometryEnvelope&inSR=4326"
               "&spatialRel=esriSpatialRelIntersects&outFields=AIRPORT_ID,LENGTH&outSR=4326&geometryPrecision=5&f=json")
        reply = None
        for attempt in range(3):
            try:
                reply = get_json(url)
            except OSError as e:
                print(f"  the FAA's runways for {code}: {e}")
            if reply and "features" in reply:
                break
            if attempt < 2:
                print(f"  the FAA is busy; its runways for {code} again in a minute")
                time.sleep(62)
        if not reply or "features" not in reply:
            return None
        kept.parent.mkdir(parents=True, exist_ok=True)
        kept.write_text(json.dumps(reply))
    best = None
    for f in reply["features"]:
        ring = (f.get("geometry") or {}).get("rings", [[]])[0]
        if len(ring) < 3:
            continue
        # The runway's two ends: the farthest two corners of its outline, each end the middle of its own two.
        pts = [(p[1], p[0]) for p in ring]
        far = max(((a, b) for a in pts for b in pts), key=lambda ab: dist_nm(*ab[0], *ab[1]))
        ends = []
        for e in far:
            near = sorted(pts, key=lambda p: dist_nm(*p, *e))[:2]
            ends.append(((near[0][0] + near[1][0]) / 2, (near[0][1] + near[1][1]) / 2))
        for start, stop in (ends, ends[::-1]):
            course = bearing_of(*start, *stop)
            off = abs((course - flow + 180) % 360 - 180)
            length = dist_nm(*start, *stop)
            score = off - length * 4  # the runway pointing the right way, the longer the better
            if off < 50 and (best is None or score < best[0]):
                best = (score, start[0], start[1], course)
    return best[1:] if best else None


def dist_nm(lat1, lon1, lat2, lon2):
    north = (lat2 - lat1) * NM_PER_DEGREE
    east = (lon2 - lon1) * NM_PER_DEGREE * math.cos(math.radians((lat1 + lat2) / 2))
    return math.hypot(north, east)


def bearing_of(lat1, lon1, lat2, lon2):
    north = (lat2 - lat1) * NM_PER_DEGREE
    east = (lon2 - lon1) * NM_PER_DEGREE * math.cos(math.radians((lat1 + lat2) / 2))
    return math.degrees(math.atan2(east, north)) % 360


def rainy_place():
    """Of RAIN_CANDIDATES, the one whose METAR reports the most weather now (thunder over rain or snow over
    drizzle), with its coordinates; None if none does."""
    try:
        reply = get_json("https://aviationweather.gov/api/data/metar?format=json&ids=" + ",".join(RAIN_CANDIDATES))
    except (OSError, ValueError) as e:
        print(f"  the METARs: {e}")
        return None
    weights = (("TS", 5), ("+", 2), ("RA", 3), ("SN", 3), ("SH", 1), ("DZ", 1))
    best = None
    for m in reply:
        wx = m.get("wxString") or ""
        score = sum(w for word, w in weights if word in wx)
        if score and (best is None or score > best[0]):
            best = (score, m["icaoId"], m["lat"], m["lon"], int((m.get("elev") or 0) * 3.28))
    return best[1:] if best else None


# ---------------------------------------------------------------- the made-up sky

US_AIRLINES = [("AAL", ["A321", "B738", "A319"]), ("DAL", ["A321", "B739", "A220"]), ("UAL", ["B39M", "B789", "A320"]),
               ("JBU", ["A320", "A321"]), ("SWA", ["B38M", "B737"]), ("SKW", ["E75L", "CRJ9"]), ("RPA", ["E75L"]),
               ("ASA", ["B739"]), ("FDX", ["B752", "B763"]), ("UPS", ["B744", "B763"]), ("NKS", ["A20N"])]
FAR_AIRLINES = [("BAW", ["B77W", "A35K"]), ("DLH", ["A359", "B748"]), ("AFR", ["B77W"]), ("UAE", ["A388"]),
                ("KLM", ["B789"]), ("JAL", ["B789"])]
UK_AIRLINES = [("BAW", ["A320", "A321", "B77W", "A35K"]), ("EZY", ["A320", "A20N"]), ("VIR", ["A359", "B789"]),
               ("RYR", ["B738"]), ("DLH", ["A320"]), ("KLM", ["B738"]), ("UAE", ["A388"]), ("AAL", ["B77W"]),
               ("SHT", ["A320"])]
DESCRIPTIONS = {
    "A319": "AIRBUS A-319", "A320": "AIRBUS A-320", "A321": "AIRBUS A-321", "A20N": "AIRBUS A-320NEO",
    "A220": "AIRBUS A220-100", "A359": "AIRBUS A-350-900", "A35K": "AIRBUS A-350-1000", "A388": "AIRBUS A-380-800",
    "B737": "BOEING 737-700", "B738": "BOEING 737-800", "B739": "BOEING 737-900", "B38M": "BOEING 737 MAX 8",
    "B39M": "BOEING 737 MAX 9", "B752": "BOEING 757-200", "B763": "BOEING 767-300", "B744": "BOEING 747-400",
    "B748": "BOEING 747-8", "B77W": "BOEING 777-300ER", "B789": "BOEING 787-9", "E75L": "EMBRAER ERJ-175",
    "CRJ9": "BOMBARDIER CRJ-900", "C172": "CESSNA 172 SKYHAWK", "SR22": "CIRRUS SR-22", "PA28": "PIPER PA-28",
    "PC12": "PILATUS PC-12", "GLF6": "GULFSTREAM G650", "C68A": "CESSNA CITATION LATITUDE", "CL35": "BOMBARDIER CL-350",
    "EC35": "AIRBUS HELICOPTERS EC-135", "B06": "BELL 206", "AS50": "AIRBUS HELICOPTERS AS-350",
}
HEAVIES = {"A359", "A35K", "A388", "B744", "B748", "B763", "B77W", "B789"}


class Sky:
    """Planes for one place, laid out once (the same each run) and flown by the clock."""

    def __init__(self, code: str, lat: float, lon: float, elevation: int, runway, range_nm: int, uk: bool):
        self.lat, self.lon, self.elevation = lat, lon, elevation
        self.started = time.time()
        rnd = random.Random(code)
        reach = max(12, min(range_nm, 60))
        airlines = UK_AIRLINES if uk else US_AIRLINES + (FAR_AIRLINES if code == "KJFK" else [])
        used = set()

        def callsign():
            while True:
                icao, types = rnd.choice(airlines)
                c = f"{icao}{rnd.randint(12, 2899)}"
                if c not in used:
                    used.add(c)
                    return c, rnd.choice(types)

        def registration():
            if uk:
                return "G-" + "".join(rnd.choice("ABCDEFGHJKLMNPRSTUVWXYZ") for _ in range(4))
            return f"N{rnd.randint(100, 999)}{rnd.choice('ABCDEFGHJKLMNPRSTUVWXYZ')}{rnd.choice('ABCDEFGHJKLMNPRSTUVWXYZ')}"

        def squawk():
            return "".join(str(rnd.randint(0, 7)) for _ in range(4)).replace("7700", "4217")

        self.planes = []
        n = 0

        def add(path, fields):
            nonlocal n
            n += 1
            self.planes.append((f"{0xa10000 + n * 16:06x}", path, fields))

        if runway:
            t_lat, t_lon, course = runway
        else:  # outside the FAA's data: the field and the direction of the flow
            t_lat, t_lon, course = lat, lon, PLACES[code][3]
        # Arrivals strung out down the final approach, three miles apart, and one joining it from the side.
        for i in range(4):
            flight, t = callsign()
            add(("final", t_lat, t_lon, course, 3 + i * 3.2 + rnd.random(), 150 + rnd.randint(0, 25)),
                {"flight": flight, "t": t, "r": registration(), "squawk": squawk(), "baro_rate": -768})
        for side in (rnd.choice((-1, 1)),):
            flight, t = callsign()
            add(("base", t_lat, t_lon, course, side, 24 + rnd.random() * 8, 210),
                {"flight": flight, "t": t, "r": registration(), "squawk": squawk(), "baro_rate": -1200})
        # Departures climbing out and turning away.
        for i, turn in enumerate((-45, 70, -80)):
            flight, t = callsign()
            add(("climb", t_lat, t_lon, course, turn, 2 + i * 6 + rnd.random() * 2, 250 + rnd.randint(0, 40)),
                {"flight": flight, "t": t, "r": registration(), "squawk": squawk(), "baro_rate": 2496})
        # Over the top, high and fast, in every direction.
        for i in range(4):
            flight, t = callsign()
            add(("line", rnd.uniform(-reach, reach) * 0.8, rnd.uniform(-reach, reach) * 0.8, rnd.uniform(0, 360),
                 430 + rnd.randint(0, 60), reach * 2.4, rnd.random(), 29000 + 2000 * rnd.randint(0, 6)),
                {"flight": flight, "t": t, "r": registration(), "squawk": squawk()})
        # Light aircraft, a turboprop, helicopters and business jets, lower down.
        for t, gs, alt in (("C172", 105, 2500), ("SR22", 165, 4500), ("PC12", 260, 9500), ("EC35", 120, 1200),
                           ("GLF6", 330, 12000)):
            heli = t in ("EC35", "B06", "AS50")
            near = reach * (0.35 if heli else 0.7)
            fields = {"t": t, "r": registration(), "squawk": "1200" if t in ("C172", "PA28") else squawk()}
            fields["flight"] = fields["r"].replace("-", "")
            if t in ("GLF6", "C68A", "CL35"):
                fields["flight"] = rnd.choice(["EJA", "LXJ", "XSR"]) + str(rnd.randint(100, 999))
            add(("line", rnd.uniform(-near, near), rnd.uniform(-near, near), rnd.uniform(0, 360), gs, near * 2.2,
                 rnd.random(), alt + elevation), fields)

    def aircraft(self, now: float = None):
        """Every plane where it is now (or at `now`)."""
        seconds = (time.time() if now is None else now) - self.started
        out = []
        for hex_, path, fields in self.planes:
            p = self.fly(path, seconds)
            if not p:
                continue
            lat, lon, alt, gs, track, generation = p
            category = "A5" if fields["t"] in HEAVIES else "A7" if fields["t"] in ("EC35", "B06", "AS50") else \
                "A1" if fields["t"] in ("C172", "SR22", "PA28") else "A3"
            plane = {"hex": f"{(int(hex_, 16) + generation) & 0xffffff:06x}", "lat": round(lat, 6),
                     "lon": round(lon, 6), "alt_baro": int(alt) // 25 * 25, "gs": gs, "track": round(track % 360, 1),
                     "seen_pos": 0.4, "category": category, "desc": DESCRIPTIONS.get(fields["t"], ""), **fields}
            plane["flight"] = f"{fields['flight']:<8}"
            out.append(plane)
        return out

    def fly(self, path, seconds):
        kind = path[0]
        if kind == "final":
            _, t_lat, t_lon, course, start, gs = path
            span = 20.0
            gone = start - gs * seconds / 3600 - 0.6  # down to just short of the threshold, then the next one
            generation = int(math.floor(-gone / span)) + 1 if gone < 0 else 0
            d = gone + generation * span + 0.6
            lat, lon = bearing_offset(t_lat, t_lon, course + 180, d)
            return lat, lon, self.elevation + d * 318, gs, course, generation
        if kind == "base":
            # A base leg at right angles to the final, eight miles out, turning onto it.
            _, t_lat, t_lon, course, side, start, gs = path
            span = 30.0
            travelled = (30 - start) + gs * seconds / 3600
            generation = int(travelled // span)
            s = travelled % span  # along the leg, from its start 22 NM out to the side
            join = 9.0
            out = 22 - s
            if out > 0:
                lat, lon = bearing_offset(*bearing_offset(t_lat, t_lon, course + 180, join), course + 90 * side, out)
                track = course - 90 * side
                alt = 3000 + out * 150
            else:
                lat, lon = bearing_offset(t_lat, t_lon, course + 180, max(join + out, 0.5))
                track = course
                alt = max(join + out, 0.5) * 318
            return lat, lon, self.elevation + alt, gs, track, generation
        if kind == "climb":
            _, t_lat, t_lon, course, turn, start, gs = path
            span = 32.0
            travelled = start + gs * seconds / 3600
            generation = int(travelled // span)
            s = travelled % span
            straight = min(s, 4)
            lat, lon = bearing_offset(t_lat, t_lon, course, straight)
            track = course
            if s > 4:
                track = course + turn
                lat, lon = bearing_offset(lat, lon, track, s - 4)
            return lat, lon, self.elevation + min(1000 + s * 650, 17000), gs, track, generation
        # A straight line through the area, `length` long, and round again (as another plane) when it leaves.
        _, north, east, track, gs, length, phase, alt = path
        travelled = phase * length + gs * seconds / 3600
        generation = int(travelled // length)
        s = travelled % length - length / 2
        lat, lon = mock_feed.offset(self.lat, self.lon, north, east)
        lat, lon = bearing_offset(lat, lon, track, s)
        return lat, lon, alt, gs, track, generation


# ---------------------------------------------------------------- the scenes

# What every scene starts from (as device_test's own, a new screen's settings but for the place and the planes).
COMMON = {"map_brightness": 100, "tile_scale": 0, "rings": True, "place_names": True, "airspace": False,
          "tint": "green", "scope": True, "sweep": True, "fallback": False, "refresh_s": 5, "alt_min": 0,
          "alt_max": 60000, "ground": False, "most": 40, "marker": "type", "label": "auto", "by_altitude": True,
          "glide": True, "track_s": 90, "watch": [], "follow": "", "metric": False, "brightness": 80,
          "sleep_min": 0, "bedtime": False, "weather_station": "", "radar_overlay": False, "radar_loop": False}

# name, place ("rain": where it rains now), what it shows (for docs/screens.md), settings, a touch after
SCENES = [
    ("streets-light", "KJFK", "Streets, light", "Esri's light gray map with range rings and the compass rose, as a new "
     "screen starts. Arrivals line up on the final approach; each plane is drawn as what it is.",
     {"map": "streets", "theme": "light", "range_nm": 20}, None),
    ("streets-dark", "KJFK", "Streets, dark, with airspace", "The same map dark, with the FAA's Class B, C and D "
     "airspace and the runways' extended centerlines.",
     {"map": "streets", "theme": "dark", "range_nm": 20, "airspace": True}, None),
    ("card", "KJFK", "A plane's card", "Tap a plane: its type, registration, altitude with the climb or descent, speed, "
     "heading and squawk.", {"map": "streets", "theme": "dark", "range_nm": 20, "airspace": True}, "card"),
    ("sectional", "KSFO", "VFR sectional", "The FAA's sectional chart round San Francisco, with the Class B shelves "
     "over it.", {"map": "sectional", "theme": "light", "range_nm": 20, "airspace": True}, None),
    ("ifr-low-dark", "KORD", "IFR low, dark, with data blocks", "The IFR enroute low chart made dark (its paper and "
     "ink turned round, its colours kept), and every label a controller's data block.",
     {"map": "ifr_low", "theme": "dark", "range_nm": 30, "airspace": True, "label": "scope"}, None),
    ("radar", "KATL", "Radar scope", "The scope drawn on the screen itself, nothing downloaded: its glow, bearing "
     "lines and the beam painting the planes as it passes, with Atlanta's airspace in the phosphor.",
     {"map": "radar", "theme": "dark", "range_nm": 30, "airspace": True, "tint": "green"}, None),
    ("radar-bare", "KJFK", "A controller's bare map", "The scope switched off: the planes, data blocks and airspace "
     "alone on the dark ground, in amber.",
     {"map": "radar", "theme": "dark", "range_nm": 20, "airspace": True, "tint": "amber", "scope": False,
      "label": "scope"}, None),
    ("satellite", "KLAS", "Satellite", "Esri's imagery of Las Vegas with its place names over it.",
     {"map": "satellite", "theme": "dark", "range_nm": 10}, None),
    ("topo", "KSLC", "USGS topo", "The USGS topographic map round Salt Lake City, light.",
     {"map": "topo", "theme": "light", "range_nm": 20}, None),
    ("ifr-high", "KDEN", "IFR high enroute", "The IFR high enroute chart at 80 NM, the jets passing over Denver.",
     {"map": "ifr_high", "theme": "light", "range_nm": 80, "airspace": True, "label": "callsign"}, None),
    ("osm", "EGLL", "OpenStreetMap, dark", "OpenStreetMap round Heathrow, drawn dark with its hues kept, and a watched "
     "plane in a colour of its own.", {"map": "osm", "theme": "dark", "range_nm": 15}, "watch"),
    ("weather-radar", "rain", "Weather radar", "The NEXRAD composite over the dark streets map, where it is raining "
     "now.", {"map": "streets", "theme": "dark", "range_nm": 60, "radar_overlay": True, "airspace": True}, None),
    ("weather-page", "KJFK", "Aviation weather", "A swipe up from the bottom edge: the station's METAR, decoded, with "
     "its flight category in colour; the TAF, MOS and advisories are a tab away.",
     {"map": "streets", "theme": "dark", "range_nm": 20}, "weather"),
    ("menu", "KJFK", "The menu", "A long press opens a menu with every setting the web page has; a tap on a value "
     "types it on a keyboard on the glass.", {"map": "streets", "theme": "dark", "range_nm": 20}, "menu"),
]


def png(width: int, height: int, rgb: bytes) -> bytes:
    def chunk(kind, data):
        return len(data).to_bytes(4, "big") + kind + data + zlib.crc32(kind + data).to_bytes(4, "big")

    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    header = width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def bmp_to_png(bmp: bytes) -> bytes:
    """GET /api/screenshot's BMP (RGB565, the top row first) as a PNG."""
    offset = int.from_bytes(bmp[10:14], "little")
    width = int.from_bytes(bmp[18:22], "little", signed=True)
    height = -int.from_bytes(bmp[22:26], "little", signed=True)
    stride = (width * 2 + 3) & ~3
    lut5 = [v * 255 // 31 for v in range(32)]
    lut6 = [v * 255 // 63 for v in range(64)]
    out = bytearray(width * height * 3)
    i = 0
    for y in range(height):
        row = bmp[offset + y * stride:offset + y * stride + width * 2]
        for x in range(0, width * 2, 2):
            v = row[x] | row[x + 1] << 8
            out[i] = lut5[v >> 11]
            out[i + 1] = lut6[(v >> 5) & 63]
            out[i + 2] = lut5[v & 31]
            i += 3
    return png(width, height, bytes(out))


def wait_drawn(screen: Screen, settings: dict, airspace_before):
    """Until the map's tiles, the airspace and the weather radar asked for are all on the glass (each given up on
    after a while, and said)."""
    time.sleep(3)

    def ready():
        s = screen.json("/api/status")[1]
        tiles = settings["map"] == "radar" or (s["tiles_total"] > 0 and s["tiles_done"] >= s["tiles_total"])
        # A new place's airspace is a new one (its revision changes); the same place keeps the one it has.
        air = not settings.get("airspace") or (
            (airspace_before is None or s["airspace_rev"] != airspace_before) and s["airspace_areas"] > 0)
        radar = not settings.get("radar_overlay") or bool(s.get("radar_scan"))
        return s if tiles and air and radar and s["aircraft"] > 0 else None

    s = wait_for(ready, 150, 2)
    if not s:
        s = screen.json("/api/status")[1]
        print(f"    not all drawn: tiles {s['tiles_done']}/{s['tiles_total']} {s['tile_error']}, airspace "
              f"{s['airspace_areas']} {s['airspace_error']}, radar {s.get('radar_scan')!r}, planes {s['aircraft']}")
    return s


def take(screen: Screen, out: Path, name: str):
    status, kind, body = screen.call("/api/screenshot", timeout=20)
    if status != 200 or not body.startswith(b"BM"):
        print(f"    no picture: {status} {body[:80]!r}")
        return False
    (out / f"{name}.png").write_bytes(bmp_to_png(body))
    print(f"    {out / name}.png")
    return True


def touch_for(glass: Glass, action: str, sky: Sky) -> bool:
    ui = glass.ui()
    w, h = ui["width"], ui["height"]
    if action == "card":
        # A plane near the middle, well inside the glass, so its card has room.
        spots = [p for p in ui["planes"] if 60 < p["x"] < w - 60 and 120 < p["y"] < h - 160]
        spots.sort(key=lambda p: (p["kind"] not in ("airliner", "heavy"), math.hypot(p["x"] - w / 2, p["y"] - h / 2)))
        for p in spots[:4]:
            glass.tap(p["x"], p["y"])
            if wait_for(lambda: glass.ui().get("card"), 3, 0.3):
                time.sleep(1)
                return True
        return False
    if action == "menu":
        return glass.open_menu() is not None
    if action == "weather":
        y = h - 15
        glass.screen.json("/api/touch", {"gesture": "drag", "x": w // 2, "y": y, "x1": w // 2 + 10, "y1": y - 150,
                                         "ms": 400})
        filled = wait_for(lambda: (lambda p: p if p and "Asking" not in p["line"] else None)(glass.ui().get("weather")),
                          45, 1)
        time.sleep(1.5)
        return filled is not None
    return True


def undo_touch(glass: Glass, action: str):
    ui = glass.ui()
    if ui.get("menu"):
        glass.close_menu()
    if ui.get("weather"):
        glass.tap_key(ui["weather"]["close"])
    if ui.get("card") and action == "card":
        glass.tap(ui["width"] // 2, 40)  # off the card and any plane: the card closes
        wait_for(lambda: not glass.ui().get("card"), 3, 0.3)


def write_page(out: Path, columns: int = 3) -> Path:
    """screens.md beside the pictures' folder: every scene that has its picture, in a grid, each with its title and
    what it shows."""
    page = out.parent / "screens.md"
    shown = [s for s in SCENES if (out / f"{s[0]}.png").exists()]
    lines = ["# What the screen shows", "",
             "Pictures of a Waveshare AMOLED 2.41 taken from the screen itself (`tools/screenshots.py`, which asks it for "
             "what its glass shows: `GET /api/screenshot`). The maps, airspace and weather are the real ones; the "
             "planes are made up, so no real flight is shown. Back to the [README](../README.md).", "",
             "<table>"]
    for i in range(0, len(shown), columns):
        lines.append("<tr>")
        for name, _, title, caption, _, _ in shown[i:i + columns]:
            lines.append(f'<td width="33%" valign="top"><img src="{out.name}/{name}.png" alt="{title}" width="100%">'
                         f"<br><b>{title}</b><br>{caption}</td>")
        lines.append("</tr>")
    lines += ["</table>", ""]
    page.write_text("\n".join(lines))
    return page


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", nargs="?", help="the screen's address (an IP or name.local)")
    parser.add_argument("--out", default=str(ROOT / "docs" / "screens"), help="where the pictures go")
    parser.add_argument("--only", default="", help="these scenes alone, by name, with commas between")
    parser.add_argument("--port", type=int, default=8095, help="this machine's port for the screen's planes")
    parser.add_argument("--list", action="store_true", help="the scenes, and nothing taken")
    parser.add_argument("--page", action="store_true", help="screens.md from the pictures there now, nothing taken")
    args = parser.parse_args()
    out = Path(args.out)
    if args.page:
        print(write_page(out))
        return
    if args.list or not args.address:
        for name, place, title, *_ in SCENES:
            print(f"{name:15} {place:6} {title}")
        return
    sys.stdout.reconfigure(line_buffering=True)
    only = {s for s in args.only.split(",") if s}
    scenes = [s for s in SCENES if not only or s[0] in only]
    if only - {s[0] for s in SCENES}:
        raise SystemExit(f"No scene {', '.join(sorted(only - {s[0] for s in SCENES}))}: --list names them")
    out.mkdir(parents=True, exist_ok=True)

    screen = Screen(args.address)
    if wait_for(screen.up, 30, 1) is None:
        raise SystemExit(f"No screen answers at {screen.base}")
    if screen.json("/api/status")[1].get("divert") is None:
        raise SystemExit("This firmware has no test API (test_api: false)")
    if screen.call("/api/screenshot", timeout=20)[0] != 200:
        raise SystemExit("This firmware takes no screenshot (GET /api/screenshot): install a newer one")
    saved = device_test.kept_settings(args.address)
    if saved.exists():
        raise SystemExit(f"A run before could not put the settings back: tests/device_test.py {args.address} "
                         f"--restore ({saved})")
    status, original = screen.json("/api/settings")
    if status != 200:
        raise SystemExit("The screen gave no settings")
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_text(json.dumps(original))

    server = mock_feed.Server(port=args.port).start()
    origin = f"http://{device_test.own_address_towards(args.address)}:{server.port}"
    glass = Glass(screen)
    skies = {}
    failed = 0
    try:
        rain = None
        last_area = None
        for name, place, title, caption, settings, action in scenes:
            if place == "rain":
                rain = rain or rainy_place()
                if not rain:
                    print(f"{name}: no rain reported at any of the airports looked at; left out")
                    continue
                code = rain[0]
                PLACES.setdefault(code, (rain[1], rain[2], rain[3], 0, "us"))
            else:
                code = place
            lat, lon, elevation, _, country = PLACES[code]
            print(f"{name}: {title} at {code}")
            key = (code, settings["range_nm"])
            fresh = key not in skies
            if fresh:
                skies[key] = Sky(code, lat, lon, elevation, runway_for(code), settings["range_nm"], country == "uk")
            sky = skies[key]
            server.feed.aircraft = sky.aircraft  # what the screen's receiver answers from now on
            full = {**COMMON, "lat": lat, "lon": lon, "located": True, "place": code, "feed": "receiver",
                    "receiver_url": origin + "/data/aircraft.json", **settings}
            if action == "watch":
                shown = [p for p in sky.aircraft() if dist_nm(lat, lon, p["lat"], p["lon"]) < settings["range_nm"] * 0.6
                         and p["alt_baro"] > 3000]
                if shown:
                    full["watch"] = [{"id": shown[0]["flight"].strip(), "colour": "purple"}]
            area = (code, settings["range_nm"], settings.get("airspace", False))
            before = None if area == last_area else screen.json("/api/status")[1]["airspace_rev"]
            last_area = area
            status, answer = screen.json("/api/settings", full)
            if status != 200:
                print(f"    settings refused: {answer}")
                failed += 1
                continue
            wait_drawn(screen, full, before)
            # Long enough for the planes' tracks to grow behind them, the first time at a place.
            time.sleep(40 if fresh else 8)
            if action and action != "watch" and not touch_for(glass, action, sky):
                print(f"    the touch for {action} did not take")
                failed += 1
            if not take(screen, out, name):
                failed += 1
            if action:
                undo_touch(glass, action)
    finally:
        server.stop()
        changed = not screen.up() or screen.json("/api/settings")[1] != original
        if not changed or device_test.put_back(screen, original):
            saved.unlink()
            print("Settings put back")
        else:
            print(f"SETTINGS NOT PUT BACK: kept in {saved}; tests/device_test.py {args.address} --restore")
            failed += 1
    print(write_page(out))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
