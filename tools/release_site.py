#!/usr/bin/env python3
"""The files a release publishes on GitHub Pages: the firmware, and the manifests the screens read.

    tools/release_site.py <version> <out dir> --builds <dir> [--boards <dir>] [--summary TEXT] [--release-url URL]

Every board in boards/ (or --boards) is published, from ESPHome's build of it in <dir>/skyscope-<board>/build (as
skyscope.yaml's build_path puts it), each in a folder of its own: <board>/manifest.json, which a screen of that board
asks for (skyscope.yaml: update:), beside its firmware. A screen is offered only its own board's firmware, so two boards
may share a chip.

The manifest is ESP Web Tools' (the same file installs a screen from a browser, with the factory image), with the
`ota` part ESPHome's http_request update reads: the update image's path (beside the manifest), its MD5, a summary and
the release's page. Its one build is named by the board's chip as ESPHOME_VARIANT names it ("ESP32-S3"), which the
update check requires. The firmware files carry the version and the board in their names, so a screen never gets a
stale copy from a cache, and the GitHub release can hold every board's.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parent.parent


def chip_of(board_file: Path) -> str:
    """The board's chip as ESPHOME_VARIANT names it: esp32s3 is "ESP32-S3", esp32 "ESP32"."""
    found = re.search(r"^  variant:\s*(\w+)", board_file.read_text(), re.M)
    if not found:
        raise SystemExit(f"{board_file}: no esp32 variant")
    variant = found.group(1).upper()
    return "ESP32" if variant == "ESP32" else "ESP32-" + variant.removeprefix("ESP32")


def manifest(version: str, chip: str, ota: str, factory: str, md5: str, args) -> dict:
    update = {"path": ota, "md5": md5}
    if args.summary:
        update["summary"] = args.summary
    if args.release_url:
        update["release_url"] = args.release_url
    return {
        "name": "esp-skyscope",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": chip, "parts": [{"path": factory, "offset": 0}], "ota": update}],
    }


def write(path: Path, content: dict) -> None:
    path.write_text(json.dumps(content, indent=2) + "\n")
    print(f"{path}:\n{json.dumps(content, indent=2)}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("out", type=Path)
    parser.add_argument("--builds", type=Path, required=True,
                        help="ESPHome's build folders, skyscope-<board>/build with firmware.ota.bin and .factory.bin")
    parser.add_argument("--boards", type=Path, default=ROOT / "boards", help="the boards' files, <board>.yaml")
    parser.add_argument("--summary", default="")
    parser.add_argument("--release-url", default="")
    args = parser.parse_args()

    boards = sorted(args.boards.glob("*.yaml"))
    if not boards:
        parser.error(f"no boards in {args.boards}")

    for board in boards:
        folder = args.builds / f"skyscope-{board.stem}" / "build"
        if not (folder / "firmware.ota.bin").is_file():
            parser.error(f"no build of {board.stem} in {folder}")
        (args.out / board.stem).mkdir(parents=True, exist_ok=True)
        name = f"skyscope-{args.version}-{board.stem}"
        ota, factory = f"{name}.ota.bin", f"{name}.factory.bin"
        shutil.copyfile(folder / "firmware.ota.bin", args.out / board.stem / ota)
        shutil.copyfile(folder / "firmware.factory.bin", args.out / board.stem / factory)
        md5 = hashlib.md5((args.out / board.stem / ota).read_bytes()).hexdigest()
        chip = chip_of(board)
        write(args.out / board.stem / "manifest.json", manifest(args.version, chip, ota, factory, md5, args))


if __name__ == "__main__":
    main()
