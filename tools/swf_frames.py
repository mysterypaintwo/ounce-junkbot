#!/usr/bin/env python3
"""Turn the original intro SWF into a flipbook atlas at build time.

swfrender from swftools always composites the *final* display list of a SWF and
ignores SHOWFRAME, so rendering the movie directly yields one static image.  We
therefore simulate the SWF timeline here: walk PLACEOBJECT2/REMOVEOBJECT2 tags,
freeze the display list at each sampled frame, and emit a tiny single-frame SWF
that swfrender can rasterise.  Nested sprites are frozen at the internal frame
they would be showing at that point in the movie.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

TAG_END = 0
TAG_SHOWFRAME = 1
TAG_DEFINESHAPE = 2
TAG_DEFINEFONT = 4
TAG_PLACEOBJECT = 4
TAG_DEFINEFONT2 = 10
TAG_DEFINESPRITE = 39
TAG_PLACEOBJECT2 = 26
TAG_REMOVEOBJECT2 = 28
TAG_DEFINESHAPE2 = 22
TAG_DEFINESHAPE3 = 32
TAG_DEFINEFONT3 = 43
SET_BACKGROUND = 9

DEFINITION_CODES = {
    TAG_DEFINESHAPE, TAG_DEFINESHAPE2, TAG_DEFINESHAPE3,
    TAG_DEFINEFONT, TAG_DEFINEFONT2,
    TAG_DEFINESPRITE,
    11, 12, 13,  # DEFINEFONTINFO, DEFINEFONTINFO2, DEFINEFONTNAME
}
# Note: tag 43 is FRAMELABEL, not a font definition.


@dataclass
class Tag:
    code: int
    data: bytes


class BitReader:
    def __init__(self, data: bytes, pos: int = 0) -> None:
        self.data = data
        self.pos = pos
        self.bit = 0

    def read(self, n: int) -> int:
        if n == 0:
            return 0
        value = 0
        for _ in range(n):
            if self.pos >= len(self.data):
                return 0
            byte = self.data[self.pos]
            value = (value << 1) | ((byte >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return value

    def read_signed(self, n: int) -> int:
        if n == 0:
            return 0
        value = self.read(n)
        if value & (1 << (n - 1)):
            value -= 1 << n
        return value

    def align(self) -> None:
        if self.bit:
            self.bit = 0
            self.pos += 1

    def remaining(self) -> bytes:
        return self.data[self.pos:]


def parse_tags(data: bytes, start: int = 0) -> list[Tag]:
    tags: list[Tag] = []
    pos = start
    while pos + 2 <= len(data):
        raw = struct.unpack_from("<H", data, pos)[0]
        code, length = raw >> 6, raw & 0x3F
        pos += 2
        if length == 0x3F:
            if pos + 4 > len(data):
                break
            length = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        end = pos + length
        if end > len(data):
            break
        tags.append(Tag(code, data[pos:end]))
        pos = end
        if code == TAG_END:
            break
    return tags


@dataclass
class Place:
    char_id: int = 0
    matrix: bytes = b""
    cxform: bytes = b""
    ratio: int | None = None
    name: bytes | None = None
    clip_depth: int | None = None
    clip: bytes | None = None
    placed_at: int = 1

    def clone(self) -> "Place":
        return Place(self.char_id, self.matrix, self.cxform, self.ratio,
                     self.name, self.clip_depth, self.clip, self.placed_at)


def parse_place(tag: Tag, frame: int) -> tuple[int, Place, bool] | None:
    if len(tag.data) < 3:
        return None
    br = BitReader(tag.data)
    flags = br.read(8)
    br.align()
    depth = struct.unpack_from("<H", tag.data, 1)[0]
    p = Place(placed_at=frame)

    has_clip_actions = bool(flags & 0x80)
    has_clip_depth = bool(flags & 0x40)
    has_name = bool(flags & 0x20)
    has_ratio = bool(flags & 0x10)
    has_cxform = bool(flags & 0x08)
    has_matrix = bool(flags & 0x04)
    has_char = bool(flags & 0x02)
    has_move = bool(flags & 0x01)

    pos = 3
    if has_char:
        if pos + 2 > len(tag.data):
            return None
        p.char_id = struct.unpack_from("<H", tag.data, pos)[0]
        pos += 2
    if has_matrix:
        m = BitReader(tag.data, pos)
        if m.read(1):
            bits = m.read(5)
            m.read_signed(bits)
            m.read_signed(bits)
        if m.read(1):
            bits = m.read(5)
            m.read_signed(bits)
            m.read_signed(bits)
        bits = m.read(5)
        m.read_signed(bits)
        m.read_signed(bits)
        m.align()
        p.matrix = tag.data[pos:m.pos]
        pos = m.pos
    if has_cxform:
        c = BitReader(tag.data, pos)
        has_add = c.read(1)
        has_mult = c.read(1)
        bits = c.read(4)
        n = bits * (4 if has_mult else 0) + bits * (4 if has_add else 0)
        c.read(n)
        c.align()
        p.cxform = tag.data[pos:c.pos]
        pos = c.pos
    if has_ratio:
        if pos + 2 > len(tag.data):
            return None
        p.ratio = struct.unpack_from("<H", tag.data, pos)[0]
        pos += 2
    if has_name:
        end = tag.data.find(b"\0", pos)
        if end < 0:
            return None
        p.name = tag.data[pos:end + 1]
        pos = end + 1
    if has_clip_depth:
        if pos + 2 > len(tag.data):
            return None
        p.clip_depth = struct.unpack_from("<H", tag.data, pos)[0]
        pos += 2
    if has_clip_actions:
        p.clip = tag.data[pos:]

    p.char_id = p.char_id if has_char else -1
    p.matrix = p.matrix if has_matrix else b""
    p.cxform = p.cxform if has_cxform else b""
    p.ratio = p.ratio if has_ratio else None
    p.name = p.name if has_name else None
    p.clip = p.clip if has_clip_actions else None
    p.placed_at = frame
    return depth, p, has_move


def simulate(tags: list[Tag], target_frame: int) -> dict[int, Place]:
    """Display list at target_frame (1-based)."""
    display: dict[int, Place] = {}
    frame = 1
    for tag in tags:
        if tag.code == TAG_SHOWFRAME:
            if frame >= target_frame:
                return dict(display)
            frame += 1
            continue
        if tag.code == TAG_PLACEOBJECT2:
            parsed = parse_place(tag, frame)
            if not parsed:
                continue
            depth, place, has_move = parsed
            old = display.get(depth)
            if has_move and old is not None:
                merged = old.clone()
                if place.char_id >= 0:
                    merged.char_id = place.char_id
                    merged.placed_at = frame
                if place.matrix:
                    merged.matrix = place.matrix
                if place.cxform:
                    merged.cxform = place.cxform
                if place.ratio is not None:
                    merged.ratio = place.ratio
                if place.name is not None:
                    merged.name = place.name
                if place.clip_depth is not None:
                    merged.clip_depth = place.clip_depth
                if place.clip:
                    merged.clip = place.clip
                display[depth] = merged
            else:
                if place.char_id < 0:
                    continue
                display[depth] = place
        elif tag.code == TAG_REMOVEOBJECT2 and len(tag.data) >= 2:
            display.pop(struct.unpack_from("<H", tag.data, 0)[0], None)
    return dict(display)


def frame_count(tags: list[Tag]) -> int:
    return sum(1 for t in tags if t.code == TAG_SHOWFRAME)


def write_tag(code: int, data: bytes) -> bytes:
    length = len(data)
    if length < 0x3F:
        return struct.pack("<H", (code << 6) | length) + data
    return struct.pack("<HI", (code << 6) | 0x3F, length) + data


def emit_place(depth: int, place: Place, char_id: int) -> bytes:
    flags = 0x02 | 0x04  # hasCharacter | hasMatrix
    if place.cxform:
        flags |= 0x08
    if place.ratio is not None:
        flags |= 0x10
    if place.name:
        flags |= 0x20
    if place.clip_depth is not None:
        flags |= 0x40
    if place.clip:
        flags |= 0x80
    data = bytes([flags]) + struct.pack("<H", depth) + struct.pack("<H", char_id)
    data += place.matrix
    if place.cxform:
        data += place.cxform
    if place.ratio is not None:
        data += struct.pack("<H", place.ratio)
    if place.name:
        data += place.name
    if place.clip_depth is not None:
        data += struct.pack("<H", place.clip_depth)
    if place.clip:
        data += place.clip
    return write_tag(TAG_PLACEOBJECT2, data)


def freeze_sprite(sprite: Tag, internal_frame: int, next_id: int) -> tuple[Tag, int]:
    """Return a DEFINESPRITE whose timeline is frozen at internal_frame."""
    inner = parse_tags(sprite.data, 4)
    total = frame_count(inner)
    if total <= 0:
        return sprite, next_id
    target = ((internal_frame - 1) % total) + 1
    display = simulate(inner, target)

    # Recursively freeze nested sprites.
    replacements: dict[int, int] = {}
    definitions: list[Tag] = []
    for inner_tag in inner:
        if inner_tag.code in DEFINITION_CODES:
            if inner_tag.code == TAG_DEFINESPRITE and len(inner_tag.data) >= 2:
                nested_id = struct.unpack_from("<H", inner_tag.data, 0)[0]
                nested = parse_tags(inner_tag.data, 4)
                nested_frames = frame_count(nested)
                # Find when this nested sprite was placed in the parent sprite.
                placed_at = 1
                for d, p in display.items():
                    if p.char_id == nested_id:
                        placed_at = p.placed_at
                        break
                nested_internal = target - placed_at + 1
                frozen, next_id = freeze_sprite(inner_tag, nested_internal, next_id)
                if frozen is not inner_tag:
                    new_id = struct.unpack_from("<H", frozen.data, 0)[0]
                    replacements[nested_id] = new_id
                    definitions.append(frozen)
                    continue
            definitions.append(inner_tag)

    # DEFINESPRITE = CharacterId UI16, FrameCount UI16, then nested tags.
    body = struct.pack("<HH", next_id, 1) + b"".join(
        write_tag(t.code, t.data) for t in definitions)
    for depth in sorted(display):
        place = display[depth]
        char_id = replacements.get(place.char_id, place.char_id)
        body += emit_place(depth, place, char_id)
    body += write_tag(TAG_SHOWFRAME, b"") + write_tag(TAG_END, b"")
    new_id = next_id
    next_id += 1
    return Tag(TAG_DEFINESPRITE, body), next_id


def build_frame_swf(original: bytes, frame: int, next_id: int = 10000) -> tuple[bytes, int]:
    # FWS header is 8 bytes, then a bit-packed RECT. This movie's RECT is 8
    # bytes, so the tag stream starts at 20 (confirmed by swfdump).
    header_end = 20
    tags = parse_tags(original, header_end)
    display = simulate(tags, frame)

    # Definitions needed, with sprites frozen at their current internal frame.
    definitions: list[Tag] = []
    replacements: dict[int, int] = {}
    # Key by (tag, id): a font and its DefineFontInfo legitimately share an id,
    # and dropping the font info makes swfrender rasterise text with garbage.
    seen: set[tuple[int, int]] = set()
    # Frozen sprites get fresh ids. Start above every id used by the original
    # movie: this SWF defines character 10371, so the old hard-coded 10000 start
    # produced duplicate ids and swfrender drew the wrong art (tearing pixels).
    max_id = max((struct.unpack_from("<H", t.data, 0)[0]
                  for t in tags if t.code in DEFINITION_CODES and len(t.data) >= 2),
                 default=0)
    next_id = max(next_id, max_id + 1)
    for tag in tags:
        if tag.code in DEFINITION_CODES and len(tag.data) >= 2:
            cid = struct.unpack_from("<H", tag.data, 0)[0]
            if (tag.code, cid) in seen:
                continue
            seen.add((tag.code, cid))
            if tag.code == TAG_DEFINESPRITE:
                placed_at = 1
                for place in display.values():
                    if place.char_id == cid:
                        placed_at = place.placed_at
                        break
                frozen, next_id = freeze_sprite(tag, frame - placed_at + 1, next_id)
                if frozen is not tag:
                    new_id = struct.unpack_from("<H", frozen.data, 0)[0]
                    replacements[cid] = new_id
                    definitions.append(frozen)
                    continue
            definitions.append(tag)
        elif tag.code == SET_BACKGROUND:
            definitions.append(tag)

    body = b"".join(write_tag(t.code, t.data) for t in definitions)
    for depth in sorted(display):
        place = display[depth]
        char_id = replacements.get(place.char_id, place.char_id)
        body += emit_place(depth, place, char_id)
    body += write_tag(TAG_SHOWFRAME, b"") + write_tag(TAG_END, b"")

    file_size = header_end + len(body)
    out = bytearray(original[:8])
    struct.pack_into("<I", out, 4, file_size)
    out += original[8:16]  # RECT
    out += struct.pack("<HH", 15 << 8, 1)  # 15 fps, one frame
    out += body
    return bytes(out), next_id


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("swf", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--end", type=int, default=0, help="0 = all frames")
    ap.add_argument("--step", type=int, default=1)
    args = ap.parse_args()

    original = args.swf.read_bytes()
    # The header is 8 bytes + an 8-byte RECT + 4 bytes of frame rate/count, so
    # the tag stream starts at 20 (build_frame_swf uses the same offset).
    total = frame_count(parse_tags(original, 20))
    end = args.end or total
    args.out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="jb_swf_") as tmp:
        temp = Path(tmp)
        ids: dict[str, int] = {}
        for frame in range(args.start, end + 1, args.step):
            swf, next_id = build_frame_swf(original, frame)
            swf_path = temp / "frame.swf"
            swf_path.write_bytes(swf)
            png_path = temp / "frame.png"
            result = subprocess.run(
                ["swfrender", str(swf_path), "-o", str(png_path)],
                capture_output=True)
            if result.returncode != 0 or not png_path.exists():
                print(f"frame {frame}: render failed: {result.stderr[:200]!r}")
                continue
            target = args.out / f"frame_{frame:04d}.png"
            target.write_bytes(png_path.read_bytes())
            print(f"frame {frame}: {target.stat().st_size} bytes")


if __name__ == "__main__":
    main()
