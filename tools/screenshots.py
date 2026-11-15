#!/usr/bin/env python3
"""The pictures in docs/screens.md: a real screen photographed through its own API, over the real maps with a sky of
made-up planes.

    tools/screenshots.py <address> [--board NAME] [--out FOLDER] [--only NAME,...] [--port 8095]
    tools/screenshots.py --list | --page | --round

Each picture is a scene: a place (a public airport, never the screen's own), a map and its settings, and often a touch
(a plane's card, the menu, the weather page, the day's tally). The screen is sent to this machine for every request it
makes (the test API's divert, which restarts it), and this machine plays the world:

- Its planes are made up (Sky: airliners on the runway's final approach and climbing out, jets passing high over,
  light aircraft, helicopters and business jets, all moving), so no real flight is shown and every run has the same
  kind of traffic. They come as a receiver's on the network, or on a screen with a radio as the messages its radio
  would hear (tests/mock_radio.py), and as the exchanges' when a plane is followed; every answer says they are made up,
  so the glass credits them as simulated.
- One plane is NASA 905, the 747 that carried the shuttles, circling the Statue of Liberty as it did with Enterprise
  in 2012 on its way from Dulles to JFK: its card has that route and a NASA photo (tools/nasa905.jpg, ECN-6887, in the
  public domain) where a plane's Planespotters.net photo goes. The other planes have neither.
- The network-based place is the scene's town, never the screen's own.
- The maps, the FAA's airspace and runways and the weather are the real ones: those requests alone are passed on to
  their servers as the screen sent them (its own User-Agent), and their answers handed back.

The day's tally is set aside while it runs and put back after, so the made-up planes are never counted in the real
day and the real day (the screen's own place) is never in a picture. At the end the divert is lifted (another
restart), the map card's picture is taken where the screen has one, every setting is put back, and screens.md is
written beside the pictures' folder (docs/screens.md): each board's pictures in a grid of their own, each with what it
shows; --page writes it alone. A round board's pictures are round, the corners its glass does not have left
clear; --round makes those taken before that so.

It needs the test API (`test_api: true`: GET /api/screenshot, /api/touch, /api/ui, POST /api/test) and the screen to
reach this machine (WSL: networkingMode=mirrored). Every setting it changes is put back at the end, as
tests/device_test.py does (`tests/device_test.py <address> --restore` puts them back if it could not). It never prints
the screen's own place.

The FAA's runways near each airport are asked once and kept in tests/build/runways-<code>.json, so the approaches
line up with the runways the screen draws. The weather radar's scene goes where the Aviation Weather Center's METARs
report rain or snow now, of a list of large airports (once a run, one request).

Python's standard library only.
"""

import argparse
import http.server
import json
import math
import random
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import device_test  # noqa: E402
import mock_feed  # noqa: E402
import mock_radio  # noqa: E402
import mock_services  # noqa: E402
from device_test import Glass, Screen, wait_for  # noqa: E402

AGENT = "esp-skyscope screenshots (https://github.com/dath1974/esp-skyscope)"

# The boards, by the name their firmware's manifest has: the folder their pictures go in, and what docs/screens.md
# calls them.
BOARDS = {
    "waveshare-p4-lcd-4c": ("p4", "Waveshare ESP32-P4 4C", "the round 4-inch 720 × 720 screen, with its own radio"),
    "waveshare-amoled-241": ("amoled", "Waveshare AMOLED 2.41", "the 450 × 600 AMOLED"),
    "waveshare-lcd-28b": ("lcd-28b", "Waveshare LCD 2.8B", "the 480 × 640 LCD"),
}
# The boards whose glass is round (board `round: true`): their pictures are round too, the corners the glass does not
# have left clear, so a page of any color shows them as the screen looks.
ROUND = {"p4"}

# ---------------------------------------------------------------- places

# Public airports: (latitude, longitude, field elevation in feet, the direction they land today, roughly, in degrees
# true; the runway nearest it is used), the country ("us" for the FAA's runways), and the town the screen's network
# would be put in.
PLACES = {
    "KJFK": (40.6413, -73.7781, 13, 300, "us", "Queens, New York"),
    "KSFO": (37.6189, -122.3750, 13, 280, "us", "San Bruno, California"),
    "KORD": (41.9786, -87.9048, 672, 270, "us", "Chicago, Illinois"),
    "KDEN": (39.8561, -104.6737, 5434, 170, "us", "Denver, Colorado"),
    "KSLC": (40.7884, -111.9778, 4227, 340, "us", "Salt Lake City, Utah"),
    "KATL": (33.6407, -84.4277, 1026, 270, "us", "College Park, Georgia"),
    "KLAS": (36.0840, -115.1537, 2181, 260, "us", "Paradise, Nevada"),
    "KSEA": (47.4502, -122.3088, 433, 160, "us", "SeaTac, Washington"),
    "EGLL": (51.4700, -0.4543, 83, 270, "uk", "Hounslow, England"),
}
# Where to look for rain for the weather radar's scene: large airports across the United States.
RAIN_CANDIDATES = ["KATL", "KBOS", "KBNA", "KCLT", "KDCA", "KDEN", "KDFW", "KDTW", "KIAH", "KJFK", "KLAX", "KMCI",
                   "KMCO", "KMEM", "KMIA", "KMSP", "KMSY", "KORD", "KPDX", "KPHL", "KPHX", "KPIT", "KSEA", "KSFO",
                   "KSLC", "KSTL", "KTPA", "KOKC", "KOMA", "KBUF", "KCLE", "KCVG", "KIND", "KJAX", "KRDU", "KSAT"]

NM_PER_DEGREE = 60.0

# NASA 905, Enterprise on its back, circling the Statue of Liberty on its way from Dulles to JFK (April 2012).
NASA = {"hex": "ad905a", "flight": "NASA905", "t": "B741", "r": "N905NA", "desc": "BOEING 747-100", "ownOp": "NASA",
        "squawk": "4601", "category": "A5"}
LIBERTY = (40.6892, -74.0445)
NASA_ROUTE = [("KIAD", "IAD", "Washington Dulles", 38.9445, -77.4558), ("KJFK", "JFK", "New York JFK", 40.6413,
                                                                        -73.7781)]
NASA_PHOTO = ROOT / "tools" / "nasa905.jpg"  # NASA ECN-6887, in the public domain, cut to a thumbnail's size
NASA_PHOTO_PAGE = "https://commons.wikimedia.org/wiki/File:OV-101-CaptiveFlights.jpg"


def bearing_offset(lat, lon, bearing, nm):
    """The place `nm` away from lat, lon toward `bearing` (degrees true)."""
    b = math.radians(bearing)
    return mock_feed.offset(lat, lon, nm * math.cos(b), nm * math.sin(b))


def get_json(url: str, timeout: float = 20):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as r:
        return json.loads(r.read())


def runway_for(code: str):
    """The landing runway at `code`: (threshold lat, lon, the direction it lands) or None. Asked of the FAA (the
    screen's own query, airspace.h) once and kept; when the FAA is busy, tried again a minute later, twice."""
    lat, lon, _, flow, country, _ = PLACES[code]
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
            best = (score, m["icaoId"], m["lat"], m["lon"], int((m.get("elev") or 0) * 3.28), m.get("name") or "")
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
OPERATORS = {"AAL": "AMERICAN AIRLINES INC", "DAL": "DELTA AIR LINES INC", "UAL": "UNITED AIRLINES INC",
             "JBU": "JETBLUE AIRWAYS CORP", "SWA": "SOUTHWEST AIRLINES CO", "SKW": "SKYWEST AIRLINES INC",
             "RPA": "REPUBLIC AIRWAYS INC", "ASA": "ALASKA AIRLINES INC", "FDX": "FEDERAL EXPRESS CORP",
             "UPS": "UNITED PARCEL SERVICE CO", "NKS": "SPIRIT AIRLINES INC", "BAW": "British Airways",
             "DLH": "Lufthansa", "AFR": "Air France", "UAE": "Emirates", "KLM": "KLM", "JAL": "Japan Airlines",
             "EZY": "easyJet", "VIR": "Virgin Atlantic", "RYR": "Ryanair", "SHT": "British Airways"}
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
HEAVIES = {"A359", "A35K", "A388", "B744", "B748", "B763", "B77W", "B789", "B741"}


class Sky:
    """Planes for one place, laid out once (the same each run) and flown by the clock."""

    def __init__(self, code: str, lat: float, lon: float, elevation: int, runway, range_nm: int, uk: bool):
        self.lat, self.lon, self.elevation = lat, lon, elevation
        self.started = time.time()
        self.squawking = set()  # flights squawking an emergency (7700) now
        rnd = random.Random(code)
        reach = max(12, min(range_nm, 60))
        airlines = UK_AIRLINES if uk else US_AIRLINES + (FAR_AIRLINES if code == "KJFK" else [])
        # Each place its own block of addresses, so a radio moved on never takes one place's plane for another's.
        base = 0xa00000 | (zlib.crc32(code.encode()) & 0xff) << 12
        used = set()

        def callsign():
            while True:
                icao, types = rnd.choice(airlines)
                c = f"{icao}{rnd.randint(12, 2899)}"
                if c not in used:
                    used.add(c)
                    return c, rnd.choice(types), OPERATORS.get(icao, "")

        def registration():
            if uk:
                return "G-" + "".join(rnd.choice("ABCDEFGHJKLMNPRSTUVWXYZ") for _ in range(4))
            return f"N{rnd.randint(100, 999)}{rnd.choice('ABCDEFGHJKLMNPRSTUVWXYZ')}{rnd.choice('ABCDEFGHJKLMNPRSTUVWXYZ')}"

        def squawk():
            return "".join(str(rnd.randint(0, 7)) for _ in range(4)).replace("7700", "4217")

        self.planes = []
        n = 0

        def add(path, fields, hex_=None):
            nonlocal n
            n += 1
            self.planes.append((hex_ or f"{base + n * 16:06x}", path, fields))

        def airline(path, rate=None):
            flight, t, operator = callsign()
            fields = {"flight": flight, "t": t, "r": registration(), "squawk": squawk(), "ownOp": operator}
            if rate is not None:
                fields["baro_rate"] = rate
            add(path, fields)

        if runway:
            t_lat, t_lon, course = runway
        else:  # outside the FAA's data: the field and the direction of the flow
            t_lat, t_lon, course = lat, lon, PLACES[code][3]
        # Arrivals strung out down the final approach, three miles apart, and one joining it from the side.
        for i in range(4):
            airline(("final", t_lat, t_lon, course, 3 + i * 3.2 + rnd.random(), 150 + rnd.randint(0, 25)), -768)
        for side in (rnd.choice((-1, 1)),):
            airline(("base", t_lat, t_lon, course, side, 24 + rnd.random() * 8, 210), -1200)
        # Departures climbing out and turning away.
        for i, turn in enumerate((-45, 70, -80)):
            airline(("climb", t_lat, t_lon, course, turn, 2 + i * 6 + rnd.random() * 2, 250 + rnd.randint(0, 40)), 2496)
        # A straight line across the area at `off` NM from the place, from the edge of a circle well outside the
        # glass to its other side: a plane comes into sight and leaves it there, and a radio's last word of one gone
        # (kept a minute, as for a plane out of its reach) is never on the glass beside the plane come back as another.
        edge = reach * 1.5

        def across(off, track, gs, alt):
            north, east = off * math.cos(math.radians(track + 90)), off * math.sin(math.radians(track + 90))
            return ("line", north, east, track, gs, 2 * math.sqrt(edge * edge - off * off), rnd.random(), alt)

        # Over the top, high and fast, in every direction.
        for i in range(4):
            airline(across(rnd.uniform(-reach, reach) * 0.8, rnd.uniform(0, 360), 430 + rnd.randint(0, 60),
                           29000 + 2000 * rnd.randint(0, 6)))
        # Light aircraft, a turboprop, helicopters and business jets, lower down.
        for t, gs, alt in (("C172", 105, 2500), ("SR22", 165, 4500), ("PC12", 260, 9500), ("EC35", 120, 1200),
                           ("GLF6", 330, 12000)):
            heli = t in ("EC35", "B06", "AS50")
            near = reach * (0.35 if heli else 0.7)
            fields = {"t": t, "r": registration(), "squawk": "1200" if t in ("C172", "PA28") else squawk()}
            fields["flight"] = fields["r"].replace("-", "")
            if t in ("GLF6", "C68A", "CL35"):
                fields["flight"] = rnd.choice(["EJA", "LXJ", "XSR"]) + str(rnd.randint(100, 999))
            add(across(rnd.uniform(-near, near), rnd.uniform(0, 360), gs, alt + elevation), fields)
        if code == "KJFK":
            # NASA 905 and Enterprise, round and round the Statue of Liberty at 2000 ft.
            add(("orbit", *LIBERTY, 2.5, 160, 2000), {k: v for k, v in NASA.items() if k not in ("hex", "category")},
                NASA["hex"])

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
            if fields["flight"] in self.squawking:
                plane["squawk"] = "7700"
                plane["emergency"] = "general"
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
            traveled = (30 - start) + gs * seconds / 3600
            generation = int(traveled // span)
            s = traveled % span  # along the leg, from its start 22 NM out to the side
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
            traveled = start + gs * seconds / 3600
            generation = int(traveled // span)
            s = traveled % span
            straight = min(s, 4)
            lat, lon = bearing_offset(t_lat, t_lon, course, straight)
            track = course
            if s > 4:
                track = course + turn
                lat, lon = bearing_offset(lat, lon, track, s - 4)
            return lat, lon, self.elevation + min(1000 + s * 650, 17000), gs, track, generation
        if kind == "orbit":
            # Round a point to the left, `radius` NM out, never another plane.
            _, c_lat, c_lon, radius, gs, alt = path
            angle = math.degrees(gs * seconds / 3600 / radius) % 360  # around, from the north, anticlockwise
            lat, lon = bearing_offset(c_lat, c_lon, -angle, radius)
            return lat, lon, alt, gs, (-angle - 90) % 360, 0
        # A straight line through the area, `length` long, and around again (as another plane) when it leaves.
        _, north, east, track, gs, length, phase, alt = path
        traveled = phase * length + gs * seconds / 3600
        generation = int(traveled // length)
        s = traveled % length - length / 2
        lat, lon = mock_feed.offset(self.lat, self.lon, north, east)
        lat, lon = bearing_offset(lat, lon, track, s)
        return lat, lon, alt, gs, track, generation


# ---------------------------------------------------------------- the world, played by this machine

# The services whose real answers the pictures show, passed on as the screen asked: the maps, the FAA's airspace,
# runways and airports, the weather and the weather radar, and places by name.
PASSED_ON = {"server.arcgisonline.com", "tile.openstreetmap.org", "basemap.nationalmap.gov", "tiles.arcgis.com",
             "services6.arcgis.com", "aviationweather.gov", "mesonet.agron.iastate.edu",
             "nominatim.openstreetmap.org"}
# Of an answer passed on, the headers handed back with it (how long to keep it, and how soon to ask again).
KEPT_HEADERS = ("Cache-Control", "Expires", "Last-Modified", "ETag", "Retry-After", "x-ratelimit-limit",
                "x-ratelimit-remaining", "x-ratelimit-reset")


class Studio:
    """Everything the screen asks while it is sent here: the made-up planes and what goes with them, and the real
    maps, airspace and weather passed on (PASSED_ON). Anything else is answered 404 and noted."""

    def __init__(self, port: int):
        self.lock = threading.Lock()
        self.sky = None
        self.lat, self.lon, self.town = PLACES["KJFK"][0], PLACES["KJFK"][1], PLACES["KJFK"][5]
        self.unknown = []
        self.passed = self.failed = 0
        studio = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"  # keep-alive, as the screen's client uses it

            def log_message(self, *args):
                pass

            def send(self, status: int, kind: str, body: bytes, headers=None):
                self.send_response(status)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                status, kind, body, headers = studio.answer(self.path, self.headers)
                self.send(status, kind, body, headers)

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                status, kind, body = studio.post(self.path, body)
                self.send(status, kind, body)

        self.httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]

    def start(self):
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def place(self, sky, lat, lon, town):
        with self.lock:
            self.sky, self.lat, self.lon, self.town = sky, lat, lon, town

    def aircraft(self):
        with self.lock:
            sky = self.sky
        return sky.aircraft() if sky else []

    def answer(self, path: str, asked):
        """(status, content type, body, headers) for a GET of `path`."""
        as_json = lambda body, status=200: (status, "application/json", json.dumps(body).encode(), {})
        if path.split("?")[0].endswith("/data/aircraft.json"):
            # Made-up planes, said so: the screen credits them as "simulated", never as a real receiver's.
            return as_json({"now": time.time(), "messages": 1000, "simulated": True, "aircraft": self.aircraft()})
        parsed = urllib.parse.urlsplit(path)
        host, _, rest = parsed.path.lstrip("/").partition("/")
        with self.lock:
            lat, lon, town = self.lat, self.lon, self.town
        if host in ("opendata.adsb.fi", "api.adsb.lol"):
            # The exchanges (a followed plane, and the planes around it): the same sky, said to be made up.
            found = mock_services.answer(path, lat, lon, time.time(), self.aircraft)
            if found and found[0] == 200:
                body = json.loads(found[2])
                body["simulated"] = True
                return as_json(body)
        if host == "api.planespotters.net" and rest.startswith("pub/photos/hex/"):
            wanted = rest.removeprefix("pub/photos/hex/").strip().lower()
            if wanted != NASA["hex"]:
                return as_json({"photos": []})
            return as_json({"photos": [{"id": "ECN-6887",
                                        "thumbnail": {"src": f"https://cdn.planespotters.net/nasa/{NASA['hex']}.jpg",
                                                      "size": {"width": 200, "height": 133}},
                                        "link": NASA_PHOTO_PAGE, "photographer": "NASA"}]})
        if host == "cdn.planespotters.net":
            return 200, "image/jpeg", NASA_PHOTO.read_bytes(), {}
        if host == "hexdb.io" and rest.startswith("api/v2/routes/"):
            callsign = urllib.parse.unquote(rest.removeprefix("api/v2/routes/")).strip().upper()
            if callsign != NASA["flight"]:
                return as_json({"error": {"status": 404, "code": "route_not_found",
                                          "message": f"No route for callsign {callsign}."}}, 404)
            airports = [{"icao": i, "iata": a, "name": n, "country_code": "US", "region": "", "latitude": la,
                         "longitude": lo} for i, a, n, la, lo in NASA_ROUTE]
            return as_json({"callsign": callsign, "origin": airports[0], "destination": airports[-1],
                            "airports": airports, "updated_at": "2012-04-27T00:00:00Z"})
        if host == "free.freeipapi.com":
            # The network's place: the scene's town, never the screen's own.
            city, _, region = town.partition(", ")
            return as_json({"ipVersion": 4, "ipAddress": "192.0.2.1", "latitude": round(lat + 0.03, 4),
                            "longitude": round(lon - 0.04, 4), "countryName": "United States of America",
                            "countryCode": "US", "cityName": city, "regionName": region, "isProxy": False})
        if host in PASSED_ON:
            return self.pass_on(host, rest + ("?" + parsed.query if parsed.query else ""), asked)
        with self.lock:
            self.unknown.append(path)
        return as_json({"msg": "not here"}, 404)

    def pass_on(self, host: str, rest: str, asked):
        """The real service's answer, asked as the screen asked it."""
        headers = {k: asked[k] for k in ("User-Agent", "Accept", "If-None-Match", "If-Modified-Since") if asked.get(k)}
        request = urllib.request.Request(f"https://{host}/{rest}", headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=30) as r:
                status, kind, body, got = r.status, r.headers.get("Content-Type", ""), r.read(), r.headers
        except urllib.error.HTTPError as e:
            status, kind, body, got = e.code, e.headers.get("Content-Type", ""), e.read(), e.headers
        except OSError as e:
            with self.lock:
                self.failed += 1
            return 502, "application/json", json.dumps({"msg": str(e)}).encode(), {}
        with self.lock:
            self.passed += 1
        return status, kind or "application/octet-stream", body, {k: got[k] for k in KEPT_HEADERS if got.get(k)}

    def post(self, path: str, body: bytes):
        """adsb.lol's routes: NASA 905's, and no other's."""
        if path.lstrip("/").split("?")[0] != "api.adsb.lol/api/0/routeset":
            with self.lock:
                self.unknown.append("POST " + path)
            return 404, "application/json", b'{"msg":"not here"}'
        try:
            planes = (json.loads(body or b"{}") or {}).get("planes") or []
        except ValueError:
            return 400, "application/json", b""
        out = []
        for p in planes:
            callsign = str(p.get("callsign", "")).strip().upper()
            if callsign != NASA["flight"]:
                out.append({"callsign": callsign, "airport_codes": "unknown", "_airports": []})
                continue
            out.append({"callsign": callsign, "airport_codes": "-".join(i for i, *_ in NASA_ROUTE), "plausible": True,
                        "_airports": [{"icao": i, "iata": a, "name": n, "lat": la, "lon": lo}
                                      for i, a, n, la, lo in NASA_ROUTE]})
        return 200, "application/json", json.dumps(out).encode()


class RadioSender:
    """On a screen with a radio, the sky as its radio would hear it: every plane's messages twice a second, even
    positions and odd in turn (tests/mock_radio.py), handed over with POST /api/test {"radio": [...]}."""

    def __init__(self, screen: Screen, studio: Studio):
        self.screen, self.studio = screen, studio
        self.stopping = threading.Event()
        self.failures = 0
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stopping.set()
        self.thread.join(5)

    def run(self):
        odd = False
        while not self.stopping.wait(0.5):
            messages = mock_radio.heard(self.studio.aircraft(), odd)
            odd = not odd
            for at in range(0, len(messages), 100):  # well inside the web server's 6000 bytes
                try:
                    if self.screen.call("/api/test", {"radio": messages[at:at + 100]}, timeout=5)[0] != 200:
                        self.failures += 1
                except OSError:
                    self.failures += 1  # restarting, or busy: the next half second tries again


# ---------------------------------------------------------------- the scenes

# What every scene starts from (as device_test's own, a new screen's settings but for the place and the planes).
COMMON = {"map_brightness": 100, "tile_scale": 0, "rings": True, "place_names": True, "airspace": False,
          "tint": "green", "scope": True, "sweep": True, "fallback": False, "refresh_s": 5, "alt_min": 0,
          "alt_max": 60000, "ground": False, "most": 40, "marker": "type", "label": "auto", "by_altitude": True,
          "glide": True, "track_s": 90, "watch": [], "follow": "", "metric": False, "brightness": 80,
          "sleep_min": 0, "bedtime": False, "weather_station": "", "radar_overlay": False, "radar_loop": False,
          "photos": True, "tour": False}


class Scene:
    """A picture: where, what it shows (for docs/screens.md), the settings, and a touch after (`action`). `details`
    for one that needs a plane's type and registration, which a radio does not hear: its planes come from the
    receiver. `boards` limits it to those (BOARDS' folder names); `after` takes it once the divert is lifted."""

    def __init__(self, name, place, title, caption, settings, action=None, details=False, boards=(), after=False):
        self.name, self.place, self.title, self.caption = name, place, title, caption
        self.settings, self.action, self.details, self.boards, self.after = settings, action, details, boards, after

    def caption_for(self, board: str) -> str:
        return self.caption.get(board, self.caption[""]) if isinstance(self.caption, dict) else self.caption


DARK_JFK = {"map": "streets", "theme": "dark", "range_nm": 20}
SCENES = [
    Scene("splash", "KJFK", "At the start", {
        "": "The emblem, a radar scope whose beam is a propeller, turning to rest over the screen's name and version "
            "while it joins its network and fetches its map.",
        "p4": "The emblem, a radar scope whose beam is a propeller, turning to rest over the screen's name and "
              "version; on the round screen the scope's rim is the edge of the glass."},
          {"map": "streets", "theme": "light", "range_nm": 20}, "splash"),
    Scene("streets-light", "KJFK", "Streets, light", "Esri's light gray map with range rings and the compass rose, "
          "as a new screen starts. Arrivals line up on the final approach; each plane is drawn as what it is.",
          {"map": "streets", "theme": "light", "range_nm": 20}),
    # Early, at the first place: the divert's restart has emptied the radio's memory of the planes it heard (it
    # keeps them 20 minutes, locate.h), so where it puts the screen is from this place's planes alone.
    Scene("locate", "KJFK", "Where is the screen?", {
        "": "Locate, under the place's field in the menu, offers the town the screen's internet address is in (asked "
            "only then); neither is used until it is picked.",
        "p4": "Locate, under the place's field in the menu: where the planes the screen's own radio hears put it, and "
              "the town its internet address is in (asked only then); neither is used until it is picked."},
          DARK_JFK, "locate"),
    Scene("streets-dark", "KJFK", "Streets, dark, with airspace", "The same map dark, with the FAA's Class B, C and D "
          "airspace and the runways' extended centerlines.", {**DARK_JFK, "airspace": True}),
    Scene("card", "KJFK", "A plane's card", "Tap a plane: its type, registration, operator, altitude with the climb or "
          "descent, speed and squawk, its route (drawn on the map), which way to look for it, and its photo with a QR "
          "code to the photo's page. Here NASA's 747 with Enterprise on its back, circling the Statue of Liberty, and "
          "a NASA photo standing in for Planespotters.net's.", {**DARK_JFK, "airspace": True}, "card", details=True),
    Scene("follow", "KJFK", "Following a plane", "The map moves with a plane followed, at a range for its speed, with "
          "its track since it was found and keys down the left edge for its route and the range. NASA 905 over the "
          "harbor on Esri's imagery.",
          {"map": "satellite", "theme": "dark", "range_nm": 20, "follow": NASA["flight"], "follow_label": "callsign"},
          "follow", details=True),
    Scene("tour", "KJFK", "The tour", "A new screen's first map comes with a tour of what a finger does, one touch at "
          "a time.", {"map": "streets", "theme": "light", "range_nm": 20}, "tour"),
    Scene("weather-page", "KJFK", "Aviation weather", "A swipe up from the bottom edge: the station's METAR, decoded, "
          "with its flight category in color; the TAF, MOS and advisories are a tab away.", DARK_JFK, "weather"),
    Scene("sectional", "KSFO", "VFR sectional", "The FAA's sectional chart around San Francisco, with the Class B "
          "shelves over it.", {"map": "sectional", "theme": "light", "range_nm": 20, "airspace": True}),
    Scene("ifr-low-dark", "KORD", "IFR low, dark, with data blocks", "The IFR enroute low chart made dark (its paper "
          "and ink turned around, its colors kept), and every label a controller's data block.",
          {"map": "ifr_low", "theme": "dark", "range_nm": 30, "airspace": True, "label": "scope"}),
    Scene("radar", "KATL", "Radar scope", "The scope drawn on the screen itself, nothing downloaded: its glow, bearing "
          "lines and the beam painting the planes as it passes, with Atlanta's airspace in the phosphor.",
          {"map": "radar", "theme": "dark", "range_nm": 30, "airspace": True, "tint": "green"}),
    Scene("phosphor", "KATL", "The phosphor in any color", "The menu's color picker: a swatch for each named phosphor, "
          "a slider for each of red, green and blue, and a small scope lit in the color.",
          {"map": "radar", "theme": "dark", "range_nm": 30, "airspace": True, "tint": "#ff5fd2"}, "phosphor"),
    Scene("radar-bare", "KJFK", "A controller's bare map", "The scope switched off: the planes, data blocks and "
          "airspace alone on the dark ground, in amber.",
          {"map": "radar", "theme": "dark", "range_nm": 20, "airspace": True, "tint": "amber", "scope": False,
           "label": "scope"}),
    Scene("satellite", "KLAS", "Satellite", "Esri's imagery of Las Vegas with its place names over it.",
          {"map": "satellite", "theme": "dark", "range_nm": 10}),
    Scene("topo", "KSLC", "USGS topo", "The USGS topographic map around Salt Lake City, light.",
          {"map": "topo", "theme": "light", "range_nm": 20}),
    Scene("ifr-high", "KDEN", "IFR high enroute", "The IFR high enroute chart at 80 NM, the jets passing over Denver.",
          {"map": "ifr_high", "theme": "light", "range_nm": 80, "airspace": True, "label": "callsign"}),
    Scene("alert", "KSEA", "An alert", "A plane squawking 7700 in range shows along the top of the glass, its edge in "
          "the emergency's red; a tap on it opens the plane's card. A watched plane coming into range, or one passing "
          "close by, shows the same way.", {**DARK_JFK, "airspace": True}, "alert"),
    Scene("osm", "EGLL", "OpenStreetMap, dark", "OpenStreetMap around Heathrow, drawn dark with its hues kept, and a "
          "watched plane in a color of its own.", {"map": "osm", "theme": "dark", "range_nm": 15}, "watch"),
    Scene("swipe", "EGLL", "Another map with a swipe", "A swipe along the bottom edge goes to the next map, its name "
          "shown large with a dot for each; nothing is downloaded until the swipes stop.",
          {"map": "osm", "theme": "dark", "range_nm": 15}, "swipe"),
    Scene("weather-radar", "rain", "Weather radar", "The NEXRAD composite over the dark streets map, where it is "
          "raining now.", {"map": "streets", "theme": "dark", "range_nm": 60, "radar_overlay": True, "airspace": True}),
    Scene("today", "KJFK", "Today's planes", "A swipe down from the top edge: the planes seen in range today, in range "
          "now and the most at once, a bar for each hour, the day's records and its most common types and operators.",
          DARK_JFK, "today"),
    Scene("menu", "KJFK", "The menu", "A long press opens a menu with every setting the web page has; a tap on a value "
          "types it on a keyboard on the glass.", DARK_JFK, "menu"),
    Scene("map-card", "KJFK", "The map card", "With a microSD card in, every map tile is kept on it. The menu shows the "
          "card, its space and what the screen keeps on it, map by map.", DARK_JFK, "card-menu", after=True),
]
ACTIONS = (None, "splash", "card", "follow", "tour", "weather", "phosphor", "alert", "locate", "watch", "swipe", "today",
           "menu", "card-menu")


def png(width: int, height: int, rgb: bytes, alpha: bytes = None) -> bytes:
    """A PNG of 8-bit RGB pixels, top row first; with `alpha` (a byte a pixel), RGBA."""
    def chunk(kind, data):
        return len(data).to_bytes(4, "big") + kind + data + zlib.crc32(kind + data).to_bytes(4, "big")

    if alpha is not None:
        rgba = bytearray(width * height * 4)
        for c in range(3):
            rgba[c::4] = rgb[c::3]
        rgba[3::4] = alpha
        pixels, size, kind = bytes(rgba), 4, 6
    else:
        pixels, size, kind = rgb, 3, 2
    rows = b"".join(b"\0" + pixels[y * width * size:(y + 1) * width * size] for y in range(height))
    header = width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, kind, 0, 0, 0])
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def round_glass(width: int, height: int) -> bytes:
    """The alpha of a round glass: opaque inside the circle that fills the picture, clear outside it, and the pixels
    the edge crosses partly clear (by how far their middle is from it), so the edge is smooth."""
    r = min(width, height) / 2
    cx, cy = width / 2, height / 2
    out = bytearray(width * height)
    for y in range(height):
        dy = y + 0.5 - cy
        for x in range(width):
            inside = r - math.hypot(x + 0.5 - cx, dy) + 0.5
            out[y * width + x] = 255 if inside >= 1 else 0 if inside <= 0 else int(inside * 255 + 0.5)
    return bytes(out)


def png_rgb(data: bytes):
    """width, height and RGB pixels of a PNG this script wrote (8-bit RGB, every row filter 0)."""
    width, height = int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    if data[24:26] != bytes([8, 2]):
        raise ValueError("not a picture of RGB pixels")
    at, idat = 8, b""
    while at < len(data):
        length = int.from_bytes(data[at:at + 4], "big")
        if data[at + 4:at + 8] == b"IDAT":
            idat += data[at + 8:at + 8 + length]
        at += 12 + length
    raw = zlib.decompress(idat)
    row = width * 3 + 1
    if any(raw[y * row] for y in range(height)):
        raise ValueError("rows filtered")
    return width, height, b"".join(raw[y * row + 1:(y + 1) * row] for y in range(height))


def made_round(folder: Path) -> int:
    """Every picture in `folder` still square made round; how many."""
    done = 0
    for f in sorted(folder.glob("*.png")):
        data = f.read_bytes()
        if data[25] != 2:
            continue  # round already
        width, height, rgb = png_rgb(data)
        f.write_bytes(png(width, height, rgb, round_glass(width, height)))
        done += 1
    return done


def bmp_to_png(bmp: bytes, round_: bool = False) -> bytes:
    """GET /api/screenshot's BMP (RGB565, the top row first) as a PNG; round, the corners clear, for a round glass."""
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
    return png(width, height, bytes(out), round_glass(width, height) if round_ else None)


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
    """The glass now, as out/<name>.png (round where the board's glass is)."""
    status, kind, body = screen.call("/api/screenshot", timeout=30)
    if status != 200 or not body.startswith(b"BM"):
        print(f"    no picture: {status} {body[:80]!r}")
        return False
    (out / f"{name}.png").write_bytes(bmp_to_png(body, out.name in ROUND))
    print(f"    {out / name}.png")
    return True


def swipe_from_edge(glass: Glass, ui: dict, way: str):
    """A finger from the bottom edge up ("up"), the top edge down ("down"), or along the bottom edge ("along")."""
    w, h = ui["width"], ui["height"]
    if way == "up":
        start, end = (w // 2, h - 15), (w // 2 + 10, h - 165)
    elif way == "down":
        start, end = (w // 2, 8), (w // 2 + 10, 220)
    else:
        start, end = (w // 2 + w // 6, h - 14), (w // 2 - w // 6, h - 14)
    glass.screen.json("/api/touch", {"gesture": "drag", "x": start[0], "y": start[1], "x1": end[0], "y1": end[1],
                                     "ms": 350})


def touch_for(screen: Screen, glass: Glass, action: str, sky: Sky, take_now):
    """The touch the scene's picture is of, and the picture taken (`take_now`) at the right moment. False if the touch
    did not take."""
    ui = glass.ui()
    w, h = ui["width"], ui["height"]
    if action == "splash":
        screen.json("/api/test", {"splash": True})
        time.sleep(3.2)  # the beam come to rest over the name, before the splash goes
        return take_now()
    if action == "card":
        # NASA 905 if it is in sight, else a plane near the middle well inside the glass, so its card has room.
        spots = [p for p in ui["planes"] if 60 < p["x"] < w - 60 and 120 < p["y"] < h - 160]
        spots.sort(key=lambda p: (p["id"] != NASA["hex"], p["kind"] not in ("airliner", "heavy"),
                                  math.hypot(p["x"] - w / 2, p["y"] - h / 2)))
        for p in spots[:4]:
            glass.tap(p["x"], p["y"])
            if wait_for(lambda: glass.ui().get("card"), 3, 0.3):
                # Its photo and its route, which take a request each.
                wait_for(lambda: (lambda c: c if c and c.get("photo") and glass.ui().get("route_points") else None)(
                    glass.ui().get("card")), 20, 1)
                time.sleep(1.5)
                return take_now()
        return False
    if action == "follow":
        keys = wait_for(lambda: glass.ui().get("follow_keys"), 60, 1)
        if not keys:
            return False
        if keys.get("route"):
            glass.tap_key(keys["route"])  # the whole route once, and back to the plane: its line stays drawn
            time.sleep(4)
            glass.tap_key(glass.ui()["follow_keys"]["plane"])
        time.sleep(75)  # once round the statue, for its track
        return take_now()
    if action == "tour":
        screen.json("/api/settings", {"tour": True})
        if not wait_for(lambda: (glass.ui().get("tour") or {}).get("shown"), 10, 0.5):
            return False
        time.sleep(1)
        return take_now()
    if action == "weather":
        swipe_from_edge(glass, ui, "up")
        filled = wait_for(lambda: (lambda p: p if p and "Asking" not in p["line"] else None)(glass.ui().get("weather")),
                          45, 1)
        time.sleep(1.5)
        return filled is not None and take_now()
    if action == "phosphor":
        glass.open_menu()
        row = glass.reach_row("tint", "edit")
        if not row:
            return False
        glass.tap_key(row["edit"])
        if not wait_for(lambda: (glass.menu() or {}).get("color"), 3, 0.3):
            return False
        time.sleep(1)
        return take_now()
    if action == "alert":
        # The C172 calls an emergency; the alert comes with the next report.
        light = next(fields["flight"] for _, path, fields in sky.planes if fields["t"] == "C172")
        sky.squawking.add(light)
        try:
            if not wait_for(lambda: glass.ui().get("alert"), 30, 0.5):
                return False
            time.sleep(1)
            return take_now()
        finally:
            sky.squawking.discard(light)
    if action == "locate":
        glass.open_menu()
        row = glass.reach_row("place", "edit")
        if not row:
            return False
        glass.tap_key(row["edit"])
        editor = wait_for(lambda: (glass.menu() or {}).get("editor"), 5, 0.3)
        if not editor or not editor.get("locate"):
            return False
        glass.tap_key(editor["locate"])
        picker = wait_for(lambda: (lambda p: p if p and not p["line"].startswith("Looking") else None)(
            (glass.menu() or {}).get("picker")), 30, 0.5)
        time.sleep(1)
        return picker is not None and take_now()
    if action == "swipe":
        swipe_from_edge(glass, ui, "along")
        return bool(wait_for(lambda: glass.ui().get("swipe"), 2, 0.1)) and take_now()
    if action == "today":
        swipe_from_edge(glass, ui, "down")
        if not wait_for(lambda: glass.ui().get("today"), 5, 0.3):
            return False
        time.sleep(2)
        return take_now()
    if action == "menu":
        return glass.open_menu() is not None and take_now()
    if action == "card-menu":
        glass.open_menu()
        if not glass.scroll_to(lambda m: m["keys"].get("card_clear")):
            return False
        time.sleep(2)  # its count of what is on the card
        return take_now()
    return take_now()


def undo_touch(screen: Screen, glass: Glass, action: str):
    if action == "tour":
        screen.json("/api/settings", {"tour": False})
        wait_for(lambda: "tour" not in glass.ui(), 5, 0.5)
    ui = glass.ui()
    menu = ui.get("menu") or {}
    if menu.get("color"):
        glass.tap_key(menu["color"]["cancel"])
    if menu.get("picker"):
        # A place picked takes the keyboard away with it, back to the menu, whose Cancel drops it (as
        # tests/device_test.py leaves Locate): the picker's Back would leave the keyboard over everything.
        town = next((i for i in menu["picker"]["items"] if i["name"] == "Network-based location"), None)
        glass.tap_key(town or menu["picker"]["back"])
        wait_for(lambda: not (glass.menu() or {}).get("picker"), 5, 0.3)
    if glass.ui().get("menu"):
        glass.close_menu()
    ui = glass.ui()
    if ui.get("weather"):
        glass.tap_key(ui["weather"]["close"])
    if ui.get("today"):
        glass.tap_key(ui["today"]["close"])
    if ui.get("card"):
        glass.tap(ui["width"] // 2, 40)  # off the card and any plane: the card closes
        wait_for(lambda: not glass.ui().get("card"), 3, 0.3)


def write_page(screens: Path, columns: int = 3) -> Path:
    """screens.md beside the pictures' folder: for each board with pictures there, its scenes in a grid, each with its
    title and what it shows."""
    page = screens.parent / "screens.md"
    lines = ["# What the screen shows", "",
             "Pictures taken from the screens themselves (`tools/screenshots.py`, which asks a screen for what its "
             "glass shows: `GET /api/screenshot`). The maps, airspace and weather are the real ones; the planes are "
             "made up, so no real flight is shown, and the glass credits them as **simulated**. Back to the "
             "[README](../README.md).", ""]
    boards = [b for b in BOARDS.values() if any((screens / b[0] / f"{s.name}.png").exists() for s in SCENES)]
    if len(boards) > 1:
        lines += [" · ".join(f"[{title}](#{title.lower().replace(' ', '-').replace('.', '')})"
                             for _, title, _ in boards), ""]
    for folder, title, about in boards:
        shown = [s for s in SCENES if (screens / folder / f"{s.name}.png").exists()]
        lines += [f"## {title}", ""]
        intro = f"Pictures of {about}."
        if folder == "p4":
            intro += (" Its planes are those its own radio hears (**simulated radio**), but for a plane's card and "
                      "a plane followed, which show what a receiver adds: the type and registration.")
        lines += [intro, "", "<table>"]
        for i in range(0, len(shown), columns):
            lines.append("<tr>")
            for s in shown[i:i + columns]:
                lines.append(f'<td width="33%" valign="top"><img src="{screens.name}/{folder}/{s.name}.png" '
                             f'alt="{s.title}" width="100%"><br><b>{s.title}</b><br>{s.caption_for(folder)}</td>')
            lines.append("</tr>")
        lines += ["</table>", ""]
    page.write_text("\n".join(lines))
    return page


def board_of(screen: Screen) -> str:
    """The board's name, from the manifest its firmware asks for updates (…/<board>/manifest.json)."""
    source = (screen.json("/api/firmware")[1] or {}).get("source", "")
    return next((b for b in BOARDS if f"/{b}/" in source), "")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", nargs="?", help="the screen's address (an IP or name.local)")
    parser.add_argument("--board", choices=sorted(BOARDS), help="the board (else from its firmware)")
    parser.add_argument("--out", default=str(ROOT / "docs" / "screens"), help="where the boards' folders go")
    parser.add_argument("--only", default="", help="these scenes alone, by name, with commas between")
    parser.add_argument("--port", type=int, default=8095, help="this machine's port for the screen's requests")
    parser.add_argument("--list", action="store_true", help="the scenes, and nothing taken")
    parser.add_argument("--page", action="store_true", help="screens.md from the pictures there now, nothing taken")
    parser.add_argument("--round", action="store_true",
                        help="the round boards' pictures there now made round, nothing taken")
    args = parser.parse_args()
    screens = Path(args.out)
    if args.round:
        for folder in sorted(ROUND):
            if (screens / folder).is_dir():
                print(f"{screens / folder}: {made_round(screens / folder)} made round")
        return
    if args.page:
        print(write_page(screens))
        return
    if args.list or not args.address:
        for s in SCENES:
            print(f"{s.name:15} {s.place:6} {s.title}{'  (' + ', '.join(s.boards) + ')' if s.boards else ''}")
        return
    sys.stdout.reconfigure(line_buffering=True)
    only = {s for s in args.only.split(",") if s}
    if only - {s.name for s in SCENES}:
        raise SystemExit(f"No scene {', '.join(sorted(only - {s.name for s in SCENES}))}: --list names them")

    screen = Screen(args.address)
    if wait_for(screen.up, 30, 1) is None:
        raise SystemExit(f"No screen answers at {screen.base}")
    status = screen.json("/api/status")[1]
    if status.get("divert") is None:
        raise SystemExit("This firmware has no test API (test_api: false)")
    if screen.call("/api/screenshot", timeout=20)[0] != 200:
        raise SystemExit("This firmware takes no screenshot (GET /api/screenshot): install a newer one")
    board = args.board or board_of(screen)
    if board not in BOARDS:
        raise SystemExit("Which board is it? --board names it")
    folder = BOARDS[board][0]
    has_radio = "radio" in status
    if has_radio and screen.json("/api/test", {"radio": []})[0] != 200:
        raise SystemExit("This firmware takes no made-up messages for its radio: install a newer one")
    if screen.json("/api/test", {"tally": "back"})[0] != 200:
        raise SystemExit("This firmware cannot set the day's tally aside: install a newer one")
    out = screens / folder
    out.mkdir(parents=True, exist_ok=True)
    scenes = [s for s in SCENES if (not only or s.name in only) and (not s.boards or folder in s.boards)]
    print(f"{BOARDS[board][1]}: {len(scenes)} scenes{', its planes from its radio' if has_radio else ''}")

    saved = device_test.kept_settings(args.address)
    if saved.exists():
        raise SystemExit(f"A run before could not put the settings back: tests/device_test.py {args.address} "
                         f"--restore ({saved})")
    status, original = screen.json("/api/settings")
    if status != 200:
        raise SystemExit("The screen gave no settings")
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_text(json.dumps(original))

    studio = Studio(args.port).start()
    origin = f"http://{device_test.own_address_toward(args.address)}:{studio.port}"
    radio = RadioSender(screen, studio).start() if has_radio else None
    glass = Glass(screen)
    skies = {}
    failed = 0
    rain = None
    last_area = None

    def shoot(scene: Scene):
        nonlocal rain, last_area, failed
        if scene.place == "rain":
            rain = rain or rainy_place()
            if not rain:
                print(f"{scene.name}: no rain reported at any of the airports looked at; left out")
                return
            code = rain[0]
            PLACES.setdefault(code, (rain[1], rain[2], rain[3], 0, "us", rain[4]))
        else:
            code = scene.place
        lat, lon, elevation, _, country, town = PLACES[code]
        print(f"{scene.name}: {scene.title} at {code}")
        key = (code, scene.settings["range_nm"])
        fresh = key not in skies
        if fresh:
            skies[key] = Sky(code, lat, lon, elevation, runway_for(code), scene.settings["range_nm"], country == "uk")
        sky = skies[key]
        studio.place(sky, lat, lon, town)
        full = {**COMMON, "lat": lat, "lon": lon, "located": True, "place": code, **scene.settings}
        if has_radio and not scene.details:
            full["feed"] = "radio"
        else:
            full.update({"feed": "receiver", "receiver_url": origin + "/data/aircraft.json"})
        if has_radio and "volume" in original:
            full["volume"] = 0  # no chimes for the made-up alerts
        if scene.action == "watch":
            shown = [p for p in sky.aircraft() if dist_nm(lat, lon, p["lat"], p["lon"]) < full["range_nm"] * 0.6
                     and p["alt_baro"] > 3000]
            if shown:
                full["watch"] = [{"id": shown[0]["flight"].strip(), "color": "purple"}]
        area = (code, full["range_nm"], full.get("airspace", False))
        before = None if area == last_area else screen.json("/api/status")[1]["airspace_rev"]
        last_area = area
        status, answer = screen.json("/api/settings", full)
        if status != 200:
            print(f"    settings refused: {answer}")
            failed += 1
            return
        wait_drawn(screen, full, before)
        # Long enough for the planes' tracks to grow behind them, the first time at a place.
        time.sleep(40 if fresh else 8)
        try:
            if not touch_for(screen, glass, scene.action, sky, lambda: take(screen, out, scene.name)):
                print(f"    the touch for {scene.action} did not take, or no picture")
                failed += 1
        finally:
            undo_touch(screen, glass, scene.action)

    try:
        print("Sent here for every request (the screen restarts)")
        if not device_test.divert(screen, origin):
            raise SystemExit(f"The screen did not come back sent here: can it reach {origin}?")
        screen.json("/api/test", {"tally": "aside"})
        for scene in scenes:
            if not scene.after:
                shoot(scene)
    finally:
        screen.json("/api/test", {"tally": "back"})
        print("The real day's tally back, and the divert lifted (the screen restarts)")
        if not device_test.divert(screen, ""):
            print("THE SCREEN IS STILL SENT HERE: power it off and on")
            failed += 1
    try:
        for scene in scenes:
            if scene.after:
                if (screen.json("/api/card")[1] or {}).get("mounted"):
                    shoot(scene)
                else:
                    print(f"{scene.name}: no map card in the screen; left out")
    finally:
        if radio:
            radio.stop()
        studio.stop()
        if studio.unknown:
            print(f"Asked and not known here: {len(studio.unknown)}, such as {sorted(set(studio.unknown))[:5]}")
        print(f"Passed on to the real services: {studio.passed} ({studio.failed} failed)")
        changed = not screen.up() or screen.json("/api/settings")[1] != original
        if not changed or device_test.put_back(screen, original):
            saved.unlink()
            print("Settings put back")
        else:
            print(f"SETTINGS NOT PUT BACK: kept in {saved}; tests/device_test.py {args.address} --restore")
            failed += 1
    print(write_page(screens))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
