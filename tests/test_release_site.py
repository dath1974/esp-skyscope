"""tools/release_site.py writes what ESPHome's http_request update reads (http_request_update.cpp): run with
python3 -m unittest discover -s tests."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


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


BOARDS = sorted(p.stem for p in (ROOT / "boards").glob("*.yaml"))
RELEASES = "https://example.github.io/esp-skyscope"


class ReleaseSite(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.out = Path(self.dir.name, "site")
        self.builds = Path(self.dir.name, "builds")
        # A build for each board, as skyscope.yaml's build_path puts it, each image its own.
        for board in BOARDS:
            self.build(board)

    def build(self, board: str):
        folder = self.builds / f"skyscope-{board}" / "build"
        folder.mkdir(parents=True)
        (folder / "firmware.ota.bin").write_bytes(f"\xe9update image {board}".encode() * 100)
        (folder / "firmware.factory.bin").write_bytes(f"\xe9factory image {board}".encode() * 100)

    def tearDown(self):
        self.dir.cleanup()

    def run_tool(self, *extra, check=True):
        return subprocess.run(
            [sys.executable, str(ROOT / "tools/release_site.py"), "1.2.3", str(self.out), "--builds", str(self.builds),
             *extra],
            check=check,
            capture_output=True,
        )

    def manifest(self, board: str) -> dict:
        return json.loads((self.out / board / "manifest.json").read_text())

    def test_each_board_finds_its_own_firmware(self):
        self.run_tool("--summary", "What changed", "--release-url", "https://example.com/r")
        for board in BOARDS:
            manifest = self.manifest(board)
            self.assertEqual(manifest["version"], "1.2.3")
            chip = chip_of(board)
            self.assertEqual([b["chipFamily"] for b in manifest["builds"]], [chip])
            ota = ota_build(manifest, chip)
            self.assertIsNotNone(ota)
            # The path beside the manifest, a file that is there and its own board's, with the MD5 the screen checks it
            # by.
            source = f"{RELEASES}/{board}/manifest.json"
            url = firmware_url(source, ota["path"])
            self.assertEqual(url, f"{RELEASES}/{board}/skyscope-1.2.3-{board}.ota.bin")
            image = (self.out / board / ota["path"]).read_bytes()
            self.assertEqual(image, (self.builds / f"skyscope-{board}/build/firmware.ota.bin").read_bytes())
            self.assertRegex(ota["md5"], r"^[0-9a-f]{32}$")
            self.assertEqual(ota["md5"], hashlib.md5(image).hexdigest())
            self.assertEqual(ota["summary"], "What changed")
            self.assertEqual(ota["release_url"], "https://example.com/r")
        # Nothing at the top: a screen asks only in its board's folder.
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), BOARDS)

    def test_another_chip_finds_nothing(self):
        self.run_tool()
        for board in BOARDS:
            self.assertIsNone(ota_build(self.manifest(board), "ESP32"))

    def test_two_boards_may_share_a_chip(self):
        # A second ESP32-S3 board: each has its own manifest, so neither is offered the other's firmware.
        boards = Path(self.dir.name, "boards")
        boards.mkdir()
        for board in BOARDS:
            shutil.copyfile(ROOT / "boards" / f"{board}.yaml", boards / f"{board}.yaml")
        shutil.copyfile(ROOT / "boards/waveshare-amoled-241.yaml", boards / "another-s3.yaml")
        self.build("another-s3")
        self.run_tool("--boards", str(boards))
        mine, theirs = (ota_build(self.manifest(b), "ESP32-S3") for b in ("another-s3", "waveshare-amoled-241"))
        self.assertNotEqual(mine["md5"], theirs["md5"])
        self.assertEqual(mine["path"], "skyscope-1.2.3-another-s3.ota.bin")

    def test_a_board_without_a_build_is_refused(self):
        shutil.rmtree(self.builds / f"skyscope-{BOARDS[-1]}")
        done = self.run_tool(check=False)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn(b"no build of", done.stderr)

    def test_a_browser_install_gets_the_factory_image(self):
        self.run_tool()
        for board in BOARDS:
            parts = self.manifest(board)["builds"][0]["parts"]
            self.assertEqual(parts, [{"path": f"skyscope-1.2.3-{board}.factory.bin", "offset": 0}])
            self.assertTrue((self.out / board / parts[0]["path"]).is_file())

    def test_no_summary_no_field(self):
        self.run_tool()
        ota = ota_build(self.manifest("waveshare-amoled-241"), "ESP32-S3")
        self.assertNotIn("summary", ota)
        self.assertNotIn("release_url", ota)


def chip_of(board: str) -> str:
    """The board's chip, as ESPHOME_VARIANT names it: an esp32s3 is "ESP32-S3"."""
    config = (ROOT / "boards" / f"{board}.yaml").read_text()
    variant = re.search(r"^  variant:\s*(\w+)", config, re.M).group(1).upper()
    return "ESP32" if variant == "ESP32" else "ESP32-" + variant.removeprefix("ESP32")


class TheReleaseMatchesTheConfig(unittest.TestCase):
    def test_every_board_is_built_and_published(self):
        # release.yml builds every file in boards/ and hands their builds to release_site.py, which publishes each.
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        self.assertIn("for f in boards/*.yaml; do", workflow)
        self.assertIn('esphome -s board "$(basename "$f" .yaml)" compile skyscope.yaml', workflow)
        self.assertIn("tools/release_site.py \"$VERSION\" site --builds .esphome/build", workflow)
        self.assertIn("site/*/*.bin", workflow)
        # And a push builds every board too.
        tests = (ROOT / ".github/workflows/tests.yml").read_text()
        matrix = re.search(r"^        board: \[([^\]]*)\]$", tests, re.M)
        self.assertIsNotNone(matrix)
        self.assertEqual(sorted(b.strip() for b in matrix.group(1).split(",")), BOARDS, "tests.yml must build every board")

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
        # The update entity asks for <releases>/<board>/manifest.json, where the workflow publishes each board's.
        config = (ROOT / "skyscope.yaml").read_text()
        self.assertIn("source: ${releases}/${board}/manifest.json", config)

if __name__ == "__main__":
    unittest.main()
