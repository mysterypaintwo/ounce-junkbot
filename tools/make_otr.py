#!/usr/bin/env python3
"""Build junkbot.otr from a user-supplied Junkbot source.

The archive contains only data derived from the user's own original game. It
is deliberately not committed and is not embedded in the NRO. Two inputs are
supported:

  --code PATH   an exported junkbot-code tree (BMP/WAV/MP3/levels/SWF)
  --dir PATH    the original junkbot.dir movie

The exported tree is preferred when both are available because it can include
intro_anim.swf. Use --intro-swf to add the SWF explicitly with a .dir source.
"""

from __future__ import annotations

import argparse
import binascii
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
MAGIC = b"JOTR"
VERSION = 1
HEADER_SIZE = 72
ENTRY_SIZE = 152
NAME_SIZE = 128
MAX_SIZE = 256 * 1024 * 1024


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--code", type=Path, help="exported junkbot-code tree")
    source.add_argument("--dir", type=Path, help="original junkbot.dir movie")
    parser.add_argument("--intro-swf", type=Path,
                        help="original intro_anim.swf (optional)")
    parser.add_argument("--skip-intro", action="store_true",
                        help="omit the SWF intro even when one is available")
    parser.add_argument("--levels", type=Path,
                        help="extra level .txt directory used to fill gaps")
    parser.add_argument("-o", "--out", type=Path, default=HERE / "junkbot.otr",
                        help="output archive (default: ./junkbot.otr)")
    parser.add_argument("--label", default=None,
                        help="source label stored in the archive header")
    return parser.parse_args()


def collect(root: Path) -> list[tuple[str, bytes]]:
    files: list[tuple[str, bytes]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.name == ".DS_Store" or path.name.startswith("."):
            continue
        rel = path.relative_to(root).as_posix()
        if len(rel.encode("utf-8")) >= NAME_SIZE:
            raise SystemExit(f"asset path is too long: {rel}")
        payload = path.read_bytes()
        if len(files) >= 8192:
            raise SystemExit("too many asset files")
        files.append((rel, payload))
    if not files:
        raise SystemExit(f"no files generated in {root}")
    return files


def verify_generated(files: list[tuple[str, bytes]]) -> None:
    """Catch a half-generated package before it reaches the console."""
    sizes = {name: len(payload) for name, payload in files}
    levels = [name for name in sizes
              if name.startswith("levels/") and name.endswith(".txt")
              and not name.endswith("_LEVEL_LISTING.txt")]
    if len(levels) < 60:
        raise SystemExit(f"generated package has only {len(levels)}/60 levels")
    if sizes.get("levels/_LEVEL_LISTING.txt", 0) == 0:
        raise SystemExit("generated package has an empty level listing")

    header = HERE / "include" / "jb_audio_gain.h"
    if not header.is_file():
        print(f"package check: {len(levels)} levels, level listing")
        return
    expected = set(re.findall(r'\{\s*"([^"]+\.(?:wav|ogg))"',
                              header.read_text(encoding="utf-8")))

    def asset_path(name: str) -> str:
        prefix = "audio/music/" if name.endswith(".ogg") else "audio/sound-effects/"
        return prefix + name

    missing = sorted(name for name in expected
                     if sizes.get(asset_path(name), 0) == 0)
    if missing:
        raise SystemExit("generated package is missing audio: " +
                         ", ".join(missing[:12]) +
                         (" ..." if len(missing) > 12 else ""))
    print(f"package check: {len(levels)} levels, level listing, "
          f"{len(expected)} audio entries present and non-empty")


def pack(files: list[tuple[str, bytes]], out: Path, label: str) -> None:
    files.sort(key=lambda item: item[0])
    toc_offset = HEADER_SIZE
    data_offset = (toc_offset + len(files) * ENTRY_SIZE + 15) & ~15
    cursor = data_offset
    toc = bytearray()
    data = bytearray()
    for name, payload in files:
        padded = (len(payload) + 15) & ~15
        padded_payload = payload + b"\0" * (padded - len(payload))
        entry = name.encode("utf-8").ljust(NAME_SIZE, b"\0")
        entry += struct.pack("<QQII", cursor, len(payload),
                             binascii.crc32(padded_payload) & 0xFFFFFFFF, 0)
        if len(entry) != ENTRY_SIZE:
            raise AssertionError("entry size mismatch")
        toc += entry
        data += payload + b"\0" * (padded - len(payload))
        cursor += padded
    total = cursor
    if total > MAX_SIZE:
        raise SystemExit("asset package would exceed 256 MiB")

    has_intro = any(name == "intro/seq.bin" for name, _ in files)
    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<IIIQQ", header, 4, VERSION, 1 if has_intro else 0,
                     len(files), toc_offset, data_offset)
    label_bytes = label.encode("utf-8", "replace")[:31]
    header[40:40 + len(label_bytes)] = label_bytes

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    with tmp.open("wb") as fp:
        fp.write(header)
        fp.write(toc)
        fp.write(b"\0" * (data_offset - HEADER_SIZE - len(toc)))
        fp.write(data)
    tmp.replace(out)
    print(f"{out}: {len(files)} entries, {total} bytes "
          f"({label}, intro={'yes' if has_intro else 'no'})")


def main() -> int:
    args = parse_args()
    if args.code:
        if not args.code.is_dir():
            raise SystemExit(f"not a directory: {args.code}")
        source = args.code.resolve()
        label = args.label or f"repo:{source.name}"
        env_key = "JUNKBOT_CODE"
    else:
        if not args.dir.is_file():
            raise SystemExit(f"not a file: {args.dir}")
        source = args.dir.resolve()
        label = args.label or f"dir:{source.name}"
        env_key = "JUNKBOT_DIR"

    with tempfile.TemporaryDirectory(prefix="junkbot_otr_") as tmp:
        romfs = Path(tmp) / "romfs"
        env = os.environ.copy()
        env["ROMFS_OUT"] = str(romfs)
        env.pop("JUNKBOT_CODE", None)
        env.pop("JUNKBOT_DIR", None)
        env.pop("INTRO_SWF", None)
        env.pop("SKIP_INTRO", None)
        env.pop("JUNKBOT_LEVELS", None)
        env[env_key] = str(source)
        if args.skip_intro:
            env["SKIP_INTRO"] = "1"
        if args.levels:
            if not args.levels.is_dir():
                raise SystemExit(f"not a directory: {args.levels}")
            env["JUNKBOT_LEVELS"] = str(args.levels.resolve())
        if args.intro_swf:
            if not args.intro_swf.is_file():
                raise SystemExit(f"not a file: {args.intro_swf}")
            env["INTRO_SWF"] = str(args.intro_swf.resolve())
        result = subprocess.run([sys.executable, str(HERE / "tools/prepare_assets.py")],
                                env=env, cwd=HERE)
        if result.returncode != 0:
            return result.returncode
        files = collect(romfs)
        verify_generated(files)
        pack(files, args.out.resolve(), label)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
