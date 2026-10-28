"""tools/release_site.py writes what ESPHome's http_request update reads (http_request_update.cpp): run with
python3 -m unittest discover -s tests."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = "https://example.github.io/esp-skyscope/manifest.json"


def firmware_url(source: str, path: str) -> str:
    """The firmware's address as the update entity makes it from the manifest's and the path in it."""
    if "http" in path:
        return path
    if path.startswith("/"):
        return source[: source.find("/", 8)] + path
    return source[: source.rfind("/") + 1] + path


def ota_build(manifest: dict, variant: str):
    """The update entity's reading: the fields it requires, and the build for its own chip (None if there is none)."""
    if not isinstance(manifest.get("name"), str) or not isinstance(manifest.get("version"), str):
        raise ValueError("name and version")
    if not isinstance(manifest.get("builds"), list):
        raise ValueError("builds")
    for build in manifest["builds"]:
        if not isinstance(build.get("chipFamily"), str):
            raise ValueError("chipFamily")
        if build["chipFamily"] == variant:
            ota = build.get("ota")
            if not isinstance(ota, dict) or not isinstance(ota.get("path"), str) or not isinstance(ota.get("md5"), str):
                raise ValueError("ota")
            return ota
    return None


class ReleaseSite(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.build = Path(self.dir.name, "build")
        self.out = Path(self.dir.name, "site")
        self.build.mkdir()
        (self.build / "firmware.ota.bin").write_bytes(b"\xe9update image" * 100)
        (self.build / "firmware.factory.bin").write_bytes(b"\xe9factory image" * 100)

    def tearDown(self):
        self.dir.cleanup()

    def run_tool(self, *extra):
        subprocess.run(
            [sys.executable, str(ROOT / "tools/release_site.py"), "1.2.3", str(self.build), str(self.out), *extra],
            check=True,
            capture_output=True,
        )
        return json.loads((self.out / "manifest.json").read_text())

    def test_the_screen_finds_its_firmware(self):
        manifest = self.run_tool("--summary", "What changed", "--release-url", "https://example.com/r")
        self.assertEqual(manifest["version"], "1.2.3")
        ota = ota_build(manifest, "ESP32-S3")
        self.assertIsNotNone(ota)
        # The path beside the manifest, a file that is there, with the MD5 the screen checks it by.
        url = firmware_url(SOURCE, ota["path"])
        self.assertEqual(url, "https://example.github.io/esp-skyscope/skyscope-1.2.3.ota.bin")
        image = (self.out / ota["path"]).read_bytes()
        self.assertEqual(image, (self.build / "firmware.ota.bin").read_bytes())
        self.assertRegex(ota["md5"], r"^[0-9a-f]{32}$")
        self.assertEqual(ota["md5"], hashlib.md5(image).hexdigest())
        self.assertEqual(ota["summary"], "What changed")
        self.assertEqual(ota["release_url"], "https://example.com/r")

    def test_another_chip_finds_nothing(self):
        self.assertIsNone(ota_build(self.run_tool(), "ESP32"))

    def test_a_browser_install_gets_the_factory_image(self):
        manifest = self.run_tool()
        parts = manifest["builds"][0]["parts"]
        self.assertEqual(parts, [{"path": "skyscope-1.2.3.factory.bin", "offset": 0}])
        self.assertTrue((self.out / parts[0]["path"]).is_file())

    def test_no_summary_no_field(self):
        ota = ota_build(self.run_tool(), "ESP32-S3")
        self.assertNotIn("summary", ota)
        self.assertNotIn("release_url", ota)


class TheReleaseMatchesTheConfig(unittest.TestCase):
    def test_chip(self):
        # The manifest's chip is the board's: ESPHOME_VARIANT for an esp32s3 is "ESP32-S3".
        board = (ROOT / "boards/waveshare-amoled-241.yaml").read_text()
        self.assertRegex(board, r"variant:\s*esp32s3", "the board is no longer an ESP32-S3: change CHIP in release_site.py")
        tool = (ROOT / "tools/release_site.py").read_text()
        self.assertIn('CHIP = "ESP32-S3"', tool)

    def test_version_is_one_the_release_reads(self):
        # The workflow reads `  version: "x.y.z"` from skyscope.yaml and compares it with the tag.
        config = (ROOT / "skyscope.yaml").read_text()
        found = re.findall(r'^  version: "?([^"\n]+)"?$', config, re.M)
        self.assertEqual(len(found), 1)
        self.assertRegex(found[0], r"^\d+\.\d+\.\d+$")

    def test_manifest_address(self):
        # The update entity asks for <releases>/manifest.json, where the workflow publishes it.
        config = (ROOT / "skyscope.yaml").read_text()
        self.assertIn("source: ${releases}/manifest.json", config)


if __name__ == "__main__":
    unittest.main()
