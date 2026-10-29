#!/usr/bin/env python3
"""A stand-in for an ADS-B receiver (readsb's aircraft.json) and for the update server, for tests on a real screen:
aircraft that are known, where they are known to be, a release to offer, and the failures each can have, on demand.

    tests/mock_feed.py [--port 8095] [--lat 40.6413 --lon -73.7781] [--scenario fixed]

Point the screen at it (settings: feed "receiver", receiver_url http://<this machine>:8095/data/aircraft.json).
tests/device_test.py --mock starts one of its own, in the same process. By hand, it takes:

    GET  /data/aircraft.json      the aircraft (any path ending in aircraft.json, and /v2/<lat>/<lon>/<dist>)
    GET  /control                 what it serves, and every request it has had
    POST /control                 {"scenario": ..., "fault": ..., "times": n, "lat": ..., "lon": ...,
                                   "firmware": {"version": ..., "fault": ..., "times": n, "image": "<a file>"}}
    GET  /firmware/manifest.json  a release, as tools/release_site.py writes it (the screen asks it with the test
                                  API's {"source": url} on /api/firmware)
    GET  /firmware/<name>.ota.bin its firmware

Scenarios: "fixed" (a few planes with every kind of field, at known places), "busy" (600 planes with long names),
"crowd" (4500 planes, 600 KB: as much as a screen takes), "moving" (one plane flying east at 480 kt), "odd"
(what a receiver should not send but may: wrong types, nulls, duplicates, words past their length in UTF-8, places off
the earth, entries that are no plane), "empty". Faults, for the next `times` requests (0: until changed): "429"
(Retry-After), "429-reset" (x-ratelimit-reset), "403" (refused, saying nothing more), "paced" (a good answer with
x-ratelimit-limit 1;w=20), "500", "404", "slow" (15 s before answering, past the screen's 12 s), "truncated" (stops half-way through the body
it announced), "bad-json", "huge" (a body of 1 MB), "close" (the connection closed without an answer), "no-list"
({"msg": ...} and no aircraft), "html" (a web page where the JSON should be, as a captive portal sends), "blank" (200
and no body). And answers that are good but sent another way: "chunked" (no length, in chunks, as nginx sends),
"redirect" (302 to where the list is).

The release's firmware is a stand-in unless an image is given: an image's first byte and then noise, so a screen takes
it in to the end and refuses it there, keeping its own. Its faults, for the next `times` manifests or images: "500"
(the manifest), "bad-json", "no-build" (none for this chip), "no-ota" (a build without its download); "bin-404",
"bin-md5" (a digest that is not the image's), "bin-truncated" (cut off half-way), "bin-close" (no answer at all).

Python's standard library only.
"""

import argparse
import hashlib
import http.server
import json
import math
import threading
import time

NM_PER_DEGREE = 60.0


def offset(lat: float, lon: float, north_nm: float, east_nm: float):
    """The place `north_nm` north and `east_nm` east of lat, lon."""
    return lat + north_nm / NM_PER_DEGREE, lon + east_nm / (NM_PER_DEGREE * math.cos(math.radians(lat)))


# The fixed scenario's planes: where they are from the middle (nautical miles north, east) and what they say.
FIXED = [
    ("a00001", 5, 0, {"flight": "NORTH5  ", "r": "N10001", "t": "B738", "alt_baro": 10000, "gs": 300, "track": 90,
                      "baro_rate": 1500, "squawk": "1200", "category": "A3"}),
    ("a00002", 0, 10, {"flight": "EAST10  ", "r": "N10002", "t": "A320", "alt_baro": 24000, "gs": 420, "track": 270}),
    ("a00003", -10, 0, {"flight": "SOUTH10 ", "r": "N10003", "t": "C172", "alt_baro": 3000, "gs": 110, "track": 0,
                        "desc": "CESSNA 172 SKYHAWK", "ownOp": "A FLYING CLUB WITH A RATHER LONG NAME INDEED"}),
    ("a00004", 0, -5, {"flight": "EMERG1  ", "r": "N10004", "t": "B77W", "alt_baro": 8000, "gs": 250, "track": 45,
                       "squawk": "7700", "emergency": "general"}),
    ("a00005", 2, 2, {"flight": "GROUND1 ", "r": "N10005", "alt_baro": "ground", "gs": 12, "track": 180}),
    ("a00006", 7, -7, {"flight": "HIGH41  ", "r": "N10006", "t": "GLF6", "alt_baro": 41000, "gs": 510, "track": 135}),
    ("a00007", -3, 6, {"flight": "NOALT   ", "r": "N10007"}),
    ("a00008", 4, 4, {"flight": "WATCH1  ", "r": "N12345", "t": "PC12", "alt_baro": 15000, "gs": 260, "track": 300}),
    # One of each kind the screen draws (shapes.h), known by its type, its class or its description.
    ("a00020", -6, -3, {"flight": "HELI1", "t": "EC35", "alt_baro": 1500, "gs": 110, "track": 200}),
    ("a00021", -4, -8, {"flight": "HELI2", "category": "A7", "alt_baro": 900, "gs": 90, "track": 20}),
    ("a00022", -9, -6, {"flight": "QUAD1", "t": "B744", "category": "A5", "alt_baro": 33000, "gs": 480, "track": 60}),
    ("a00023", 8, 3, {"flight": "GLDR1", "category": "B1", "alt_baro": 4000, "gs": 50, "track": 270}),
    ("a00024", -7, 7, {"flight": "BLN1", "category": "B2", "alt_baro": 2500}),  # no heading: drawn all the same
    ("a00025", 3, -8, {"flight": "DRN1", "category": "B6", "alt_baro": 300, "gs": 30, "track": 90}),
    ("a00026", 9, -2, {"flight": "DRN2", "category": "A2", "desc": "GENERAL ATOMICS MQ-9 REAPER", "alt_baro": 19000,
                       "gs": 200, "track": 180}),
    ("a00027", -8, 1, {"flight": "FAST1", "t": "F16", "alt_baro": 19500, "gs": 450, "track": 10}),
    ("a00028", 6, 9, {"flight": "BLMP1", "category": "B2", "desc": "GOODYEAR AIRSHIP", "alt_baro": 1200, "gs": 30,
                      "track": 120}),
    ("a00029", 1, -1, {"flight": "VEH1", "category": "C2", "alt_baro": "ground", "gs": 15, "track": 90}),
]
# What each fixed plane is drawn as, by type.
KINDS = {"a00001": "airliner", "a00002": "airliner", "a00003": "light", "a00004": "heavy", "a00005": "airliner",
         "a00006": "jet", "a00007": "airliner", "a00008": "turboprop", "a00020": "helicopter", "a00021": "helicopter",
         "a00022": "quad", "a00023": "glider", "a00024": "balloon", "a00025": "drone", "a00026": "drone",
         "a00027": "fast", "a00028": "airship", "a00029": "vehicle"}
# The odd scenario: (what the entry is, whether it is a plane to keep). Positions from the middle, in NM, as FIXED.
ODD = [
    ({"hex": "A0000B", "flight": "UPPER1", "lat": (1, 1), "alt_baro": 5000}, "a0000b"),  # hex in capitals
    ({"hex": "a0000c", "flight": "DUP1", "lat": (-1, -1), "alt_baro": 6000}, "a0000c"),
    ({"hex": "a0000c", "flight": "DUP1", "lat": (-1, -1.1), "alt_baro": 6000}, None),  # the same plane twice
    ({"hex": "a0000d", "flight": "ÄÖÜ✈ÅÉÎ漢字Ω", "r": "Ñ-ÉÇÃ", "t": "ÆØ", "desc": "Ü" * 30, "ownOp": "✈" * 30,
      "lat": (-6, 6), "alt_baro": 7000}, "a0000d"),  # UTF-8 cut at a byte count; apart, for a finger
    ({"hex": "a0000e", "flight": "X" * 80, "lat": (-2, 2), "alt_baro": -1500, "gs": 9999, "track": 725.5,
      "baro_rate": -99999, "squawk": 7700, "category": None}, "a0000e"),  # out of range, a number for a string
    ({"hex": "a0000f", "flight": None, "r": 12345, "lat": (3, -1), "alt_baro": "unknown", "gs": "fast",
      "track": None, "emergency": None, "nested": {"deep": [1, 2, {"x": None}]}}, "a0000f"),
    ({"hex": "a00010", "lat": "40.7", "lon": "-73.7"}, None),  # a place in strings
    ({"hex": "a00011", "lat": 91.5, "lon": -73.7}, None),  # off the earth
    ({"hex": "a00012", "lat": 40.7, "lon": 181}, None),
    ({"hex": "", "lat": (0.5, 0.5)}, None),  # no address
    ({"flight": "NOHEX", "lat": (0.5, -0.5)}, None),
    ({"hex": "a00013", "lat": (1.5, 0), "seen_pos": "recent"}, "a00013"),
    ({"hex": "a00014", "lat": (0, 1.5), "track": -90, "true_heading": 45, "geom_rate": 500}, "a00014"),
]
ODD_JUNK = [7, "plane", None, [], True]  # entries in the list that are no plane at all

# In the reply but never to be shown: no position, and a position too old.
UNSHOWABLE = [
    {"hex": "a00009", "flight": "NOPOS   ", "alt_baro": 5000},
    {"hex": "a0000a", "flight": "STALE   ", "alt_baro": 5000, "seen_pos": 300},
]


class Feed:
    """What the server serves, and what it was asked."""

    def __init__(self, lat=40.6413, lon=-73.7781, scenario="fixed"):
        self.lock = threading.Lock()
        self.lat, self.lon = lat, lon
        self.scenario = scenario
        self.fault, self.times = "", 0
        self.started = time.time()
        self.requests = []  # (time, path, user agent, what was answered)

    def set(self, scenario=None, fault=None, times=None, lat=None, lon=None):
        with self.lock:
            if scenario is not None:
                self.scenario = scenario
                self.started = time.time()
            if fault is not None:
                self.fault = fault
                self.times = times or 0
            if lat is not None:
                self.lat, self.lon = lat, lon

    def log(self, since: float = 0):
        """The requests since `since`, in the order they came."""
        with self.lock:
            return sorted((r for r in self.requests if r[0] >= since), key=lambda r: r[0])

    def aircraft(self):
        now = time.time()
        if self.scenario == "empty":
            return []
        if self.scenario == "busy":
            planes = []
            for i in range(600):
                # A spiral out to 50 NM: some near, most far, all different.
                r, a = 1 + 49 * (i / 600) ** 0.7, i * 2.399963
                lat, lon = offset(self.lat, self.lon, r * math.cos(a), r * math.sin(a))
                planes.append({"hex": f"b{i:05x}", "flight": f"BUSY{i:<4d}", "r": f"N{20000 + i}", "t": "B738",
                               "alt_baro": 2000 + (i * 97) % 38000, "gs": 200 + i % 300, "track": (i * 37) % 360,
                               "lat": round(lat, 6), "lon": round(lon, 6), "seen_pos": 0.5,
                               "desc": "BOEING 737-800 WITH A NAME LONGER THAN SIXTEEN", "ownOp": f"OPERATOR {i}"})
            return planes
        if self.scenario == "crowd":
            planes = []
            for i in range(4500):
                r, a = 0.5 + 60 * (i / 4500) ** 0.6, i * 2.399963
                lat, lon = offset(self.lat, self.lon, r * math.cos(a), r * math.sin(a))
                planes.append({"hex": f"d{i:05x}", "flight": f"CRWD{i:<4d}", "alt_baro": 1000 + (i * 131) % 40000,
                               "gs": 150 + i % 350, "track": (i * 53) % 360, "lat": round(lat, 5),
                               "lon": round(lon, 5), "seen_pos": 1.2})
            return planes
        if self.scenario == "odd":
            planes = []
            for entry, _ in ODD:
                p = dict(entry)
                if isinstance(p.get("lat"), tuple):
                    p["lat"], p["lon"] = (round(v, 6) for v in offset(self.lat, self.lon, *p["lat"]))
                planes.append(p)
            return planes + ODD_JUNK
        if self.scenario == "moving":
            east = -10 + 480 / 3600 * (now - self.started)  # 480 kt, from 10 NM west
            lat, lon = offset(self.lat, self.lon, 0, east)
            return [{"hex": "c00001", "flight": "MOVER1  ", "alt_baro": 12000, "gs": 480, "track": 90,
                     "lat": round(lat, 6), "lon": round(lon, 6), "seen_pos": 0.2}]
        planes = []
        for hex_, north, east, fields in FIXED:
            lat, lon = offset(self.lat, self.lon, north, east)
            planes.append({"hex": hex_, "lat": round(lat, 6), "lon": round(lon, 6), "seen_pos": 0.3, **fields})
        return planes + UNSHOWABLE

    def take_fault(self) -> str:
        with self.lock:
            fault = self.fault
            if fault and self.times:
                self.times -= 1
                if self.times == 0:
                    self.fault = ""
            return fault


CHIP = "ESP32-S3"  # as tools/release_site.py names the screen's chip
STAND_IN = bytes([0xE9]) + bytes((i * 7919 + 13) % 251 for i in range(256 * 1024 - 1))


class Firmware:
    """The release the update server offers, and what it does wrong."""

    def __init__(self):
        self.lock = threading.Lock()
        self.version, self.image = "9.9.9", STAND_IN
        self.fault, self.times = "", 0

    def set(self, version=None, fault=None, times=None, image=None):
        with self.lock:
            if version is not None:
                self.version = version
            if fault is not None:
                self.fault, self.times = fault, times or 0
            if image is not None:
                self.image = open(image, "rb").read() if image else STAND_IN

    def take_fault(self, kind: str) -> str:
        """The fault for a manifest ("") or an image ("bin-"), used up if it is one of that kind."""
        with self.lock:
            fault = self.fault
            if not fault or fault.startswith("bin-") != (kind == "bin-"):
                return ""
            if self.times:
                self.times -= 1
                if self.times == 0:
                    self.fault = ""
            return fault

    def name(self) -> str:
        return f"skyscope-{self.version}.ota.bin"

    def manifest(self, fault: str) -> dict:
        md5 = hashlib.md5(self.image).hexdigest()
        if self.fault == "bin-md5":
            md5 = hashlib.md5(self.image + b"!").hexdigest()
        ota = {"path": self.name(), "md5": md5, "summary": f"A test release, {self.version}",
               "release_url": f"https://example.com/releases/{self.version}"}
        build = {"chipFamily": "ESP32-C3" if fault == "no-build" else CHIP, "parts": []}
        if fault != "no-ota":
            build["ota"] = ota
        return {"name": "esp-skyscope", "version": self.version, "builds": [build]}


def handler_for(feed: Feed, firmware: Firmware):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"  # keep-alive, as the screen's client uses it

        def log_message(self, *args):
            pass

        def note(self, what: str):
            """The request in the log, at the time it came (an answer may come much later, or never be taken)."""
            with feed.lock:
                feed.requests.append((self.arrived, self.path, self.headers.get("User-Agent", ""), what))

        def answer(self, status, body: bytes, headers=None, what=None):
            if what != "":  # "": noted already
                self.note(what or str(status))
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            self.arrived = time.time()
            if self.path == "/control":
                with feed.lock, firmware.lock:
                    state = {"scenario": feed.scenario, "fault": feed.fault, "times": feed.times,
                             "firmware": {"version": firmware.version, "fault": firmware.fault,
                                          "times": firmware.times, "image": len(firmware.image)},
                             "requests": [{"at": t, "path": p, "agent": a, "answer": w} for t, p, a, w in feed.requests]}
                body = json.dumps(state).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path.startswith("/firmware/"):
                self.firmware()
                return
            if not (self.path.split("?")[0].endswith("aircraft.json") or self.path.startswith("/v2/")):
                self.answer(404, b'{"msg":"not here"}', what="404")
                return
            fault = feed.take_fault()
            reply = {"now": time.time(), "messages": 1000, "aircraft": feed.aircraft()}
            body = json.dumps(reply).encode()
            if fault == "429":
                self.answer(429, b'{"msg":"slow down"}', {"Retry-After": "30"}, "429")
            elif fault == "429-reset":
                self.answer(429, b'{"msg":"quota"}', {"x-ratelimit-limit": "100;w=3600", "x-ratelimit-reset": "25"},
                            "429-reset")
            elif fault == "paced":
                self.answer(200, body, {"x-ratelimit-limit": "1;w=20", "x-ratelimit-reset": "120"}, "paced")
            elif fault == "403":
                self.answer(403, b'{"msg":"forbidden"}', what="403")
            elif fault == "500":
                self.answer(500, b'{"msg":"broken"}', what="500")
            elif fault == "404":
                self.answer(404, b'{"msg":"not here"}', what="404")
            elif fault == "html":
                page = b"<!doctype html><html><body><h1>Sign in to the Wi-Fi</h1></body></html>"
                self.note("html")
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
            elif fault == "blank":
                self.answer(200, b"", what="blank")
            elif fault == "chunked":
                self.note("chunked")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for at in range(0, len(body), 1500):
                    part = body[at:at + 1500]
                    self.wfile.write(f"{len(part):x}\r\n".encode() + part + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            elif fault == "redirect":
                self.note("redirect")
                self.send_response(302)
                self.send_header("Location", "/moved/aircraft.json")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif fault == "slow":
                self.note("slow")  # as it comes: the answer is not taken
                time.sleep(15)
                self.answer(200, body, what="")
            elif fault == "truncated":
                self.note("truncated")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body[: len(body) // 2])
                self.wfile.flush()
                self.close_connection = True
            elif fault == "bad-json":
                self.answer(200, body[:-40] + b"}}]", what="bad-json")
            elif fault == "huge":
                pad = {"hex": "ffffff", "flight": "PAD", "lat": 0, "lon": 0, "seen_pos": 999, "note": "x" * 1000}
                self.answer(200, json.dumps({"now": 0, "aircraft": [pad] * 1000}).encode(), what="huge")
            elif fault == "close":
                self.note("close")
                self.close_connection = True
            elif fault == "no-list":
                self.answer(200, b'{"msg":"no aircraft here today"}', what="no-list")
            else:
                self.answer(200, body, what="ok")

        def firmware(self):
            if self.path == "/firmware/manifest.json":
                fault = firmware.take_fault("")
                with firmware.lock:
                    body = json.dumps(firmware.manifest(fault)).encode()
                if fault == "500":
                    self.answer(500, b'{"msg":"broken"}', what="manifest-500")
                elif fault == "bad-json":
                    self.answer(200, body[:-20], what="manifest-bad-json")
                else:
                    self.answer(200, body, what="manifest-" + (fault or "ok"))
                return
            with firmware.lock:
                name, image = firmware.name(), firmware.image
            if self.path != "/firmware/" + name:
                self.answer(404, b'{"msg":"not here"}', what="404")
                return
            fault = firmware.take_fault("bin-")
            if fault == "bin-404":
                self.answer(404, b'{"msg":"gone"}', what="bin-404")
            elif fault == "bin-close":
                self.note("bin-close")
                self.close_connection = True
            else:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(image)))
                self.end_headers()
                sent = image[: len(image) // 2] if fault == "bin-truncated" else image
                for at in range(0, len(sent), 16384):
                    self.wfile.write(sent[at:at + 16384])
                self.wfile.flush()
                self.close_connection = fault == "bin-truncated"
                self.note(fault or "bin-ok")  # once sent whole (or cut short): a download taken in

        def do_POST(self):
            self.arrived = time.time()
            if self.path != "/control":
                self.answer(404, b"{}")
                return
            change = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            feed.set(change.get("scenario"), change.get("fault"), change.get("times"), change.get("lat"),
                     change.get("lon"))
            if "firmware" in change:
                firmware.set(**change["firmware"])
            self.answer(200, b'{"ok":true}', what="control")

    return Handler


class Server:
    """The mock on its own thread: `start()`, then `feed` and `firmware` to steer it, `stop()` at the end. Every
    request, the firmware's too, is in `feed.log()`."""

    def __init__(self, port=8095, **feed):
        self.feed = Feed(**feed)
        self.firmware = Firmware()
        self.httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler_for(self.feed, self.firmware))
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]

    def start(self):
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8095)
    parser.add_argument("--lat", type=float, default=40.6413)
    parser.add_argument("--lon", type=float, default=-73.7781)
    parser.add_argument("--scenario", default="fixed")
    args = parser.parse_args()
    server = Server(args.port, lat=args.lat, lon=args.lon, scenario=args.scenario)
    print(f"Serving {args.scenario} on port {server.port}: /data/aircraft.json, /firmware/manifest.json, /control")
    server.httpd.serve_forever()


if __name__ == "__main__":
    main()
