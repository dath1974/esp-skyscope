"""The public services a screen asks, stood in for by tests/mock_feed.py, so tests on a real screen ask none of them.

A screen sent to the mock (the test API's /api/test {"divert": "http://<mock>:8095"}) asks it every address it would
have asked the world, as /<server>/<path>: http://<mock>:8095/tile.openstreetmap.org/9/150/192.png. `answer` gives
what each service would, in its own form, at the mock's place: map tiles of every map (a checkered square, each its own
colour; the layers of place names a few marks on nothing), the FAA's airspace and runways and its airports, places by
name, the weather (METAR, TAF, MOS, SIGMETs, G-AIRMETs, pilot reports), the weather radar's latest scan and its tiles,
and the ADS-B exchanges' planes. What each is is known, so a test can say exactly what the screen should make of it:

    AREAS (Class B, C and two D), RUNWAYS (three, two at a towered airport), STATION, AIRPORT, PLACE_NAME

Anything else is 404, noted as "unknown" so a test can say nothing went unanswered. Python's standard library only.
"""

import json
import math
import struct
import time
import urllib.parse
import zlib

NM_PER_DEGREE = 60.0

# The airspace round the mock's place: (class, its airport's id, its middle north and east in NM, its half-width in NM).
AREAS = [("B", "MOCK-B-AIRPORT", 0, 0, 12), ("C", "MOCK-C-AIRPORT", 20, 25, 6), ("D", "MOCK-D1-AIRPORT", -15, -10, 3),
         ("D", "MOCK-D2-AIRPORT", 10, -20, 3)]
# A Class E, which the screen passes over.
PASSED_OVER = [("E", "MOCK-E-AIRPORT", -25, 25, 5)]
# Runways: (their airport, one end and the other north and east of the place in NM, length in feet). The first two at
# the Class B's airport (towered), the last at an airport of no airspace's.
RUNWAYS = [("MOCK-B-AIRPORT", (-1.0, -0.6), (0.6, 1.0), 12000), ("MOCK-B-AIRPORT", (-0.8, 0.8), (0.4, -0.4), 8000),
           ("MOCK-X-AIRPORT", (8.0, 8.0), (8.4, 8.2), 3000)]
STATION = "KTST"  # the weather station at the place, and its airport
AIRPORT = {"IDENT": "TST", "ICAO_ID": STATION, "NAME": "MOCK TEST INTL", "SERVCITY": "TESTVILLE", "STATE": "NY"}
PLACE_NAME = "Testville"

# The servers each map's tiles come from, as tiles.h names them, by map setting.
TILE_SERVERS = {"streets": "server.arcgisonline.com", "satellite": "server.arcgisonline.com",
                "osm": "tile.openstreetmap.org", "topo": "basemap.nationalmap.gov", "sectional": "tiles.arcgis.com",
                "ifr_low": "tiles.arcgis.com", "ifr_high": "tiles.arcgis.com"}


def offset(lat: float, lon: float, north_nm: float, east_nm: float):
    return lat + north_nm / NM_PER_DEGREE, lon + east_nm / (NM_PER_DEGREE * math.cos(math.radians(lat)))


def png(width: int, height: int, rows_rgba) -> bytes:
    """A PNG of 8-bit RGBA rows."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\x00" + row for row in rows_rgba)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def tile_png(z: int, x: int, y: int, layer: bool = False) -> bytes:
    """A 256-pixel tile. A map's: squares of 32 in two shades of a colour of its own, so a tile out of place shows. A
    layer over a map (place names, the weather radar): nothing but a small mark in its middle."""
    seed = (z * 73856093) ^ (x * 19349663) ^ (y * 83492791)
    if layer:
        clear, mark = bytes(4), bytes((40, 160, 40, 200))
        return png(256, 256, [(clear * 120 + mark * 16 + clear * 120) if 120 <= r < 136 else clear * 256
                              for r in range(256)])
    base = (96 + seed % 128, 96 + (seed >> 8) % 128, 96 + (seed >> 16) % 128)
    light, dark = bytes(base) + b"\xff", bytes(max(0, c - 40) for c in base) + b"\xff"
    return png(256, 256, [b"".join((light if (r // 32 + col) % 2 else dark) * 32 for col in range(8))
                          for r in range(256)])


def square(lat, lon, north, east, half):
    """A closed ring of [lon, lat], as ArcGIS writes them."""
    corners = [(north - half, east - half), (north - half, east + half), (north + half, east + half),
               (north + half, east - half), (north - half, east - half)]
    return [[round(p[1], 5), round(p[0], 5)] for p in (offset(lat, lon, n, e) for n, e in corners)]


def airspace(lat, lon) -> dict:
    features = [{"attributes": {"CLASS": c, "ADHP_ID": a}, "geometry": {"rings": [square(lat, lon, n, e, h)]}}
                for c, a, n, e, h in AREAS + PASSED_OVER]
    return {"objectIdFieldName": "OBJECTID", "geometryType": "esriGeometryPolygon", "features": features}


def runways(lat, lon) -> dict:
    features = []
    for airport, a, b, length in RUNWAYS:
        # A thin outline along the runway, as ADDS draws them.
        (la, oa), (lb, ob) = offset(lat, lon, *a), offset(lat, lon, *b)
        w = 0.0002
        ring = [[oa - w, la], [ob - w, lb], [ob + w, lb], [oa + w, la], [oa - w, la]]
        features.append({"attributes": {"AIRPORT_ID": airport, "LENGTH": length},
                         "geometry": {"rings": [[[round(x, 6), round(y, 6)] for x, y in ring]]}})
    return {"features": features}


def iso(seconds: float) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(seconds))


def metar(lat, lon, now) -> dict:
    observed = int(now // 3600 * 3600 - 9 * 60)
    stamp = time.strftime("%d%H%M", time.gmtime(observed))
    return {"icaoId": STATION, "obsTime": observed, "temp": 15.0, "dewp": 9.0, "wdir": 270, "wspd": 10, "visib": "10+",
            "altim": 1015.2, "rawOb": f"METAR {STATION} {stamp}Z 27010KT 10SM FEW040 BKN250 15/09 A2998",
            "lat": round(lat, 4), "lon": round(lon, 4), "name": "Mock Test Intl, NY, US",
            "clouds": [{"cover": "FEW", "base": 4000}, {"cover": "BKN", "base": 25000}], "fltCat": "VFR"}


def weather(kind: str, lat, lon, now):
    """What the Aviation Weather Center answers for `kind` (metar, taf, airsigmet, gairmet, pirep)."""
    if kind == "metar":
        return [metar(lat, lon, now)]
    if kind == "taf":
        issued = now // 3600 * 3600 - 3600
        day = time.strftime("%d%H", time.gmtime(issued))
        return [{"icaoId": STATION, "issueTime": iso(issued), "rawTAF": f"TAF {STATION} {day}00Z 27010KT P6SM FEW040 "
                 f"FM{time.strftime('%d%H', time.gmtime(issued + 6 * 3600))}00 30012KT P6SM SCT050 "
                 f"TEMPO {time.strftime('%d%H', time.gmtime(issued + 9 * 3600))}/"
                 f"{time.strftime('%d%H', time.gmtime(issued + 11 * 3600))} BKN030"}]
    around = [{"lat": round(p[0], 3), "lon": round(p[1], 3)}
              for p in (offset(lat, lon, n, e) for n, e in ((-60, -60), (-60, 60), (60, 60), (60, -60)))]
    if kind == "airsigmet":
        return [{"airSigmetType": "SIGMET", "hazard": "CONVECTIVE", "altitudeHi1": 35000, "altitudeLow1": None,
                 "validTimeTo": int(now + 2 * 3600), "rawAirSigmet": "CONVECTIVE SIGMET 1E (MOCK)", "coords": around}]
    if kind == "gairmet":
        return [{"forecastHour": 0, "hazard": "TURB-LO", "base": "SFC", "top": "120", "severity": "MOD",
                 "expireTime": int(now + 3 * 3600), "coords": [{k: str(v) for k, v in c.items()} for c in around]}]
    if kind == "pirep":
        return [{"rawOb": f"TST UA /OV {STATION}/TM {time.strftime('%H%M', time.gmtime(now - 1200))}/FL080/TP B738/"
                 "TB LGT", "acType": "B738", "fltLvl": 80, "lat": round(lat, 3), "lon": round(lon, 3),
                 "obsTime": int(now - 1200)}]
    return None


def mos(now) -> dict:
    """The GFS MOS as the Iowa Environmental Mesonet gives it: every three hours for two days."""
    start = int(now // 10800 * 10800)
    rows = [{"ftime": time.strftime("%Y-%m-%d %H:%M", time.gmtime(start + i * 10800)), "tmp": 55 + i % 8,
             "dpt": 45 + i % 5, "cld": ("CL", "FW", "SC", "BK")[i % 4], "wdr": (240 + 10 * i) % 360, "wsp": 8 + i % 6,
             "p06": (i * 7) % 40, "cig": 8 - i % 3, "vis": 7} for i in range(16)]
    return {"schema": {}, "data": rows}


def radar_latest(now) -> dict:
    """IEM's note of the latest scan: every five minutes, the last a few minutes ago."""
    valid = int((now - 180) // 300 * 300)
    return {"meta": {"vcp": None, "product": "N0Q", "valid": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(valid))}}


def airports(where: str, lat, lon) -> dict:
    """The FAA's airports for a query: the mock's airport when the query names it, else none."""
    words = where.upper()
    if not any(w in words for w in (STATION, f"'{AIRPORT['IDENT']}'", "MOCK", "TEST")):
        return {"features": []}
    return {"features": [{"attributes": AIRPORT, "geometry": {"x": round(lon, 5), "y": round(lat, 5)}}]}


def places(q: str, lat, lon) -> list:
    """Nominatim's places for a search: one town, a little north of the mock's place, whatever was asked."""
    north = offset(lat, lon, 6, 0)
    return [{"lat": f"{north[0]:.5f}", "lon": f"{north[1]:.5f}", "display_name": f"{PLACE_NAME}, Test County, "
             "New York, United States", "class": "boundary", "type": "administrative", "extratags": {}}]


def tile_numbers(parts, xy: bool):
    """z, x, y from the end of a tile's path: {z}/{x}/{y}(.png) or ArcGIS's {z}/{y}/{x}."""
    z, a, b = (int(p.split(".")[0]) for p in parts[-3:])
    return (z, a, b) if xy else (z, b, a)


def answer(path: str, lat: float, lon: float, now: float, aircraft):
    """(status, content type, body, what it was) for `path` (/<server>/<rest>), or None for a path that names no
    server (the mock's own). `aircraft()` gives the planes, for the exchanges."""
    parsed = urllib.parse.urlsplit(path)
    parts = parsed.path.strip("/").split("/")
    server = parts[0]
    if "." not in server:
        return None
    query = urllib.parse.parse_qs(parsed.query)
    rest = "/".join(parts[1:])
    as_json = lambda what, body: (200, "application/json", json.dumps(body).encode(), what)
    try:
        if server in ("tile.openstreetmap.org",):
            return 200, "image/png", tile_png(*tile_numbers(parts, True)), "tile"
        if server in ("server.arcgisonline.com", "basemap.nationalmap.gov", "tiles.arcgis.com"):
            layer = "Reference" in rest or "Boundaries_and_Places" in rest
            return 200, "image/png", tile_png(*tile_numbers(parts, False), layer=layer), "place-names" if layer else "tile"
        if server == "services6.arcgis.com":
            if "/Class_Airspace/" in parsed.path:
                return as_json("airspace", airspace(lat, lon))
            if "/Runways/" in parsed.path:
                return as_json("runways", runways(lat, lon))
            if "/US_Airport/" in parsed.path:
                return as_json("airports", airports(" ".join(query.get("where", [""])), lat, lon))
        if server == "nominatim.openstreetmap.org" and rest == "search":
            return as_json("places", places(" ".join(query.get("q", [""])), lat, lon))
        if server == "aviationweather.gov" and rest.startswith("api/data/"):
            kind = rest.removeprefix("api/data/")
            found = weather(kind, lat, lon, now)
            if found is not None:
                return as_json(kind, found)
        if server == "mesonet.agron.iastate.edu":
            if rest == "api/1/mos.json":
                return as_json("mos", mos(now))
            if rest.endswith("USCOMP/n0q_0.json"):
                return as_json("radar-latest", radar_latest(now))
            if rest.startswith("cache/tile.py/"):
                return 200, "image/png", tile_png(*tile_numbers(parts, True), layer=True), "radar-tile"
        if server in ("opendata.adsb.fi", "api.adsb.lol"):
            return as_json("exchange", {"now": now, "ac": aircraft()})
    except (ValueError, IndexError):
        pass
    return 404, "application/json", b'{"msg":"the mock has nothing here"}', "unknown"


def write_fixtures(folder: str, lat: float = 40.6413, lon: float = -73.7781, now: float = 1793347800):
    """Each service's answer as a file, for the host tests to read with the screen's own parsers (tests/run.sh)."""
    import os

    os.makedirs(folder, exist_ok=True)
    plane = [{"hex": "e00001", "flight": "MOCK1", "lat": lat, "lon": lon, "alt_baro": 5000, "track": 90, "seen_pos": 1}]
    paths = {"airspace": "/services6.arcgis.com/x/arcgis/rest/services/Class_Airspace/FeatureServer/0/query?f=json",
             "runways": "/services6.arcgis.com/x/arcgis/rest/services/Runways/FeatureServer/0/query?f=json",
             "airports": "/services6.arcgis.com/x/arcgis/rest/services/US_Airport/FeatureServer/0/query?f=json&where="
                         "ICAO_ID%3D%27KTST%27",
             "places": "/nominatim.openstreetmap.org/search?format=json&q=Testville",
             "metar": "/aviationweather.gov/api/data/metar?format=json&ids=KTST",
             "taf": "/aviationweather.gov/api/data/taf?format=json&ids=KTST",
             "airsigmet": "/aviationweather.gov/api/data/airsigmet?format=json",
             "gairmet": "/aviationweather.gov/api/data/gairmet?format=json",
             "pirep": "/aviationweather.gov/api/data/pirep?format=json",
             "mos": "/mesonet.agron.iastate.edu/api/1/mos.json?model=GFS&station=KTST",
             "radar-latest": "/mesonet.agron.iastate.edu/data/gis/images/4326/USCOMP/n0q_0.json",
             "exchange": "/opendata.adsb.fi/api/v2/lat/40.6/lon/-73.7/dist/25"}
    for name, path in paths.items():
        status, _, body, what = answer(path, lat, lon, now, lambda: plane)
        assert status == 200 and what == name, (name, status, what)
        with open(os.path.join(folder, name + ".json"), "wb") as f:
            f.write(body)


if __name__ == "__main__":
    import sys

    write_fixtures(sys.argv[1])
