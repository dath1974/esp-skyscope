#!/usr/bin/env python3
"""Tests on a real screen, over its web API: what the host tests cannot reach (the web server, the settings as the
screen keeps them, the feed and the tile servers through its own HTTP client, its memory as it runs).

    tests/device_test.py <address> [--no-touch] [--maps] [--reboot] [--sleep] [--mock] [--soak MINUTES [--churn MINUTES]]

Every setting it changes is put back at the end, pass or fail. It never prints the screen's place. Python's standard
library only. Nothing here installs firmware.

- always: the page, the settings (round trip, wrong values refused and nothing changed), status, aircraft, firmware
  check against the published manifest, the debug switch, unknown paths;
- touches (unless --no-touch, or the firmware is built with `test_api: false`), through /api/touch and /api/ui: a
  long press opens the menu; its arrows step a value without applying it; Cancel drops it and Save applies it; the
  paging arrows scroll; another appearance rebuilds the menu in its colours; Check asks the update server, and
  Install takes a second tap (never given); a tap on a plane opens its card and a second closes it; a tap on the map
  shows the screen's address; a drag up the right edge brightens it;
- --maps: each map in turn, every tile drawn without an error, and memory after;
- --reboot: a restart keeps every setting;
- --sleep: the screen goes off after a minute without a touch, and the touch that wakes it is no tap;
- --mock: the screen pointed at a mock receiver this script serves (tests/mock_feed.py), at a test place (New York
  JFK): the planes it lists are the ones the screen keeps, each where it should be on the glass; the filters, the
  most shown and watched planes, a plane's card in both units; 600 planes at once; and every failure a feed can have
  (refusals with Retry-After or a quota's end, a pace, a server error, an answer cut short, broken JSON, too big,
  too slow, no answer, no list), each waited out as long as it should be and recovered from. The machine running this
  must be reachable from the screen (WSL: networkingMode=mirrored);
- --soak: memory sampled for that long, failing if it keeps falling; with --churn, the map and range changed every
  so many minutes (every map, near and far), memory compared each time the first of them comes round again, and
  the records that grow as the screen runs checked for staying bounded.
"""

import argparse
import json
import math
import socket
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


def differing(screen: Screen, original: dict) -> str:
    """The settings that differ from `original`, by name only (never a value: one is the screen's place)."""
    now = screen.json("/api/settings")[1]
    return ", ".join(k for k in original if now.get(k) != original[k])


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
    run.check(status == 200 and not differing(screen, original), "every setting as the screen gave it is taken back",
              differing(screen, original))

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
        # A still map draws nothing until something on it changes: the loop running is what counts.
        run.check(s["loops_per_s"] > 3, f"the loop runs {s['loops_per_s']} times a second "
                  f"({s['frames_per_s']} frames a second)")
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


# ---------------------------------------------------------------- touches (probe.cpp)


class Glass:
    """The screen's glass through /api/ui and /api/touch."""

    def __init__(self, screen: Screen):
        self.screen = screen

    def ui(self) -> dict:
        return self.screen.json("/api/ui")[1]

    def touch(self, gesture: str, x: int, y: int, **more) -> bool:
        status, answer = self.screen.json("/api/touch", {"gesture": gesture, "x": x, "y": y, **more})
        if status != 200:
            return False
        time.sleep(answer["ms"] / 1000 + 0.25)  # done, and a loop or two after it
        return True

    def tap(self, x, y):
        return self.touch("tap", x, y)

    def tap_key(self, key: dict) -> bool:
        return bool(key and key.get("reachable")) and self.tap(key["x"], key["y"])

    def menu(self):
        return self.ui().get("menu")

    def open_menu(self) -> dict:
        ui = self.ui()
        if ui.get("menu"):
            return ui["menu"]
        self.touch("hold", ui["width"] // 2, ui["height"] // 2)
        return wait_for(self.menu, 5, 0.3)

    def close_menu(self, key="cancel"):
        m = self.menu()
        if m:
            self.tap_key(m["keys"][key])
        return wait_for(lambda: not self.menu(), 5, 0.3) is not None

    def row(self, key: str):
        m = self.menu()
        return next((r for r in (m or {}).get("rows", []) if r["key"] == key), None)

    def scroll_to(self, reachable) -> dict:
        """Pages down (from the top) until `reachable(menu)` gives something a finger reaches; that, or None."""
        m = self.menu()
        while m and m["scroll_y"] > 0:
            self.tap_key(m["keys"]["up"])
            m = self.menu()
        for _ in range(12):
            found = reachable(m)
            if found and found.get("reachable"):
                return found
            before = m["scroll_y"]
            self.tap_key(m["keys"]["down"])
            m = self.menu()
            if m["scroll_y"] == before:
                return None
        return None

    def clear_spot(self, ui: dict):
        """A spot on the map with no plane within reach, away from the status line and the brightness strip."""
        w, h = ui["width"], ui["height"]
        best, far = None, 0
        for gx in range(1, 8):
            for gy in range(2, 10):
                x, y = int(w * gx / 10), int(h * gy / 11)
                d = min((math.hypot(x - p["x"], y - p["y"]) for p in ui["planes"]), default=1e9)
                if d > far:
                    best, far = (x, y), d
        return best if far > 70 else None


def test_touch(screen: Screen, run: Run, original: dict):
    glass = Glass(screen)
    status, ui = screen.json("/api/ui")
    if status == 404:
        print("Touches: this firmware has no test API (test_api: false)")
        return
    print("Touches")
    if ui.get("asleep"):
        glass.tap(ui["width"] // 2, ui["height"] // 2)  # wakes it
        time.sleep(1.5)
    if ui.get("menu"):
        glass.close_menu()

    # The menu: opened by a long press, a step shown but not applied, Cancel drops it.
    m = glass.open_menu()
    if not run.check(m is not None, "a long press on the map opens the menu"):
        return
    run.check(all(m["keys"][k]["reachable"] for k in ("cancel", "save", "up", "down")), "Cancel, Save and the arrows")
    row = glass.row("range_nm")
    was = row["value"]
    glass.tap_key(row["on"])
    row = glass.row("range_nm")
    run.check(row["value"] != was and row["pending"], f"an arrow steps the range ({was} to {row['value']}), marked")
    run.check(screen.json("/api/settings")[1]["range_nm"] == original["range_nm"], "but does not apply it")
    glass.tap_key(row["back"])
    run.check(glass.row("range_nm")["value"] == was and not glass.row("range_nm")["pending"], "and back")
    glass.tap_key(glass.row("range_nm")["on"])
    run.check(glass.close_menu("cancel"), "Cancel closes the menu")
    run.check(not differing(screen, original), "and nothing it showed is applied", differing(screen, original))

    # Save applies what the menu shows.
    glass.open_menu()
    before = glass.row("range_nm")["value"]
    glass.tap_key(glass.row("range_nm")["on"])
    shown = glass.row("range_nm")["value"]
    run.check(shown != before, f"the range steps again ({before} to {shown})")
    run.check(glass.close_menu("save"), "Save closes the menu")
    applied = screen.json("/api/settings")[1]["range_nm"]
    run.check(shown == f"{applied} NM", f"and applies the range it showed ({shown})", f"{applied} NM applied")
    screen.json("/api/settings", {"range_nm": original["range_nm"]})

    # The paging arrows, and the menu rebuilt in another appearance's colours.
    m = glass.open_menu()
    glass.tap_key(m["keys"]["down"])
    lower = glass.menu()["scroll_y"]
    run.check(lower > 0, f"the down arrow scrolls the list ({lower} px)")
    glass.tap_key(glass.menu()["keys"]["up"])
    run.check(glass.menu()["scroll_y"] < lower, "and the up arrow back")
    theme = glass.row("theme")
    if theme and theme["shown"]:
        was = theme["value"]
        glass.tap_key(theme["on"])
        time.sleep(0.5)  # rebuilt after the event
        theme = glass.row("theme")
        run.check(glass.menu() is not None and theme["value"] != was, f"the appearance steps ({was} to {theme['value']})")
        run.check(glass.menu()["keys"]["save"]["reachable"], "and the menu, rebuilt in its colours, still works")

    # The firmware: Check asks the server; Install wants a second tap, which is never given here.
    check = glass.scroll_to(lambda m: m["keys"].get("check"))
    if check is None:
        print("  (no firmware part: built without updates)")
    else:
        glass.tap_key(check)
        answered = wait_for(lambda: (lambda f: f if f and f != "Checking..." else None)(glass.menu().get("firmware")), 25, 1)
        run.check(answered is not None, f"Check is answered: {answered}")
        install = glass.menu()["keys"].get("install")
        if glass.menu().get("install"):
            install = glass.scroll_to(lambda m: m["keys"].get("install"))
            glass.tap_key(install)
            run.check(glass.menu().get("install") == "Tap again to install", "Install asks for a second tap")
            time.sleep(5)
            run.check(glass.menu().get("install", "").startswith("Install "), "and forgets it after a while")
        else:
            print("  (nothing to install: Install is not offered, as it should not be)")
    run.check(glass.close_menu("cancel"), "the menu closes")
    run.check(not differing(screen, original), "with nothing applied", differing(screen, original))

    # A plane's card.
    ui = glass.ui()
    planes = [p for p in ui["planes"] if 40 < p["x"] < ui["width"] - 80 and 60 < p["y"] < ui["height"] - 40]
    if planes:
        p = planes[0]
        glass.tap(p["x"], p["y"])
        ui = glass.ui()
        run.check(bool(ui.get("card")) and ui["picked"] != "", "a tap on a plane opens its card")
        glass.tap(p["x"], p["y"]) if ui["picked"] == p["id"] else glass.tap(*glass.clear_spot(ui) or (10, 10))
        run.check(not glass.ui().get("card"), "and a second tap closes it")
    else:
        print("  (no plane on the glass to tap)")

    # The screen's address, at a tap on the map away from the planes.
    ui = glass.ui()
    spot = glass.clear_spot(ui)
    if spot:
        # A tap turns the address on or off: one already shown is let go first (it goes by itself in 6 s).
        wait_for(lambda: "http://" not in glass.ui()["status"], 10, 0.5)
        glass.tap(*spot)
        run.check("http://" in glass.ui()["status"], "a tap on the map shows the screen's address")
        glass.tap(*spot)
    else:
        print("  (no clear spot on the map for a tap)")

    # The brightness strip: a drag up the right edge brightens the screen.
    start = screen.json("/api/settings")[1]["brightness"]
    screen.json("/api/settings", {"brightness": 20})
    time.sleep(0.5)
    w, h = ui["width"], ui["height"]
    screen.json("/api/touch", {"gesture": "drag", "x": w - 8, "y": int(h * 0.8), "x1": w - 8, "y1": int(h * 0.3), "ms": 900})
    time.sleep(0.6)
    level = glass.ui().get("level")
    time.sleep(1)
    after = screen.json("/api/settings")[1]["brightness"]
    run.check(level is not None, f"the level shows while it is dragged ({level})")
    run.check(after > 20, f"and the drag up brightens it (20% to {after}%)")
    screen.json("/api/settings", {"brightness": start})


def test_sleep(screen: Screen, run: Run, original: dict):
    print("Screen off")
    glass = Glass(screen)
    screen.json("/api/settings", {"sleep_min": 1})
    asleep = wait_for(lambda: glass.ui().get("asleep"), 100, 3)
    run.check(asleep is not None, "off after a minute without a touch")
    if asleep:
        ui = glass.ui()
        spot = glass.clear_spot(ui) or (ui["width"] // 2, ui["height"] // 2)
        glass.tap(*spot)
        woke = wait_for(lambda: not glass.ui().get("asleep"), 5, 0.3)
        run.check(woke is not None, "a tap turns it on")
        ui = glass.ui()
        run.check("http://" not in ui["status"] and not ui.get("card"), "and the tap that woke it does nothing else")
    screen.json("/api/settings", {"sleep_min": original["sleep_min"]})


# ---------------------------------------------------------------- a mock receiver (mock_feed.py)

TEST_PLACE = {"lat": 40.6413, "lon": -73.7781}  # New York JFK: the screen's own place is never used or shown


def own_address_towards(device: str) -> str:
    """This machine's address on the network the screen is on."""
    host = device.removeprefix("http://").split("/")[0].split(":")[0]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, 80))
        return s.getsockname()[0]


def test_mock(screen: Screen, run: Run, original: dict, address: str):
    import mock_feed

    print("A mock receiver")
    server = mock_feed.Server(port=8095, **TEST_PLACE).start()
    feed = server.feed
    url = f"http://{own_address_towards(address)}:{server.port}/data/aircraft.json"
    glass = Glass(screen)
    try:
        base = {**TEST_PLACE, "place": "TEST", "range_nm": 15, "map": "radar", "feed": "receiver", "receiver_url": url,
                "fallback": False, "refresh_s": 5, "most": 40, "alt_min": 0, "alt_max": 60000, "ground": False,
                "watch": [{"id": "N12345", "colour": "red"}], "track_s": 0, "sleep_min": 0, "bedtime": False,
                "metric": False, "label": "callsign"}
        status, answer = screen.json("/api/settings", base)
        if not run.check(status == 200, "the screen takes the receiver's address", str(answer)):
            return
        started = time.time()
        first = wait_for(lambda: feed.log(started), 30, 1)
        if not run.check(first is not None, "and asks it", f"no request to {url} (can the screen reach this machine?)"):
            return
        run.check(first[0][2].startswith("esp-skyscope/"), f"saying who it is ({first[0][2]})")

        def kept():
            return {p["hex"] for p in screen.json("/api/aircraft")[1]["aircraft"]}

        def placed():
            return {p["id"]: p for p in glass.ui()["planes"]}

        showable = {h for h, *_ in mock_feed.FIXED} - {"a00005"}  # not the one on the ground
        got = wait_for(lambda: kept() if kept() == showable else None, 20, 1)
        run.check(got is not None, "the planes kept are the ones that can be shown", f"{sorted(kept())}")

        # Each where it should be: the scope's middle is the place; north up; distances in proportion.
        ui = glass.ui()
        cx, cy = ui["width"] / 2, ui["height"] / 2
        p = wait_for(lambda: (lambda q: q if {"a00001", "a00002", "a00003", "a00004"} <= q.keys() else None)(placed()), 10)
        if run.check(p is not None, "the planes are on the glass"):
            n5, e10, s10, w5 = p["a00001"], p["a00002"], p["a00003"], p["a00004"]
            run.check(abs(n5["x"] - cx) <= 2 and n5["y"] < cy, f"5 NM north is straight up ({n5['x']:.0f}, {n5['y']:.0f})")
            run.check(abs(s10["x"] - cx) <= 2 and s10["y"] > cy, "10 NM south straight down")
            run.check(abs(e10["y"] - cy) <= 2 and e10["x"] > cx, "10 NM east to the right")
            run.check(abs(w5["y"] - cy) <= 2 and w5["x"] < cx, "5 NM west to the left")
            ratio = (s10["y"] - cy) / max(1, cy - n5["y"])
            run.check(abs(ratio - 2) < 0.06, f"10 NM is twice as far as 5 ({ratio:.3f})")
            run.check(abs((e10["x"] - cx) - (s10["y"] - cy)) <= 2, "the same distance east as south")

        # The filters, the most shown, and a watched plane.
        screen.json("/api/settings", {"ground": True})
        run.check(wait_for(lambda: "a00005" in placed(), 20, 1) is not None, "ground traffic when asked for")
        screen.json("/api/settings", {"ground": False, "alt_max": 20000})
        gone = wait_for(lambda: (lambda q: q if not {"a00002", "a00006", "a00007"} & q.keys() else None)(placed()), 20, 1)
        run.check(gone is not None, "an altitude limit hides those above it, and those without an altitude")
        screen.json("/api/settings", {"alt_max": 60000, "most": 1})
        few = wait_for(lambda: (lambda q: q if len(q) == 2 else None)(placed()), 20, 1)
        run.check(few is not None and "a00008" in few, "the most shown is the nearest one, and the watched plane too",
                  f"{sorted(placed())}")
        screen.json("/api/settings", {"most": 40})
        wait_for(lambda: "a00004" in placed(), 20, 1)

        # A plane's card, in both units.
        p = placed().get("a00004")
        if p:
            glass.tap(p["x"], p["y"])
            card = glass.ui().get("card")
            run.check(bool(card) and "EMERG1" in card["title"], "a tap on the emergency opens its card",
                      json.dumps(card))
            if card:
                run.check(any("7700" in r for r in card["rows"]), "with its squawk")
                feet = card["rows"][1]
                screen.json("/api/settings", {"metric": True})
                time.sleep(1.5)
                metres = (glass.ui().get("card") or {"rows": ["", ""]})["rows"][1]
                run.check(metres != feet, f"and in metric ({feet} / {metres})")
                screen.json("/api/settings", {"metric": False})
                glass.tap(p["x"], p["y"])

        # A busy sky: 600 planes, of which the screen keeps what it can show.
        feed.set(scenario="busy")
        busy = wait_for(lambda: (lambda s: s if s["records"]["flights"] >= 40 else None)(screen.json("/api/status")[1]), 20, 1)
        time.sleep(6)
        s = screen.json("/api/status")[1]
        run.check(busy is not None and s["records"]["flights"] <= 60, f"600 planes: {s['records']['flights']} kept")
        run.check(s["heap_free"] >= HEAP_LEAST, f"internal RAM free {s['heap_free'] // 1024} KB")
        run.check(s["frames_per_s"] > 0, f"{s['frames_per_s']} frames a second, longest {s['frame_ms_max']} ms")
        feed.set(scenario="fixed")

        # Failures: each waited out as long as it should be, and recovered from.
        def fault(name, what, wait_least, wait_most, words=""):
            feed.set(fault=name, times=1)
            since = time.time()
            hit = wait_for(lambda: [r for r in feed.log(since) if r[3] == what], 60, 0.5)
            if not run.check(bool(hit), f"{name}: answered"):
                return
            at = hit[0][0]
            time.sleep(2)
            st = screen.json("/api/status")[1]
            mine = next((f for f in st["feeds"] if f["feed"] == "receiver"), {})
            run.check(mine.get("waiting", False) or wait_most < 10, f"{name}: the receiver is left alone "
                      f"({mine.get('error', '')}, {mine.get('retry_in_s', '-')} s)")
            if words:
                run.check(words in glass.ui()["status"], f"{name}: the status line says so")
            nxt = wait_for(lambda: [r for r in feed.log(at + 0.1)], wait_most + 15, 0.5)
            gap = (nxt[0][0] - at) if nxt else None
            run.check(gap is not None and wait_least <= gap <= wait_most + 3,
                      f"{name}: asked again {gap:.0f} s later (between {wait_least} and {wait_most})" if gap else
                      f"{name}: asked again", "")
            ok = wait_for(lambda: [r for r in feed.log(at + 0.1) if r[3] == "ok"], 40, 0.5)
            run.check(bool(ok) and st["uptime_s"] <= screen.json("/api/status")[1]["uptime_s"],
                      f"{name}: recovered, without restarting")

        fault("429", "429", 28, 31, "again in")
        fault("429-reset", "429-reset", 28, 31)
        fault("500", "500", 13, 16)
        fault("truncated", "truncated", 13, 16)
        fault("bad-json", "bad-json", 13, 16)
        fault("no-list", "no-list", 13, 16)
        fault("close", "close", 13, 16)
        fault("huge", "huge", 13, 16)
        fault("slow", "slow", 13, 30)
        # A pace announced with a good answer: asked no more often than it says.
        feed.set(fault="paced", times=1)
        since = time.time()
        hit = wait_for(lambda: [r for r in feed.log(since) if r[3] == "paced"], 30, 0.5)
        if hit:
            nxt = wait_for(lambda: feed.log(hit[0][0] + 0.1), 40, 0.5)
            gap = nxt[0][0] - hit[0][0] if nxt else 0
            run.check(19 <= gap <= 24, f"paced: asked again {gap:.0f} s later (at most once in 20 s)")
    finally:
        server.stop()
        screen.json("/api/settings", original)


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
    run.check(not differing(screen, original), "every setting kept", differing(screen, original))


CHURN = [("streets", 15), ("satellite", 40), ("radar", 25), ("sectional", 7), ("osm", 100), ("topo", 5),
         ("ifr_low", 30), ("ifr_high", 150)]
# The records that grow as the screen runs, and how far each may: past this a record is not being let go of.
RECORD_MOST = {"symbols": 400, "label_images": 200, "spot_of": 400, "box_of": 400, "drawn": 400, "again": 400,
               "tracks": 600, "cached_tiles": 400}


def test_soak(screen: Screen, run: Run, minutes: float, churn: float = 0, original: dict = None):
    print(f"Memory over {minutes:g} minutes" + (f", the map changed every {churn:g}" if churn else ""))
    samples, rounds = [], []  # every sample; and with churn, the one at each return to CHURN's first view
    end = time.monotonic() + minutes * 60
    next_change, step = time.monotonic(), 0
    worst = {}
    while time.monotonic() < end:
        if churn and time.monotonic() >= next_change:
            name, nm = CHURN[step % len(CHURN)]
            screen.json("/api/settings", {"map": name, "range_nm": nm})
            next_change = time.monotonic() + churn * 60
            if step % len(CHURN) == 0 and step:
                time.sleep(20)  # its tiles drawn (from the cache, if it keeps them)
                s = screen.json("/api/status")[1]
                rounds.append((s["heap_free"], s["psram_free"]))
                print(f"    round {len(rounds)}: free {s['heap_free'] // 1024} KB, PSRAM {s['psram_free'] // 1024} KB")
            step += 1
        s = screen.json("/api/status")[1]
        samples.append((s["heap_free"], s["heap_largest"]))
        for k, v in s["records"].items():
            worst[k] = max(worst.get(k, 0), v)
        print(f"    {len(samples):3d}: free {s['heap_free'] // 1024} KB, largest {s['heap_largest'] // 1024} KB, "
              f"PSRAM {s['psram_free'] // 1024} KB, {s['aircraft']} aircraft, {s['records']['cached_tiles']} tiles")
        time.sleep(30)
    if churn and original:
        screen.json("/api/settings", {"map": original["map"], "range_nm": original["range_nm"]})
    lows = [f for f, _ in samples]
    run.check(min(lows) >= HEAP_LEAST, f"never under {HEAP_LEAST // 1024} KB free", f"{min(lows) // 1024} KB")
    if len(samples) >= 6:
        third = len(lows) // 3
        first, last = sum(lows[:third]) / third, sum(lows[-third:]) / third
        run.check(last >= first - 20_000, "free memory not falling",
                  f"{first / 1024:.0f} KB in the first third, {last / 1024:.0f} KB in the last")
    for k, most in RECORD_MOST.items():
        if k in worst:
            run.check(worst[k] <= most, f"{k} at most {worst[k]}", f"over {most}")
    if len(rounds) >= 2:
        # The first round is after the tile cache filled once; each later one at the same view should be no lower.
        (h0, p0), (h1, p1) = rounds[0], rounds[-1]
        run.check(h1 >= h0 - 16_000, f"internal RAM at the same view, round 1 and {len(rounds)}: "
                  f"{h0 // 1024} and {h1 // 1024} KB")
        run.check(p1 >= p0 - 512_000, f"PSRAM at the same view: {p0 // 1024} and {p1 // 1024} KB")
    elif churn:
        print("  (too short for the map to come round twice: no comparison of rounds)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", help="the screen's address (an IP or name.local)")
    parser.add_argument("--maps", action="store_true")
    parser.add_argument("--reboot", action="store_true")
    parser.add_argument("--no-touch", action="store_true")
    parser.add_argument("--sleep", action="store_true")
    parser.add_argument("--mock", action="store_true")
    parser.add_argument("--soak", type=float, metavar="MINUTES")
    parser.add_argument("--churn", type=float, metavar="MINUTES", default=0)
    args = parser.parse_args()

    sys.stdout.reconfigure(line_buffering=True)  # a soak's samples as they come, through a pipe too
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
        if not args.no_touch:
            test_touch(screen, run, original)
        if args.maps:
            test_maps(screen, run, original)
        if args.reboot:
            test_reboot(screen, run, original)
        if args.mock:
            test_mock(screen, run, original, args.address)
        if args.sleep:
            test_sleep(screen, run, original)
        if args.soak:
            test_soak(screen, run, args.soak, args.churn, original)
    finally:
        if screen.up() and screen.json("/api/settings")[1] != original:
            screen.json("/api/settings", original)
            print("Settings put back" if screen.json("/api/settings")[1] == original else "SETTINGS NOT PUT BACK")
    print(f"{run.failed} FAILED" if run.failed else "All passed")
    sys.exit(1 if run.failed else 0)


if __name__ == "__main__":
    main()
