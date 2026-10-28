#!/usr/bin/env python3
"""Tests on a real screen, over its web API: what the host tests cannot reach (the web server, the settings as the
screen keeps them, the feed and the tile servers through its own HTTP client, its memory as it runs).

    tests/device_test.py <address> [--maps] [--reboot] [--soak MINUTES]

Every setting it changes is put back at the end, pass or fail. It never prints the screen's place. Python's standard
library only. Nothing here installs firmware.

- always: the page, the settings (round trip, wrong values refused and nothing changed), status, aircraft, firmware
  check against the published manifest, the debug switch, unknown paths;
- --maps: each map in turn, every tile drawn without an error, and memory after;
- --reboot: a restart keeps every setting;
- --soak: memory sampled for that long, failing if it keeps falling.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

MAPS = ["radar", "streets", "osm", "satellite", "topo", "sectional", "ifr_low", "ifr_high"]
HEAP_LEAST = 60_000  # internal RAM free, below which TLS connections start to fail
LARGEST_LEAST = 20_000


class Screen:
    def __init__(self, address: str):
        self.base = "http://" + address.removeprefix("http://").rstrip("/")

    def call(self, path: str, body=None, timeout: float = 10):
        data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        request = urllib.request.Request(self.base + path, data=data, method="POST" if data is not None else "GET")
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as r:
                return r.status, r.headers.get("Content-Type", ""), r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers.get("Content-Type", ""), e.read()

    def json(self, path: str, body=None, timeout: float = 10):
        status, _, text = self.call(path, body, timeout)
        return status, json.loads(text) if text else None

    def up(self) -> bool:
        try:
            return self.call("/api/status", timeout=3)[0] == 200
        except OSError:
            return False


class Run:
    def __init__(self):
        self.failed = 0

    def check(self, ok: bool, what: str, detail: str = ""):
        print(("  ok    " if ok else "  FAIL  ") + what + (f" ({detail})" if detail and not ok else ""))
        if not ok:
            self.failed += 1
        return ok


def wait_for(condition, seconds: float, every: float = 2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            value = condition()
            if value:
                return value
        except OSError:
            pass
        time.sleep(every)
    return None


def published_version(releases: str) -> str:
    with urllib.request.urlopen(releases + "/manifest.json", timeout=10) as r:
        return json.loads(r.read())["version"]


def releases_address() -> str:
    from pathlib import Path

    for line in (Path(__file__).resolve().parent.parent / "skyscope.yaml").read_text().splitlines():
        if line.strip().startswith("releases:"):
            return line.split(":", 1)[1].strip()
    raise SystemExit("skyscope.yaml names no releases address")


def test_basics(screen: Screen, run: Run, original: dict):
    print("The page and the API")
    status, kind, page = screen.call("/")
    run.check(status == 200 and kind.startswith("text/html"), "the settings page is served")
    run.check(b"function fw_words" in page and b"function faa_where" in page, "it is this firmware's page")
    run.check(screen.call("/api/nothing")[0] == 404, "an unknown path is 404")

    print("Settings")
    for key in ("lat", "lon", "range_nm", "map", "feed", "timezone", "watch"):
        run.check(key in original, f"settings have {key}")
    track = 60 if original.get("track_s") != 60 else 30
    status, changed = screen.json("/api/settings", {"track_s": track})
    run.check(status == 200 and changed["track_s"] == track, "a change is taken")
    run.check(screen.json("/api/settings")[1]["track_s"] == track, "and kept")
    before = screen.json("/api/settings")[1]
    status, answer = screen.json("/api/settings", {"track_s": 30, "range_nm": 9999})
    run.check(status == 400 and answer == {"error": "range_nm"}, "a wrong value is refused and named", str(answer))
    run.check(screen.json("/api/settings")[1] == before, "and nothing of that request is taken")
    status, answer = screen.json("/api/settings", b"not json")
    run.check(status == 400 and answer == {"error": "body"}, "a body that is not JSON is refused")
    status, _ = screen.json("/api/settings", original)
    run.check(status == 200, "every setting as the screen gave it is taken back")

    print("Status")
    status, s = screen.json("/api/status")
    run.check(status == 200, "status answers")
    run.check(s["heap_free"] >= HEAP_LEAST, f"internal RAM free {s['heap_free'] // 1024} KB", f"under {HEAP_LEAST}")
    run.check(s["heap_largest"] >= LARGEST_LEAST, f"largest free block {s['heap_largest'] // 1024} KB")
    run.check(s["psram_free"] > 1_000_000, f"PSRAM free {s['psram_free'] // 1024} KB")
    run.check(s.get("wifi_rssi", 0) > -85, f"Wi-Fi {s.get('wifi_rssi')} dBm")
    if s["asleep"]:
        print("  (asleep: frames and the feed are not looked at)")
    else:
        run.check(s["frames_per_s"] > 0, f"{s['frames_per_s']} frames a second")
        run.check(s["frame_ms_max"] < 1000, f"longest frame {s['frame_ms_max']} ms")
        fed = wait_for(lambda: (lambda x: x if 0 <= x["feed_age_s"] < 120 else None)(screen.json("/api/status")[1]), 60)
        run.check(fed is not None, "a feed answered within the last two minutes",
                  json.dumps(screen.json("/api/status")[1].get("feeds")))

    print("Aircraft")
    status, a = screen.json("/api/aircraft")
    run.check(status == 200 and isinstance(a.get("aircraft"), list), f"{len(a['aircraft'])} aircraft listed")
    bad = [p for p in a["aircraft"] if not (-90 <= p["lat"] <= 90 and -180 <= p["lon"] <= 180 and p.get("hex"))]
    run.check(not bad, "each has an address and a position", f"{len(bad)} without")

    print("Debug switch")
    run.check(screen.json("/api/debug", {"planes": False}) == (200, {"planes": False}), "planes hidden")
    run.check(screen.json("/api/debug", {"planes": True}) == (200, {"planes": True}), "and shown again")
    run.check(screen.json("/api/debug", {"planes": 1})[0] == 400, "a wrong value is refused")


def test_firmware(screen: Screen, run: Run):
    print("Firmware")
    status, f = screen.json("/api/firmware")
    run.check(status == 200 and f.get("updates") is True, f"updates built in, running {f.get('version')}")
    try:
        latest = published_version(releases_address())
    except OSError as e:
        run.check(False, "the published manifest is readable", str(e))
        return
    screen.json("/api/firmware", {"check": True})
    f = wait_for(lambda: (lambda x: x if x.get("known") else None)(screen.json("/api/firmware")[1]), 60)
    run.check(f is not None, "a check reads the update server", json.dumps(screen.json("/api/firmware")[1]))
    if f:
        run.check(f["latest"] == latest, f"it finds the published {latest}", f.get("latest"))
        run.check(f["available"] == (f["version"] != latest), "offered exactly when it differs from the running one")
        run.check(not f.get("error"), "no error", f.get("error"))
    run.check(screen.json("/api/firmware", {"nonsense": True})[0] == 400, "a request that is neither is refused")


def test_maps(screen: Screen, run: Run, original: dict):
    print("Maps (each drawn whole, without a tile error)")
    for name in MAPS:
        status, _ = screen.json("/api/settings", {"map": name})
        if not run.check(status == 200, f"{name}: taken"):
            continue
        if name == "radar":
            time.sleep(3)
            continue
        started = time.monotonic()

        def drawn():
            s = screen.json("/api/status")[1]
            return s if s["tiles_total"] > 0 and s["tiles_done"] >= s["tiles_total"] else None

        time.sleep(2)  # the new plan reaches the worker
        s = wait_for(drawn, 90)
        last = screen.json("/api/status")[1]
        run.check(s is not None, f"{name}: every tile in {time.monotonic() - started:.0f} s",
                  f"{last['tiles_done']}/{last['tiles_total']}, {last['tile_error']}")
        run.check(not last["tile_error"], f"{name}: no tile error", last["tile_error"])
        run.check(last["heap_free"] >= HEAP_LEAST, f"{name}: internal RAM free {last['heap_free'] // 1024} KB")
    screen.json("/api/settings", {"map": original["map"]})


def test_reboot(screen: Screen, run: Run, original: dict):
    print("Restart")
    screen.json("/api/reboot", {})
    run.check(wait_for(lambda: not screen.up(), 30, 1) is not None, "it goes down")
    run.check(wait_for(screen.up, 90) is not None, "and comes back")
    s = screen.json("/api/status")[1]
    run.check(s["uptime_s"] < 120, f"up {s['uptime_s']} s")
    run.check(screen.json("/api/settings")[1] == original, "every setting kept")


def test_soak(screen: Screen, run: Run, minutes: float):
    print(f"Memory over {minutes:g} minutes")
    samples = []
    end = time.monotonic() + minutes * 60
    while time.monotonic() < end:
        s = screen.json("/api/status")[1]
        samples.append((s["heap_free"], s["heap_largest"]))
        print(f"    {len(samples):3d}: free {s['heap_free'] // 1024} KB, largest {s['heap_largest'] // 1024} KB, "
              f"{s['aircraft']} aircraft")
        time.sleep(30)
    lows = [f for f, _ in samples]
    run.check(min(lows) >= HEAP_LEAST, f"never under {HEAP_LEAST // 1024} KB free", f"{min(lows) // 1024} KB")
    if len(samples) >= 6:
        third = len(lows) // 3
        first, last = sum(lows[:third]) / third, sum(lows[-third:]) / third
        run.check(last >= first - 20_000, "free memory not falling",
                  f"{first / 1024:.0f} KB in the first third, {last / 1024:.0f} KB in the last")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", help="the screen's address (an IP or name.local)")
    parser.add_argument("--maps", action="store_true")
    parser.add_argument("--reboot", action="store_true")
    parser.add_argument("--soak", type=float, metavar="MINUTES")
    args = parser.parse_args()

    screen = Screen(args.address)
    if not screen.up():
        raise SystemExit(f"No screen answers at {screen.base}")
    run = Run()
    status, original = screen.json("/api/settings")
    if status != 200:
        raise SystemExit("The screen gave no settings")
    try:
        test_basics(screen, run, original)
        test_firmware(screen, run)
        if args.maps:
            test_maps(screen, run, original)
        if args.reboot:
            test_reboot(screen, run, original)
        if args.soak:
            test_soak(screen, run, args.soak)
    finally:
        if screen.up() and screen.json("/api/settings")[1] != original:
            screen.json("/api/settings", original)
            print("Settings put back" if screen.json("/api/settings")[1] == original else "SETTINGS NOT PUT BACK")
    print(f"{run.failed} FAILED" if run.failed else "All passed")
    sys.exit(1 if run.failed else 0)


if __name__ == "__main__":
    main()
