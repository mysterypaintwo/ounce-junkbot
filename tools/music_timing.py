#!/usr/bin/env python3
"""Read and edit the hand-editable C music timing table.

include/jb_music_timing.h is the single source of truth so the trim values used
by prepare_assets.py are exactly the ones documented for the runtime. BPM and
`measures` are authoritative: for loop segments, `end` is derived from the BPM
and the total length of `measures` 4/4 bars at 44.1 kHz. Most segments are one
bar; demo_4.1 is the two-bar exception. Use this tool's `set`, `set-measures`,
or `recompute` commands to write that derived value back into the header.

Examples:
    python3 tools/music_timing.py list
    python3 tools/music_timing.py set demo_4.1 130.12345
    python3 tools/music_timing.py set-measures demo_4.1 2
    python3 tools/music_timing.py recompute
"""

from __future__ import annotations

import argparse
import re
import sys
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include" / "jb_music_timing.h"

SAMPLE_RATE = Decimal(44100)
LOOP_SAMPLES_PER_BEAT = Decimal(60) * SAMPLE_RATE  # 2,646,000 samples/beat @ BPM 1
BEATS_PER_LOOP = Decimal(4)
ONE_SAMPLE = Decimal("0.00001")
WHOLE_SAMPLE = Decimal(1)

# BPM accepts up to five decimal places. Sample positions stay integers.
# `measures` is 0 only for one-shot stingers; looped tracks need at least 1.
_ROW = re.compile(
    r"\{\s*\"(?P<file>[^\"]+)\"\s*,\s*(?P<bpm>-?\d+(?:\.\d{1,5})?)\s*,\s*"
    r"(?P<anacrusis>-?\d+)\s*,\s*(?P<measures>\d+)\s*,\s*"
    r"(?P<start>-?\d+)\s*,\s*(?P<loop_start>-?\d+)\s*,\s*"
    r"(?P<end>-?\d+)\s*,\s*(?P<one_shot>true|false)\s*\}"
)
_BPM_INPUT = re.compile(r"\d+(?:\.\d{1,5})?")


def track_key(name: str) -> str:
    return name if name.lower().endswith(".ogg") else name + ".ogg"


def parse_bpm(value: str) -> Decimal:
    if not _BPM_INPUT.fullmatch(value):
        raise ValueError("BPM must be a positive number with at most 5 decimal places")
    bpm = Decimal(value)
    if bpm <= 0:
        raise ValueError("BPM must be greater than zero")
    return bpm


def format_bpm(value: Decimal) -> str:
    return str(value.quantize(ONE_SAMPLE, rounding=ROUND_HALF_UP))


def loop_end(loop_start: int, bpm: Decimal, measures: int) -> int:
    """Exclusive loop end: loopStart + `measures` 4/4 bars, rounded to a sample."""
    if bpm <= 0:
        raise ValueError("looped tracks need a positive BPM")
    if measures < 1:
        raise ValueError("looped tracks need at least one measure")
    length = BEATS_PER_LOOP * measures * LOOP_SAMPLES_PER_BEAT / bpm
    return loop_start + int(length.quantize(WHOLE_SAMPLE, rounding=ROUND_HALF_UP))


def load_music_timings(path: Path = HEADER) -> dict[str, dict[str, object]]:
    text = path.read_text(encoding="utf-8")
    rows: dict[str, dict[str, object]] = {}
    for match in _ROW.finditer(text):
        data = match.groupdict()
        bpm = Decimal(data["bpm"])
        one_shot = data["one_shot"] == "true"
        measures = int(data["measures"])
        if not one_shot and measures < 1:
            raise ValueError(f"{data['file']}: looped tracks need at least one measure")
        loop_start = int(data["loop_start"])
        header_end = int(data["end"])
        end = (header_end if one_shot or bpm == 0
               else loop_end(loop_start, bpm, measures))
        rows[data["file"]] = {
            "bpm": bpm,
            "bpm_text": data["bpm"],
            "anacrusis": int(data["anacrusis"]),
            "measures": measures,
            "start": int(data["start"]),
            "loop_start": loop_start,
            "end": end,
            "header_end": header_end,
            "one_shot": one_shot,
        }
    if not rows:
        raise RuntimeError(f"no music timing rows found in {path}")
    return rows


def update_header(path: Path, bpm_updates: dict[str, Decimal] | None = None,
                  measure_updates: dict[str, int] | None = None,
                  recompute: bool = False) -> tuple[int, dict[str, tuple[int, int]]]:
    """Rewrite BPM, measure count and derived end values in place.

    Returns the number of changed rows and a map of track -> (old end, new end).
    """
    bpm_updates = bpm_updates or {}
    measure_updates = measure_updates or {}
    text = path.read_text(encoding="utf-8")
    out: list[str] = []
    last = 0
    changed = 0
    ends: dict[str, tuple[int, int]] = {}
    seen: set[str] = set()

    for match in _ROW.finditer(text):
        data = match.groupdict()
        name = data["file"]
        seen.add(name)
        old_bpm_text = data["bpm"]
        old_measures = int(data["measures"])
        old_end = int(data["end"])
        one_shot = data["one_shot"] == "true"

        if name in bpm_updates:
            bpm = bpm_updates[name]
        else:
            bpm = Decimal(old_bpm_text)
        if name in measure_updates:
            measures = measure_updates[name]
            if one_shot:
                raise ValueError(f"{name}: one-shot stingers have no measures")
            if measures < 1:
                raise ValueError(f"{name}: looped tracks need at least one measure")
        else:
            measures = old_measures

        if not one_shot and bpm <= 0:
            raise ValueError(f"{name}: looped tracks need a positive BPM")
        if not one_shot and measures < 1:
            raise ValueError(f"{name}: looped tracks need at least one measure")

        new_bpm_text = format_bpm(bpm)
        new_end = (old_end if one_shot or bpm == 0
                   else loop_end(int(data["loop_start"]), bpm, measures))

        row = match.group(0)
        # Replace rightmost spans first so earlier offsets stay valid.
        replacements: list[tuple[int, int, str]] = []
        if new_bpm_text != old_bpm_text:
            replacements.append((match.span("bpm")[0] - match.start(),
                                 match.span("bpm")[1] - match.start(),
                                 new_bpm_text))
        if measures != old_measures:
            replacements.append((match.span("measures")[0] - match.start(),
                                 match.span("measures")[1] - match.start(),
                                 str(measures)))
        if recompute or name in bpm_updates or name in measure_updates:
            if new_end != old_end:
                replacements.append((match.span("end")[0] - match.start(),
                                     match.span("end")[1] - match.start(),
                                     str(new_end)))
            ends[name] = (old_end, new_end)
        for start, stop, replacement in sorted(replacements, key=lambda item: item[0],
                                               reverse=True):
            row = row[:start] + replacement + row[stop:]

        if row != match.group(0):
            changed += 1
        out.append(text[last:match.start()])
        out.append(row)
        last = match.end()

    tracked = set(bpm_updates) | set(measure_updates)
    missing = [track_key(name) for name in tracked if track_key(name) not in seen]
    if missing:
        raise SystemExit("unknown track(s): " + ", ".join(sorted(missing)))

    out.append(text[last:])
    if changed:
        path.write_text("".join(out), encoding="utf-8")
    return changed, ends


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--header", type=Path, default=HEADER,
                        help=f"timing header to read/edit (default: {HEADER})")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("list", help="show BPM and derived loop end for every track")
    commands.add_parser("recompute", help="refresh derived end samples in the header")

    set_cmd = commands.add_parser("set", help="set one track's BPM (up to 5 decimals)")
    set_cmd.add_argument("track", help="romfs name with or without .ogg")
    set_cmd.add_argument("bpm", help="e.g. 130.12345")

    measures_cmd = commands.add_parser("set-measures",
                                       help="set a loop segment's measure count")
    measures_cmd.add_argument("track", help="romfs name with or without .ogg")
    measures_cmd.add_argument("measures", type=int, help="e.g. 2 (minimum 1)")

    args = parser.parse_args(argv)
    path: Path = args.header

    if args.command == "list":
        for name, row in load_music_timings(path).items():
            bpm = format_bpm(row["bpm"])  # type: ignore[arg-type]
            suffix = " one-shot" if row["one_shot"] else ""
            print(f"{name:18} BPM {bpm:>10}  {row['measures']:>2} bar(s)  "
                  f"end {row['end']:>7}{suffix}")
        return 0

    if args.command == "set":
        name = track_key(args.track)
        try:
            bpm = parse_bpm(args.bpm)
        except ValueError as exc:
            parser.error(str(exc))
        before = load_music_timings(path).get(name)
        if before is None:
            raise SystemExit(f"unknown track: {name}")
        changed, ends = update_header(path, {name: bpm})
        old_end, new_end = ends.get(name, (before["end"], before["end"]))
        print(f"{name}: BPM {format_bpm(before['bpm'])} -> {format_bpm(bpm)}, "
              f"end {old_end} -> {new_end}")
        return 0

    if args.command == "set-measures":
        name = track_key(args.track)
        before = load_music_timings(path).get(name)
        if before is None:
            raise SystemExit(f"unknown track: {name}")
        try:
            changed, ends = update_header(path, measure_updates={name: args.measures})
        except ValueError as exc:
            parser.error(str(exc))
        old_end, new_end = ends.get(name, (before["end"], before["end"]))
        print(f"{name}: measures {before['measures']} -> {args.measures}, "
              f"end {old_end} -> {new_end}")
        return 0

    if args.command == "recompute":
        changed, ends = update_header(path, recompute=True)
        print(f"recomputed {len(ends)} track(s); {changed} row(s) changed in {path}")
        return 0

    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    sys.exit(main())
