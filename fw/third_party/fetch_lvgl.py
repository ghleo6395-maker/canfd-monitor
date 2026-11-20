#!/usr/bin/env python3
"""Install the exact LVGL sources used by this firmware, verified per file."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
from urllib.request import urlopen

HERE = Path(__file__).resolve().parent
FW = HERE.parent
COMMIT = "85aa60d18b3d5e5588d7b247abf90198f07c8a63"
ARCHIVE_URL = f"https://codeload.github.com/lvgl/lvgl/tar.gz/{COMMIT}"


def verify_tree(root, manifest):
    for name, expected in manifest.items():
        path = root / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Missing or changed LVGL source: {name}")
    extra = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()} - manifest.keys()
    if extra:
        raise ValueError(f"Unexpected LVGL file: {min(extra)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="Use a previously downloaded upstream .tar.gz")
    args = parser.parse_args()
    manifest = json.loads((HERE / "lvgl-files.sha256.json").read_text())
    vendor = FW / "vendor"
    vendor.mkdir(exist_ok=True)
    dest = vendor / "lvgl"
    if dest.exists():
        verify_tree(dest, manifest)
    else:
        with tempfile.TemporaryDirectory(dir=vendor) as work:
            tmp = Path(work)
            archive = args.archive
            if archive is None:
                archive = tmp / "upstream.tar.gz"
                with urlopen(ARCHIVE_URL, timeout=60) as response, archive.open("wb") as out:
                    shutil.copyfileobj(response, out)
            tree = tmp / "selected"
            tree.mkdir()
            seen = set()
            with tarfile.open(archive, "r|gz") as tar:
                for member in tar:
                    top, _, name = member.name.partition("/")
                    if top != "lvgl-" + COMMIT or name not in manifest:
                        continue
                    if name in seen or not member.isfile():
                        raise ValueError(f"Unexpected LVGL archive entry: {name}")
                    path = tree / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with tar.extractfile(member) as source, path.open("xb") as output:
                        shutil.copyfileobj(source, output)
                    seen.add(name)
            verify_tree(tree, manifest)
            tree.rename(dest)
    print(f"Verified {len(manifest)} LVGL files at commit {COMMIT}")


if __name__ == "__main__":
    main()
