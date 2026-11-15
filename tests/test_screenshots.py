"""tools/screenshots.py without a screen: the picture the screen sends read right, the made-up sky where it should be,
what this machine answers in the world's place, and the scenes all known."""

import json
import math
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import screenshots  # noqa: E402


def bmp(width, height, pixels):
    """A BMP as GET /api/screenshot sends it (probe.cpp): RGB565 in BI_BITFIELDS, 68 bytes of header, the top row first
    and each row padded to four bytes."""
    stride = (width * 2 + 3) & ~3
    header = bytearray(68)
    header[0:2] = b"BM"
    struct.pack_into("<IIIIiiHH", header, 2, 68 + stride * height, 0, 68, 40, width, -height, 1, 16)
    struct.pack_into("<I", header, 30, 3)
    struct.pack_into("<III", header, 54, 0xF800, 0x07E0, 0x001F)
    rows = b"".join(b"".join(struct.pack("<H", v) for v in pixels[y * width:(y + 1) * width]).ljust(stride, b"\0")
                    for y in range(height))
    return bytes(header) + rows


def png_pixels(data):
    """The RGB rows of a PNG this script wrote (one IDAT, filter 0)."""
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    width, height = struct.unpack(">II", data[16:24])
    at, idat = 8, b""
    while at < len(data):
        length = int.from_bytes(data[at:at + 4], "big")
        kind = data[at + 4:at + 8]
        if kind == b"IDAT":
            idat += data[at + 8:at + 8 + length]
        at += 12 + length
    raw = zlib.decompress(idat)
    return width, height, [raw[y * (width * 3 + 1) + 1:(y + 1) * (width * 3 + 1)] for y in range(height)]


class Picture(unittest.TestCase):
    def test_colors_and_rows(self):
        # Odd width (rows padded), the top row first: red, green, blue, white, black, and a middle gray.
        pixels = [0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x8410]
        width, height, rows = png_pixels(screenshots.bmp_to_png(bmp(3, 2, pixels)))
        self.assertEqual((width, height), (3, 2))
        self.assertEqual(rows[0], bytes([255, 0, 0, 0, 255, 0, 0, 0, 255]))
        self.assertEqual(rows[1], bytes([255, 255, 255, 0, 0, 0, 131, 129, 131]))


class MadeUpSky(unittest.TestCase):
    def sky(self, code="KJFK", range_nm=20, runway=None):
        lat, lon, elevation, _, country, _ = screenshots.PLACES[code]
        return screenshots.Sky(code, lat, lon, elevation, runway, range_nm, country == "uk")

    def test_the_same_each_run(self):
        a, b = self.sky(), self.sky()
        b.started = a.started
        self.assertEqual(a.aircraft(a.started + 90), b.aircraft(a.started + 90))

    def test_planes_near_and_whole(self):
        lat, lon = screenshots.PLACES["KJFK"][:2]
        sky = self.sky()
        for later in (0, 300, 1800):
            sky.started -= later
            planes = sky.aircraft()
            self.assertGreaterEqual(len(planes), 12)
            self.assertEqual(len({p["hex"] for p in planes}), len(planes), "each plane its own address")
            for p in planes:
                self.assertLess(screenshots.dist_nm(lat, lon, p["lat"], p["lon"]), 60, p["flight"])
                self.assertTrue(0 <= p["track"] < 360 and p["alt_baro"] >= 0, p)
                self.assertEqual(len(p["flight"]), 8)

    def test_arrivals_on_the_runway_line(self):
        # The final approach: every arrival on the line out from the threshold, descending toward it.
        runway = (40.62, -73.76, 300.0)
        sky = self.sky(runway=runway)
        finals = [h for h, path, _ in sky.planes if path[0] == "final"]
        self.assertTrue(finals)
        for p in sky.aircraft():
            if p["hex"] in finals:
                back = screenshots.bearing_of(runway[0], runway[1], p["lat"], p["lon"])
                self.assertAlmostEqual((back - 120 + 180) % 360 - 180, 0, delta=1)
                self.assertEqual(p["track"], 300.0)
                self.assertLess(p["baro_rate"], 0)

    def test_a_plane_leaving_comes_back_as_another(self):
        sky = self.sky()
        first = {p["flight"]: p["hex"] for p in sky.aircraft()}
        sky.started -= 4 * 3600
        later = {p["flight"]: p["hex"] for p in sky.aircraft()}
        self.assertTrue(any(first[f] != later[f] for f in first if f in later))


    def test_places_their_own_addresses(self):
        # A radio moved from one place to the next never takes a plane of one for another's.
        jfk = {p["hex"] for p in self.sky("KJFK").aircraft()}
        sea = {p["hex"] for p in self.sky("KSEA").aircraft()}
        self.assertFalse(jfk & sea)

    def test_nasa_905_round_the_statue(self):
        sky = self.sky()
        for later in (0, 200, 400, 3600):
            nasa = next(p for p in sky.aircraft(sky.started + later) if p["flight"].strip() == "NASA905")
            self.assertEqual(nasa["hex"], screenshots.NASA["hex"])
            self.assertAlmostEqual(screenshots.dist_nm(*screenshots.LIBERTY, nasa["lat"], nasa["lon"]), 2.5, delta=0.05)
            # Flying round it, at right angles to the line from the statue.
            out = screenshots.bearing_of(*screenshots.LIBERTY, nasa["lat"], nasa["lon"])
            self.assertAlmostEqual(abs((nasa["track"] - out + 180) % 360 - 180), 90, delta=2)
        self.assertNotIn("NASA905 ", [p["flight"] for p in self.sky("KSEA").aircraft()])

    def test_an_emergency_called(self):
        sky = self.sky("KSEA")
        light = next(f["flight"] for _, _, f in sky.planes if f["t"] == "C172")
        sky.squawking.add(light)
        called = [p for p in sky.aircraft() if p["squawk"] == "7700"]
        self.assertEqual([p["flight"].strip() for p in called], [light])


class TheWorld(unittest.TestCase):
    """What this machine answers while the screen is sent here (Studio), without a network: nothing is passed on."""

    def setUp(self):
        self.studio = screenshots.Studio(0)
        lat, lon, elevation, _, _, town = screenshots.PLACES["KJFK"]
        self.studio.place(screenshots.Sky("KJFK", lat, lon, elevation, None, 20, False), lat, lon, town)

    def tearDown(self):
        self.studio.httpd.server_close()

    def get(self, path):
        status, kind, body, _ = self.studio.answer(path, {})
        return status, kind, body

    def test_planes_made_up_and_said_so(self):
        for path in ("/data/aircraft.json", "/opendata.adsb.fi/api/v2/lat/40.6/lon/-73.7/dist/25",
                     "/api.adsb.lol/v2/callsign/NASA905"):
            status, _, body = self.get(path)
            reply = json.loads(body)
            self.assertEqual(status, 200, path)
            self.assertTrue(reply["simulated"], path)
            self.assertTrue(reply.get("aircraft") or reply.get("ac"), path)
        found = json.loads(self.get("/api.adsb.lol/v2/callsign/NASA905")[2])["ac"]
        self.assertEqual([p["r"] for p in found], ["N905NA"])

    def test_nasa_alone_has_a_photo_and_a_route(self):
        photo = json.loads(self.get(f"/api.planespotters.net/pub/photos/hex/{screenshots.NASA['hex'].upper()}")[2])
        self.assertEqual(photo["photos"][0]["photographer"], "NASA")
        self.assertTrue(photo["photos"][0]["link"].startswith("https://"))
        self.assertEqual(json.loads(self.get("/api.planespotters.net/pub/photos/hex/A10010")[2]), {"photos": []})
        status, kind, body = self.get("/cdn.planespotters.net/nasa/ad905a.jpg")
        self.assertEqual((status, kind), (200, "image/jpeg"))
        self.assertTrue(body.startswith(b"\xff\xd8"))
        asked = json.dumps({"planes": [{"callsign": "NASA905", "lat": 40.69, "lng": -74.04},
                                       {"callsign": "DAL123", "lat": 40.6, "lng": -73.7}]}).encode()
        status, _, body = self.studio.post("/api.adsb.lol/api/0/routeset", asked)
        routes = json.loads(body)
        self.assertEqual(routes[0]["airport_codes"], "KIAD-KJFK")
        self.assertEqual(routes[1]["airport_codes"], "unknown")
        self.assertEqual(self.get("/hexdb.io/api/v2/routes/DAL123")[0], 404)

    def test_never_the_screens_own_place(self):
        found = json.loads(self.get("/free.freeipapi.com/api/v1/json")[2])
        self.assertEqual(found["cityName"], "Queens")
        self.assertLess(screenshots.dist_nm(40.6413, -73.7781, found["latitude"], found["longitude"]), 5)

    def test_anything_else_not_known(self):
        self.assertEqual(self.get("/dath1974.github.io/esp-skyscope/x/manifest.json")[0], 404)
        self.assertEqual(self.studio.unknown, ["/dath1974.github.io/esp-skyscope/x/manifest.json"])


class Scenes(unittest.TestCase):
    def test_known(self):
        names = [s.name for s in screenshots.SCENES]
        self.assertEqual(len(names), len(set(names)))
        folders = {b[0] for b in screenshots.BOARDS.values()}
        for s in screenshots.SCENES:
            self.assertTrue(s.place == "rain" or s.place in screenshots.PLACES, s.name)
            self.assertIn(s.settings["map"], ("radar", "osm", "streets", "satellite", "topo", "sectional", "ifr_low",
                                              "ifr_high"), s.name)
            self.assertIn(s.action, screenshots.ACTIONS, s.name)
            self.assertTrue(set(s.boards) <= folders, s.name)
            for board in folders:
                self.assertTrue(s.title and s.caption_for(board).endswith("."), s.name)
            self.assertTrue(math.isfinite(s.settings["range_nm"]))

    def test_the_page(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            screens = Path(d) / "screens"
            for folder, names in (("p4", ("splash", "card")), ("amoled", ("card",))):
                (screens / folder).mkdir(parents=True)
                for n in names:
                    (screens / folder / f"{n}.png").write_bytes(b"")
            page = screenshots.write_page(screens).read_text()
            self.assertLess(page.index("## Waveshare ESP32-P4 4C"), page.index("## Waveshare AMOLED 2.41"))
            self.assertIn('src="screens/p4/splash.png"', page)
            self.assertIn('src="screens/amoled/card.png"', page)
            self.assertNotIn("amoled/splash.png", page)
            self.assertIn("simulated radio", page)


if __name__ == "__main__":
    unittest.main()
