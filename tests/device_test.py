#!/usr/bin/env python3
"""Tests on a real screen, over its web API: what the host tests cannot reach (the web server, the settings as the
screen keeps them, the feed and the tile servers through its own HTTP client, its memory as it runs).

    tests/device_test.py <address> [--no-touch] [--maps] [--reboot] [--sleep] [--soak MINUTES [--churn MINUTES]]

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
- --soak: memory sampled for that long, failing if it keeps falling; with --churn, the map and range changed every
  so many minutes (every map, near and far), memory compared each time the first of them comes round again, and
  the records that grow as the screen runs checked for staying bounded.
"""

import argparse
import json
import math
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
