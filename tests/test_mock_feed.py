"""tests/mock_feed.py serves what it says, on this machine: run with python3 -m unittest discover -s tests."""

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


if __name__ == "__main__":
    unittest.main()
