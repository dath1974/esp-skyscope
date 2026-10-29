"""tests/mock_feed.py serves what it says, on this machine: run with python3 -m unittest discover -s tests."""

import hashlib
import http.client
import json
import math
import unittest

import mock_feed


class MockFeed(unittest.TestCase):
    def setUp(self):
        self.server = mock_feed.Server(port=0).start()

    def tearDown(self):
        self.server.stop()

    def get(self, path="/data/aircraft.json"):
        c = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=5)
        c.request("GET", path, headers={"User-Agent": "esp-skyscope/test"})
        r = c.getresponse()
        return r, r.read()

    def test_fixed_planes_at_their_places(self):
        r, body = self.get()
        self.assertEqual(r.status, 200)
        planes = {p["hex"]: p for p in json.loads(body)["aircraft"]}
        self.assertEqual(len(planes), len(mock_feed.FIXED) + len(mock_feed.UNSHOWABLE))
        north = planes["a00001"]
        self.assertAlmostEqual((north["lat"] - 40.6413) * 60, 5, places=2)
        east = planes["a00002"]
        self.assertAlmostEqual((east["lon"] + 73.7781) * 60 * math.cos(math.radians(40.6413)), 10, places=2)
        self.assertEqual(planes["a00005"]["alt_baro"], "ground")
        self.assertNotIn("lat", planes["a00009"])

    def test_requests_are_logged_with_their_agent(self):
        self.get()
        self.get("/v2/40/-73/20")
        log = self.server.feed.log()
        self.assertEqual([r[2] for r in log], ["esp-skyscope/test"] * 2)
        self.assertEqual(self.get("/somewhere")[0].status, 404)

    def test_a_fault_for_so_many_requests(self):
        self.server.feed.set(fault="429", times=2)
        for _ in range(2):
            r, _ = self.get()
            self.assertEqual((r.status, r.getheader("Retry-After")), (429, "30"))
        self.assertEqual(self.get()[0].status, 200)

    def test_truncated_and_bad_json(self):
        self.server.feed.set(fault="truncated", times=1)
        c = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=5)
        c.request("GET", "/data/aircraft.json")
        r = c.getresponse()
        with self.assertRaises(http.client.IncompleteRead):
            r.read()
        self.server.feed.set(fault="bad-json", times=1)
        with self.assertRaises(ValueError):
            json.loads(self.get()[1])

    def test_busy_and_moving(self):
        self.server.feed.set(scenario="busy")
        self.assertEqual(len(json.loads(self.get()[1])["aircraft"]), 600)
        self.server.feed.set(scenario="moving")
        plane = json.loads(self.get()[1])["aircraft"][0]
        self.assertLess(plane["lon"], -73.7781)  # still west of the middle

    def test_crowd_and_odd(self):
        self.server.feed.set(scenario="crowd")
        r, body = self.get()
        self.assertEqual(len(json.loads(body)["aircraft"]), 4500)
        self.assertTrue(500_000 < len(body) < 768 * 1024, len(body))  # big, but under the screen's most
        self.server.feed.set(scenario="odd")
        planes = json.loads(self.get()[1])["aircraft"]
        self.assertIn(7, planes)
        named = next(p for p in planes if isinstance(p, dict) and p.get("hex") == "a0000d")
        self.assertGreater(len(named["flight"].encode()), 10)  # past the 10 bytes kept, inside a character
        self.assertIsInstance(named["lat"], float)

    def test_good_answers_sent_another_way(self):
        self.server.feed.set(fault="chunked", times=1)
        r, body = self.get()  # http.client reads chunks
        self.assertEqual((r.status, r.getheader("Transfer-Encoding"), r.getheader("Content-Length")), (200, "chunked", None))
        self.assertEqual(len(json.loads(body)["aircraft"]), len(mock_feed.FIXED) + len(mock_feed.UNSHOWABLE))
        self.server.feed.set(fault="redirect", times=1)
        r, _ = self.get()
        self.assertEqual((r.status, r.getheader("Location")), (302, "/moved/aircraft.json"))
        self.assertEqual(self.get("/moved/aircraft.json")[0].status, 200)

    def test_answers_that_are_no_list(self):
        for fault, status in (("html", 200), ("blank", 200), ("404", 404), ("403", 403)):
            self.server.feed.set(fault=fault, times=1)
            r, body = self.get()
            self.assertEqual(r.status, status, fault)
            if status == 200:
                with self.assertRaises(ValueError):
                    json.loads(body)

    def test_a_release_and_its_firmware(self):
        r, body = self.get("/firmware/manifest.json")
        manifest = json.loads(body)
        self.assertEqual((r.status, manifest["version"]), (200, "9.9.9"))
        ota = manifest["builds"][0]["ota"]
        self.assertEqual(manifest["builds"][0]["chipFamily"], "ESP32-S3")
        r, image = self.get("/firmware/" + ota["path"])
        self.assertEqual((r.status, image[0], hashlib.md5(image).hexdigest()), (200, 0xE9, ota["md5"]))
        self.assertEqual(self.get("/firmware/skyscope-1.0.0.ota.bin")[0].status, 404)
        self.assertEqual([r[3] for r in self.server.feed.log()], ["manifest-ok", "bin-ok", "404"])

    def test_the_update_servers_faults(self):
        fw = self.server.firmware
        fw.set(fault="500", times=1)
        self.assertEqual(self.get("/firmware/manifest.json")[0].status, 500)
        self.assertEqual(self.get("/firmware/manifest.json")[0].status, 200)
        fw.set(fault="bad-json", times=1)
        with self.assertRaises(ValueError):
            json.loads(self.get("/firmware/manifest.json")[1])
        fw.set(fault="no-build", times=1)
        self.assertNotEqual(json.loads(self.get("/firmware/manifest.json")[1])["builds"][0]["chipFamily"], "ESP32-S3")
        fw.set(fault="no-ota", times=1)
        self.assertNotIn("ota", json.loads(self.get("/firmware/manifest.json")[1])["builds"][0])
        # An image's fault is left alone by the manifest, and used up by the image.
        fw.set(fault="bin-md5", times=1)
        ota = json.loads(self.get("/firmware/manifest.json")[1])["builds"][0]["ota"]
        self.assertNotEqual(hashlib.md5(self.get("/firmware/" + ota["path"])[1]).hexdigest(), ota["md5"])
        self.assertEqual(fw.fault, "")
        fw.set(fault="bin-404", times=1)
        self.assertEqual(self.get("/firmware/" + ota["path"])[0].status, 404)
        fw.set(fault="bin-truncated", times=1)
        with self.assertRaises(http.client.IncompleteRead):
            self.get("/firmware/" + ota["path"])

    def test_steered_over_http(self):
        c = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=5)
        c.request("POST", "/control", json.dumps({"scenario": "empty", "firmware": {"version": "1.2.3"}}))
        c.getresponse().read()
        self.assertEqual(json.loads(self.get()[1])["aircraft"], [])
        self.assertEqual(json.loads(self.get("/firmware/manifest.json")[1])["version"], "1.2.3")
        state = json.loads(self.get("/control")[1])
        self.assertEqual((state["scenario"], state["firmware"]["version"]), ("empty", "1.2.3"))


if __name__ == "__main__":
    unittest.main()
