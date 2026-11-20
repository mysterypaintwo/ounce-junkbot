#!/usr/bin/env python3
"""Read and edit the hand-editable per-asset audio gain table.

include/jb_audio_gain.h is a build-time table: SFX are peak-normalised, and each
logical music track is mixed as one song (shared pre-gain, peak limiter,
20Hz-20kHz EQ, make-up limiter) before being split into runtime segments. This
file's gain offset is the per-asset BGM fader applied after that chain, at the
0 dBFS ceiling (-0.1 dB headroom). 0 dB keeps the asset at the bus level;
negative values make it quieter relative to the others. Positive values are
accepted but clamped to 0 dB at build time so a single asset can never clip.

After all groups are mixed they go through a second OST-wide loudness pass: each
group's integrated LUFS is matched to BGM_TARGET_LUFS plus its editable
BGM_GROUP_GAIN_DB offset.

Examples:
    python3 tools/audio_gain.py list
    python3 tools/audio_gain.py set voice_ohyeah.wav -3.5
    python3 tools/audio_gain.py set demo_4.1.ogg -6
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include" / "jb_audio_gain.h"

# Every asset is decoded and peak-normalised to this ceiling when romfs is
# built. -0.1 dB is a 0 dBFS master with just enough headroom for lossy OGG
# reconstruction, so the clip "just barely" reaches full scale without clipping.
MASTER_PEAK_DB = -0.1

# Second BGM pass: after every logical group is mixed, the whole OST is
# loudness-matched in integrated LUFS. Edit TARGET to change the shared level,
# or one GROUP value to make that group sit higher/lower in the soundtrack.
BGM_TARGET_LUFS = -18.0
BGM_GROUP_GAIN_DB = {
    "intro": 0.0,
    "demo6": 0.0,
    "lego1": 0.0,
    "demo_4": 0.0,
    "demo_5": 0.0,
    "demo_6": 0.0,
}


def group_target_lufs(name: str) -> float:
    """Integrated LUFS target for one BGM group in the OST-wide match pass."""
    return BGM_TARGET_LUFS + BGM_GROUP_GAIN_DB.get(name, 0.0)

_ROW = re.compile(
    r'\{\s*"(?P<file>[^"]+)"\s*,\s*(?P<gain>-?\d+(?:\.\d{1,3})?)f\s*\}')
_COUNT = re.compile(r"#define\s+JB_AUDIO_GAIN_COUNT\s+(\d+)")
_GAIN_INPUT = re.compile(r"-?\d+(?:\.\d{1,3})?")


def asset_key(name: str) -> str:
    name = name.strip()
    if name.startswith("undercover/"):
        return name
    return Path(name).name


def effective_peak_db(gain_db: float) -> float:
    """Final peak after mastering and the asset's trim.

    The mastering stage always targets MASTER_PEAK_DB; a positive offset is
    clamped to 0 dB so an asset can never be pushed above that ceiling.
    """
    return MASTER_PEAK_DB + min(0.0, gain_db)


def load_audio_gains(path: Path = HEADER) -> dict[str, float]:
    text = path.read_text(encoding="utf-8")
    rows: dict[str, float] = {}
    for match in _ROW.finditer(text):
        rows[match.group("file")] = float(match.group("gain"))
    if not rows:
        raise RuntimeError(f"no audio gain rows found in {path}")
    count = _COUNT.search(text)
    if count and len(rows) != int(count.group(1)):
        raise RuntimeError(
            f"{path}: {len(rows)} gain rows but JB_AUDIO_GAIN_COUNT is "
            f"{count.group(1)}")
    return rows


def set_audio_gain(path: Path, name: str, gain: float) -> bool:
    key = asset_key(name)
    text = path.read_text(encoding="utf-8")
    for match in _ROW.finditer(text):
        if match.group("file") != key:
            continue
        out = (text[:match.start("gain")] + f"{gain:.2f}" +
               text[match.end("gain"):])
        path.write_text(out, encoding="utf-8")
        return True
    return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--header", type=Path, default=HEADER,
                        help=f"gain header to read/edit (default: {HEADER})")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="show every asset's gain offset in dB")

    set_cmd = commands.add_parser("set", help="set one asset's gain in dB")
    set_cmd.add_argument("asset", help="romfs name, e.g. voice_ouch.wav")
    set_cmd.add_argument("db", help="e.g. -3.5 (positive values clamp to 0 dB)")

    args = parser.parse_args(argv)
    path: Path = args.header

    if args.command == "list":
        for name, gain in load_audio_gains(path).items():
            print(f"{name:32} {gain:+7.2f} dB")
        return 0

    if args.command == "set":
        raw = args.db.strip()
        if not _GAIN_INPUT.fullmatch(raw):
            parser.error("gain must be a number with at most 3 decimal places")
        gain = float(raw)
        if not -60.0 <= gain <= 24.0:
            parser.error("gain must be between -60 and +24 dB")
        if not set_audio_gain(path, asset_key(args.asset), gain):
            raise SystemExit(f"unknown asset: {asset_key(args.asset)}")
        print(f"{asset_key(args.asset)}: gain -> {gain:+.2f} dB")
        return 0

    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    sys.exit(main())
