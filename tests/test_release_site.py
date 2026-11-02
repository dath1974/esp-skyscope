"""tools/release_site.py writes what ESPHome's http_request update reads (http_request_update.cpp): run with
python3 -m unittest discover -s tests."""

import hashlib
import json
from pathlib import Path
import re
import shlex
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
        self.out = Path(self.dir.name, "site")
        # A build for each chip, each image its own.
        self.builds = {}
        for chip in ("ESP32-S3", "ESP32-P4"):
            build = Path(self.dir.name, chip)
            build.mkdir()
            (build / "firmware.ota.bin").write_bytes(f"\xe9update image {chip}".encode() * 100)
            (build / "firmware.factory.bin").write_bytes(f"\xe9factory image {chip}".encode() * 100)
            self.builds[chip] = build

    def tearDown(self):
        self.dir.cleanup()

    def run_tool(self, *extra, chips=("ESP32-S3", "ESP32-P4"), check=True):
        builds = [f"--build={chip}={self.builds[chip]}" for chip in chips]
        done = subprocess.run(
            [sys.executable, str(ROOT / "tools/release_site.py"), "1.2.3", str(self.out), *builds, *extra],
            check=check,
            capture_output=True,
        )
        if not check:
            return done
        return json.loads((self.out / "manifest.json").read_text())

    def test_the_screen_finds_its_firmware(self):
        manifest = self.run_tool("--summary", "What changed", "--release-url", "https://example.com/r")
        self.assertEqual(manifest["version"], "1.2.3")
        for chip, name in (("ESP32-S3", "esp32-s3"), ("ESP32-P4", "esp32-p4")):
            ota = ota_build(manifest, chip)
            self.assertIsNotNone(ota)
            # The path beside the manifest, a file that is there and its own chip's, with the MD5 the screen checks it
            # by.
            url = firmware_url(SOURCE, ota["path"])
            self.assertEqual(url, f"https://example.github.io/esp-skyscope/skyscope-1.2.3-{name}.ota.bin")
            image = (self.out / ota["path"]).read_bytes()
            self.assertEqual(image, (self.builds[chip] / "firmware.ota.bin").read_bytes())
            self.assertRegex(ota["md5"], r"^[0-9a-f]{32}$")
            self.assertEqual(ota["md5"], hashlib.md5(image).hexdigest())
            self.assertEqual(ota["summary"], "What changed")
            self.assertEqual(ota["release_url"], "https://example.com/r")

    def test_another_chip_finds_nothing(self):
        self.assertIsNone(ota_build(self.run_tool(), "ESP32"))
        self.assertIsNone(ota_build(self.run_tool(chips=("ESP32-S3",)), "ESP32-P4"))

    def test_one_build_for_each_chip(self):
        done = self.run_tool(chips=("ESP32-S3", "ESP32-S3"), check=False)
        self.assertNotEqual(done.returncode, 0)

    def test_a_browser_install_gets_the_factory_image(self):
        manifest = self.run_tool()
        for build, name in zip(manifest["builds"], ("esp32-s3", "esp32-p4")):
            parts = build["parts"]
            self.assertEqual(parts, [{"path": f"skyscope-1.2.3-{name}.factory.bin", "offset": 0}])
            self.assertTrue((self.out / parts[0]["path"]).is_file())

    def test_no_summary_no_field(self):
        ota = ota_build(self.run_tool(), "ESP32-S3")
        self.assertNotIn("summary", ota)
        self.assertNotIn("release_url", ota)


def release_builds() -> dict[str, str]:
    """The builds release.yml hands release_site.py: {chip: board}, from its --build=<chip>=<board's build folder>."""
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    found = re.findall(r"--build=(ESP32[-A-Z0-9]*)=\.esphome/build/skyscope-([a-z0-9-]+)/build", workflow)
    return {chip: board for chip, board in found}


class TheReleaseMatchesTheConfig(unittest.TestCase):
    def test_every_board_is_released_by_its_chip(self):
        # Each board's build goes in the manifest under its chip, as ESPHOME_VARIANT names it: an esp32s3 is "ESP32-S3".
        boards = sorted(p.stem for p in (ROOT / "boards").glob("*.yaml"))
        builds = release_builds()
        self.assertEqual(sorted(builds.values()), boards, "release.yml must build and publish every board")
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        for chip, board in builds.items():
            config = (ROOT / "boards" / f"{board}.yaml").read_text()
            variant = re.search(r"^  variant:\s*(\w+)", config, re.M)
            self.assertIsNotNone(variant, board)
            self.assertEqual(chip, "ESP32-" + variant.group(1).upper().removeprefix("ESP32"), board)
            self.assertIn(f"-s board {shlex.quote(board)} compile skyscope.yaml", workflow, board)

    def test_the_wifi_chips_firmware_is_the_one_named(self):
        # A board that carries its Wi-Fi chip's firmware names a file that is here, with the SHA-256 it has (ESPHome
        # checks it again at build time), and a file named for the version inside it.
        for board in sorted((ROOT / "boards").glob("*.yaml")):
            config = board.read_text()
            path = re.search(r"^    path: (boards/esp-hosted/network_adapter_\w+-([0-9.]+)\.bin)$", config, re.M)
            if not path:
                continue
            image = (ROOT / path.group(1)).read_bytes()
            sha = re.search(r"^    sha256: ([0-9a-f]{64})$", config, re.M)
            self.assertIsNotNone(sha, board.name)
            self.assertEqual(hashlib.sha256(image).hexdigest(), sha.group(1), board.name)
            # The version ESP-IDF writes into the image (esp_app_desc_t, after the image and segment headers), which
            # the screen compares with the chip's.
            desc = 24 + 8
            self.assertEqual(int.from_bytes(image[desc:desc + 4], "little"), 0xABCD5432, board.name)
            self.assertEqual(image[desc + 16:desc + 48].split(b"\0")[0].decode(), path.group(2), board.name)

    def test_the_default_board_is_one(self):
        config = (ROOT / "skyscope.yaml").read_text()
        board = re.search(r"^  board: ([a-z0-9-]+)$", config, re.M)
        self.assertIsNotNone(board)
        self.assertTrue((ROOT / "boards" / f"{board.group(1)}.yaml").is_file())
        self.assertIn("  board: !include boards/${board}.yaml", config)

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
