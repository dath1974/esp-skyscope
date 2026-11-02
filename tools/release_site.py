#!/usr/bin/env python3
"""The files a release publishes on GitHub Pages: the firmware, and the manifest the screens read.

    tools/release_site.py <version> <out dir> --build <chip>=<build dir> [--build ...] [--summary TEXT]
                          [--release-url URL]

One build for each board, named by its chip as ESPHOME_VARIANT names it ("ESP32-S3", "ESP32-P4"): a screen's update
check takes only the build for its own chip, so one manifest serves every board, and no two boards may share a chip.

The manifest is ESP Web Tools' (the same file installs a screen from a browser, with the factory image), with the
`ota` part ESPHome's http_request update reads: the update image's path (beside the manifest), its MD5, a summary and
the release's page. The firmware files carry the version and the chip in their names, so a screen never gets a stale
copy from a cache, nor another chip's.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil


def build_arg(text: str) -> tuple[str, Path]:
    chip, sep, folder = text.partition("=")
    if not sep or not chip or not folder:
        raise argparse.ArgumentTypeError(f"{text!r}: expected <chip>=<build dir>")
    return chip, Path(folder)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("out", type=Path)
    parser.add_argument(
        "--build",
        type=build_arg,
        action="append",
        required=True,
        help="<chip>=<ESPHome's build folder, with firmware.ota.bin and firmware.factory.bin>",
    )
    parser.add_argument("--summary", default="")
    parser.add_argument("--release-url", default="")
    args = parser.parse_args()

    chips = [chip for chip, _ in args.build]
    if len(set(chips)) != len(chips):
        parser.error(f"one build for each chip: {', '.join(chips)}")

    args.out.mkdir(parents=True, exist_ok=True)
    builds = []
    for chip, folder in args.build:
        name = f"skyscope-{args.version}-{chip.lower()}"
        ota, factory = f"{name}.ota.bin", f"{name}.factory.bin"
        shutil.copyfile(folder / "firmware.ota.bin", args.out / ota)
        shutil.copyfile(folder / "firmware.factory.bin", args.out / factory)
        update = {"path": ota, "md5": hashlib.md5((args.out / ota).read_bytes()).hexdigest()}
        if args.summary:
            update["summary"] = args.summary
        if args.release_url:
            update["release_url"] = args.release_url
        builds.append({"chipFamily": chip, "parts": [{"path": factory, "offset": 0}], "ota": update})

    manifest = {
        "name": "esp-skyscope",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": builds,
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
