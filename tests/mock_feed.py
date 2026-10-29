#!/usr/bin/env python3
"""A stand-in for an ADS-B receiver (readsb's aircraft.json), for tests on a real screen: aircraft that are known,
where they are known to be, and the failures a feed can have, on demand.

    tests/mock_feed.py [--port 8095] [--lat 40.6413 --lon -73.7781] [--scenario fixed]

Point the screen at it (settings: feed "receiver", receiver_url http://<this machine>:8095/data/aircraft.json).
tests/device_test.py --mock starts one of its own, in the same process. By hand, it takes:

    GET  /data/aircraft.json      the aircraft (any path ending in aircraft.json, and /v2/<lat>/<lon>/<dist>)
    GET  /control                 what it serves, and every request it has had
    POST /control                 {"scenario": ..., "fault": ..., "times": n, "lat": ..., "lon": ...}

Scenarios: "fixed" (a few planes with every kind of field, at known places), "busy" (600 planes), "moving" (one plane
flying east at 480 kt), "empty". Faults, for the next `times` requests (0: until changed): "429" (Retry-After),
"429-reset" (x-ratelimit-reset), "paced" (a good answer with x-ratelimit-limit 1;w=20), "500", "slow" (40 s before
answering), "truncated" (stops half-way through the body it announced), "bad-json", "huge" (a body of 1 MB), "close"
(the connection closed without an answer), "no-list" ({"msg": ...} and no aircraft).

Python's standard library only.
"""

import argparse
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
]
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
        with self.lock:
            return [r for r in self.requests if r[0] >= since]

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


def handler_for(feed: Feed):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"  # keep-alive, as the screen's client uses it

        def log_message(self, *args):
            pass

        def answer(self, status, body: bytes, headers=None, what=""):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)
            with feed.lock:
                feed.requests.append((time.time(), self.path, self.headers.get("User-Agent", ""), what or str(status)))

        def do_GET(self):
            if self.path == "/control":
                with feed.lock:
                    state = {"scenario": feed.scenario, "fault": feed.fault, "times": feed.times,
                             "requests": [{"at": t, "path": p, "agent": a, "answer": w} for t, p, a, w in feed.requests]}
                body = json.dumps(state).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
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
            elif fault == "500":
                self.answer(500, b'{"msg":"broken"}', what="500")
            elif fault == "slow":
                time.sleep(40)
                self.answer(200, body, what="slow")
            elif fault == "truncated":
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body[: len(body) // 2])
                self.wfile.flush()
                self.close_connection = True
                with feed.lock:
                    feed.requests.append((time.time(), self.path, self.headers.get("User-Agent", ""), "truncated"))
            elif fault == "bad-json":
                self.answer(200, body[:-40] + b"}}]", what="bad-json")
            elif fault == "huge":
                pad = {"hex": "ffffff", "flight": "PAD", "lat": 0, "lon": 0, "seen_pos": 999, "note": "x" * 1000}
                self.answer(200, json.dumps({"now": 0, "aircraft": [pad] * 1000}).encode(), what="huge")
            elif fault == "close":
                self.close_connection = True
                with feed.lock:
                    feed.requests.append((time.time(), self.path, self.headers.get("User-Agent", ""), "close"))
            elif fault == "no-list":
                self.answer(200, b'{"msg":"no aircraft here today"}', what="no-list")
            else:
                self.answer(200, body, what="ok")

        def do_POST(self):
            if self.path != "/control":
                self.answer(404, b"{}")
                return
            change = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            feed.set(change.get("scenario"), change.get("fault"), change.get("times"), change.get("lat"),
                     change.get("lon"))
            self.answer(200, b'{"ok":true}', what="control")

    return Handler


class Server:
    """The mock feed on its own thread: `start()`, then `feed` to steer it, `stop()` at the end."""

    def __init__(self, port=8095, **feed):
        self.feed = Feed(**feed)
        self.httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler_for(self.feed))
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
    print(f"Serving {args.scenario} on port {server.port}: /data/aircraft.json, /control")
    server.httpd.serve_forever()


if __name__ == "__main__":
    main()
