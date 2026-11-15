"""What a radio at 1090 MHz would hear from made-up planes: each plane's ADS-B messages (extended squitters, DF17) and
its identity replies (DF5), written in hex as receivers log them. A test hands them to a screen with a radio
(POST /api/test {"radio": [...]}, radio.h hear), which reads them as it reads what its stick hears, so a picture of
the glass can show the radio's planes without a real sky (tools/screenshots.py).

The planes are those of tests/mock_feed.py and tools/screenshots.py, as an exchange reports them ("hex", "flight",
"lat", "lon", "alt_baro", "gs", "track", "baro_rate", "squawk", "category"). Field layouts and the CPR arithmetic are
ICAO Doc 9871's, as set out in "The 1090 Megahertz Riddle" (Junzi Sun), and as the screen reads them (adsb.h).

    python3 tests/mock_radio.py <file>   writes a few planes' messages, one a line after the plane's fields, for the
                                         host tests to read with the screen's own decoder (tests/run.sh)

Python's standard library only.
"""

import json
import math
import sys

# ---------------------------------------------------------------- the checksum


def _table():
    out = []
    for i in range(256):
        c = i << 16
        for _ in range(8):
            c = ((c << 1) ^ 0xFFF409) if c & 0x800000 else c << 1
        out.append(c & 0xFFFFFF)
    return out


TABLE = _table()


def crc(data: bytes) -> int:
    """Mode S's CRC-24 (generator 0x1FFF409) over `data`."""
    c = 0
    for b in data:
        c = ((c << 8) ^ TABLE[((c >> 16) ^ b) & 0xFF]) & 0xFFFFFF
    return c


class Bits:
    """Bits written first to last, as the standards number them."""

    def __init__(self):
        self.value, self.count = 0, 0

    def put(self, value: int, count: int):
        assert 0 <= value < (1 << count), (value, count)
        self.value = self.value << count | value
        self.count += count
        return self


def squitter(address: int, me: Bits) -> str:
    """A DF17 extended squitter (capability 5, airborne) from `address` carrying the 56 bits of `me`."""
    assert me.count == 56
    body = bytes([17 << 3 | 5]) + address.to_bytes(3, "big") + me.value.to_bytes(7, "big")
    return (body + crc(body).to_bytes(3, "big")).hex().upper()


# ---------------------------------------------------------------- fields

CHARS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######"


def identification(address: int, callsign: str, category: str) -> str:
    """Type codes 1 to 4: the callsign and the kind of aircraft (A1 light, A3 large, A5 heavy, A7 helicopter...)."""
    tc = 4 - (ord(category[0]) - ord("A")) if category else 4
    me = Bits().put(tc, 5).put(int(category[1]) if category else 0, 3)
    for ch in f"{callsign.strip().upper()[:8]:<8}":
        me.put(CHARS.index(ch) if ch in CHARS and ch != "#" else CHARS.index(" "), 6)
    return squitter(address, me)


def altitude12(feet: float) -> int:
    """A barometric altitude in 25-foot steps with its Q bit, as ME bits 9 to 20 carry it."""
    n = max(0, min(int(round((feet + 1000) / 25)), 0x7FF))
    return (n & 0x7F0) << 1 | 0x10 | (n & 0xF)


def zones(lat: float) -> int:
    """NL: the number of longitude zones at a latitude, for 15 latitude zones a hemisphere (adsb.h zones)."""
    lat = abs(lat)
    if lat < 1e-9:
        return 59
    if lat > 87:
        return 1
    if lat == 87:
        return 2
    a, c = 1 - math.cos(math.pi / 30), math.cos(math.pi / 180 * lat)
    return int(math.floor(2 * math.pi / math.acos(1 - a / (c * c))))


def cpr(lat: float, lon: float, odd: bool):
    """An airborne compact position report: 17 bits each of latitude and longitude within the zone."""
    i = 1 if odd else 0
    dlat = 360.0 / (60 - i)
    yz = math.floor(131072 * (lat % dlat) / dlat + 0.5)
    rlat = dlat * (yz / 131072 + math.floor(lat / dlat))
    dlon = 360.0 / max(zones(rlat) - i, 1)
    xz = math.floor(131072 * (lon % dlon) / dlon + 0.5)
    return yz & 0x1FFFF, xz & 0x1FFFF


def position(address: int, lat: float, lon: float, feet: float, odd: bool) -> str:
    """Type code 11: an airborne position with its barometric altitude."""
    y, x = cpr(lat, lon, odd)
    me = Bits().put(11, 5).put(0, 2).put(0, 1).put(altitude12(feet), 12).put(0, 1).put(1 if odd else 0, 1)
    return squitter(address, me.put(y, 17).put(x, 17))


def velocity(address: int, knots: float, track: float, climb: float) -> str:
    """Type code 19, subtype 1: the speed over the ground east and north, and the rate of climb (feet a minute)."""
    east = knots * math.sin(math.radians(track))
    north = knots * math.cos(math.radians(track))
    ew, ns = min(int(round(abs(east))) + 1, 1023), min(int(round(abs(north))) + 1, 1023)
    vr = min(int(round(abs(climb) / 64)) + 1, 511)
    me = Bits().put(19, 5).put(1, 3).put(0, 1).put(0, 1).put(0, 3)
    me.put(1 if east < 0 else 0, 1).put(ew, 10).put(1 if north < 0 else 0, 1).put(ns, 10)
    me.put(0, 1).put(1 if climb < 0 else 0, 1).put(vr, 9).put(0, 2).put(0, 1).put(0, 7)
    return squitter(address, me)


def identity13(squawk: str) -> int:
    """A squawk's four digits as the 13 bits of an identity code: C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4."""
    a, b, c, d = (int(ch) for ch in squawk)
    bit = lambda digit, k: (digit >> k) & 1
    order = [bit(c, 0), bit(a, 0), bit(c, 1), bit(a, 1), bit(c, 2), bit(a, 2), 0, bit(b, 0), bit(d, 0), bit(b, 1),
             bit(d, 1), bit(b, 2), bit(d, 2)]
    out = 0
    for v in order:
        out = out << 1 | v
    return out


def identity_reply(address: int, squawk: str) -> str:
    """A DF5 surveillance identity reply: the squawk, its parity overlaid with the sender's address."""
    body = (Bits().put(5, 5).put(0, 3).put(0, 5).put(0, 6).put(identity13(squawk), 13).value).to_bytes(4, "big")
    return (body + (crc(body) ^ address).to_bytes(3, "big")).hex().upper()


def emergency(address: int, squawk: str) -> str:
    """Type code 28, subtype 1: an emergency declared (a general one; 7500 and 7600 their own), with the squawk."""
    state = {"7500": 5, "7600": 4}.get(squawk, 1)
    me = Bits().put(28, 5).put(1, 3).put(state, 3).put(identity13(squawk), 13).put(0, 32)
    return squitter(address, me)


# ---------------------------------------------------------------- planes


def messages(plane: dict, odd: bool) -> list:
    """What a plane sends in a second or so, as an exchange reports it: its identity, a position (even or odd), its
    velocity, and its squawk; an emergency besides when it squawks one."""
    address = int(plane["hex"].lstrip("~"), 16)
    out = []
    if plane.get("flight", "").strip():
        out.append(identification(address, plane["flight"], plane.get("category", "")))
    feet = plane.get("alt_baro", 0)
    if isinstance(feet, (int, float)):
        out.append(position(address, plane["lat"], plane["lon"], feet, odd))
    if "gs" in plane and "track" in plane:
        out.append(velocity(address, plane["gs"], plane["track"], plane.get("baro_rate", 0)))
    squawk = str(plane.get("squawk", ""))
    if len(squawk) == 4 and squawk.isdigit() and "8" not in squawk and "9" not in squawk:
        out.append(identity_reply(address, squawk))
        if squawk in ("7500", "7600", "7700"):
            out.append(emergency(address, squawk))
    return out


def heard(planes: list, odd: bool) -> list:
    """Every plane's messages for one second: even positions one second, odd the next, so a pair comes in two."""
    return [m for p in planes for m in messages(p, odd)]


# Planes for the host tests (test_host.cpp reads them back): north, south, east and west of the place, either side of
# the date line's longitude zones and the equator's, climbing, descending and in an emergency.
SAMPLES = [
    {"hex": "a1b2c3", "flight": "DAL1234 ", "lat": 40.7012, "lon": -73.6511, "alt_baro": 12500, "gs": 280, "track": 45.0,
     "baro_rate": 1536, "squawk": "4217", "category": "A3"},
    {"hex": "abcdef", "flight": "N905NA", "lat": 40.5521, "lon": -73.9420, "alt_baro": 3000, "gs": 150, "track": 300.0,
     "baro_rate": -768, "squawk": "7700", "category": "A5"},
    {"hex": "400a1b", "flight": "BAW286", "lat": 51.4700, "lon": -0.4543, "alt_baro": 37000, "gs": 470, "track": 260.0,
     "baro_rate": 0, "squawk": "1200", "category": "A5"},
    {"hex": "7c1234", "flight": "QFA1", "lat": -33.9461, "lon": 151.1772, "alt_baro": 800, "gs": 140, "track": 160.0,
     "baro_rate": -640, "squawk": "3000", "category": "A5"},
    {"hex": "c0ffee", "flight": "", "lat": 0.0213, "lon": -179.9876, "alt_baro": 25000, "gs": 330, "track": 90.0,
     "squawk": "0000", "category": "A1"},
]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    with open(sys.argv[1], "w") as f:
        for p in SAMPLES:
            f.write(json.dumps(p) + "\n")
            for odd in (False, True):
                for m in messages(p, odd):
                    f.write(m + "\n")
            f.write("\n")


if __name__ == "__main__":
    main()
