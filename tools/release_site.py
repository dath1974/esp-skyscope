#!/usr/bin/env python3
"""The files a release publishes on GitHub Pages: the firmware, and the manifest the screens read.

    tools/release_site.py <version> <build dir> <out dir> [--summary TEXT] [--release-url URL]

The manifest is ESP Web Tools' (the same file installs a screen from a browser, with the factory image), with the
`ota` part ESPHome's http_request update reads: the update image's path (beside the manifest), its MD5, a summary and
the release's page. The firmware files carry the version in their names, so a screen never gets a stale copy from a
cache.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil

CHIP = "ESP32-S3"  # as ESPHOME_VARIANT names it; the update check takes only a build for its own chip


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("build", type=Path, help="ESPHome's build folder, with firmware.ota.bin and firmware.factory.bin")
    parser.add_argument("out", type=Path)
    parser.add_argument("--summary", default="")
    parser.add_argument("--release-url", default="")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    ota = f"skyscope-{args.version}.ota.bin"
    factory = f"skyscope-{args.version}.factory.bin"
    shutil.copyfile(args.build / "firmware.ota.bin", args.out / ota)
    shutil.copyfile(args.build / "firmware.factory.bin", args.out / factory)
    md5 = hashlib.md5((args.out / ota).read_bytes()).hexdigest()

    update = {"path": ota, "md5": md5}
    if args.summary:
        update["summary"] = args.summary
    if args.release_url:
        update["release_url"] = args.release_url
    manifest = {
        "name": "esp-skyscope",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": CHIP, "parts": [{"path": factory, "offset": 0}], "ota": update}],
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
