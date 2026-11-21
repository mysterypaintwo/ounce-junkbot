#!/usr/bin/env python3
"""Assemble a release folder/zip containing the NRO and asset generator."""

from __future__ import annotations

import os
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NRO = ROOT / "build" / "junkbot.nro"
RELEASE = ROOT / "release"
LEGACY_ZIP = ROOT / "junkbot-release.zip"


def makefile_version() -> str:
    makefile = (ROOT / "Makefile").read_text()
    major = re.search(r"^VERSION_MAJOR\s*:=\s*(\d+)", makefile, re.M)
    minor = re.search(r"^VERSION_MINOR\s*:=\s*(\d+)", makefile, re.M)
    if not major or not minor:
        raise SystemExit("Makefile does not define VERSION_MAJOR/VERSION_MINOR")
    return f"v{major.group(1)}.{minor.group(1)}"


def release_version() -> str:
    version = (sys.argv[1] if len(sys.argv) > 1 else
               os.environ.get("JUNKBOT_VERSION") or makefile_version())
    if not re.fullmatch(r"v\d+(?:\.\d+){1,3}", version):
        raise SystemExit(f"invalid release version: {version}")
    return version


def copy_release_file(src: Path, dest: Path, version: str) -> None:
    data = src.read_bytes()
    if src.suffix.lower() in (".txt", ".bat"):
        data = data.replace(b"@VERSION@", version.encode())
    if src.suffix.lower() == ".bat":
        data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    dest.write_bytes(data)


def copy_tools(version: str) -> None:
    tools = RELEASE / "tools"
    (tools / "data").mkdir(parents=True, exist_ok=True)
    (RELEASE / "assets").mkdir(parents=True, exist_ok=True)
    (RELEASE / "include").mkdir(parents=True, exist_ok=True)
    for path in sorted((ROOT / "tools").glob("*.py")):
        shutil.copy2(path, tools / path.name)
    for path in sorted((ROOT / "tools" / "data").iterdir()):
        if path.is_file():
            shutil.copy2(path, tools / "data" / path.name)
    shutil.copytree(ROOT / "tools" / "vendor" / "directorfile",
                    tools / "vendor" / "directorfile",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
                    dirs_exist_ok=True)
    shutil.copy2(ROOT / "tools" / "make_assets.py", RELEASE / "make_assets.py")
    shutil.copy2(ROOT / "tools" / "make-assets.sh", RELEASE / "make-assets.sh")
    copy_release_file(ROOT / "tools" / "release" / "readme.txt",
                      RELEASE / "readme.txt", version)
    copy_release_file(ROOT / "tools" / "release" / "make-assets.bat",
                      RELEASE / "make-assets.bat", version)
    shutil.copy2(ROOT / "assets" / "04B_08__.TTF",
                 RELEASE / "assets" / "04B_08__.TTF")
    for name in ("jb_audio_gain.h", "jb_music_timing.h"):
        shutil.copy2(ROOT / "include" / name, RELEASE / "include" / name)
    os.chmod(RELEASE / "make-assets.sh", 0o755)


def make_zip(zip_path: Path) -> None:
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(RELEASE.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(RELEASE))


def main() -> None:
    if not NRO.is_file():
        raise SystemExit(f"build the NRO first: {NRO}")
    version = release_version()
    zip_path = ROOT / f"ounce-junkbot-{version}.zip"
    shutil.rmtree(RELEASE, ignore_errors=True)
    RELEASE.mkdir(parents=True)
    shutil.copy2(NRO, RELEASE / "junkbot.nro")
    copy_tools(version)
    make_zip(zip_path)
    LEGACY_ZIP.unlink(missing_ok=True)
    for old in ROOT.glob("ounce-junkbot-v*.zip"):
        if old != zip_path:
            old.unlink()
    print(f"{RELEASE}: NRO + asset generator ({version})")
    print(f"{zip_path}: ready to upload")


if __name__ == "__main__":
    main()
