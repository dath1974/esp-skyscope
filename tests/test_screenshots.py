"""tools/screenshots.py without a screen: the picture the screen sends read right, the made-up sky where it should be,
and the scenes all known."""

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
    def test_colours_and_rows(self):
        # Odd width (rows padded), the top row first: red, green, blue, white, black, and a middle gray.
        pixels = [0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x8410]
        width, height, rows = png_pixels(screenshots.bmp_to_png(bmp(3, 2, pixels)))
        self.assertEqual((width, height), (3, 2))
        self.assertEqual(rows[0], bytes([255, 0, 0, 0, 255, 0, 0, 0, 255]))
        self.assertEqual(rows[1], bytes([255, 255, 255, 0, 0, 0, 131, 129, 131]))


class MadeUpSky(unittest.TestCase):
    def sky(self, code="KJFK", range_nm=20, runway=None):
        lat, lon, elevation, _, country = screenshots.PLACES[code]
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
        # The final approach: every arrival on the line out from the threshold, descending towards it.
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


class Scenes(unittest.TestCase):
    def test_known(self):
        names = [s[0] for s in screenshots.SCENES]
        self.assertEqual(len(names), len(set(names)))
        for name, place, title, caption, settings, action in screenshots.SCENES:
            self.assertTrue(place == "rain" or place in screenshots.PLACES, name)
            self.assertIn(settings["map"], ("radar", "osm", "streets", "satellite", "topo", "sectional", "ifr_low",
                                            "ifr_high"), name)
            self.assertIn(action, (None, "card", "menu", "weather", "watch"), name)
            self.assertTrue(title and caption.endswith("."), name)
            self.assertTrue(math.isfinite(settings["range_nm"]))


if __name__ == "__main__":
    unittest.main()
