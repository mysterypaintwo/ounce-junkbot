#!/usr/bin/env python3
"""Extract the assets needed by the Switch 2 port from a Macromedia Director .dir.

This is deliberately small: it reads the resource map, the KEY* table, CASt
members, BITD bitmaps and `snd ` samples.  It writes a junkbot-code-like tree
(bitmaps/, sounds/, levels/, manifest.tsv) that prepare_assets.py can consume.
No extracted binary assets are stored in this repository.
"""

from __future__ import annotations

import json
import re
import struct
import sys
import wave
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "vendor"))

from directorfile.archive import load_director_archive  # noqa: E402
from builtin_palettes import (  # noqa: E402
    GRAYSCALE, JUNKBOT_LOSE, JUNKBOT_MAIN, JUNKBOT_UI, SYSTEM_MAC, SYSTEM_WIN,
)


def fourcc(value: int) -> bytes:
    return struct.pack(">I", value)


def clean_name(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("_")
    return value[:96] or "member"


class DirectorArchive:
    def __init__(self, path: Path):
        self.path = path
        with path.open("rb") as fp:
            archive = load_director_archive(fp)
        self.version = archive._parser.director_version
        self.resources: dict[int, tuple[str, bytes]] = {
            entry.index: (entry.tag, resource.data)
            for entry, resource in archive._parser.entries
        }
        self.positions = {entry.index: entry.position for entry, _ in archive._parser.entries}
        self.key: dict[tuple[int, bytes], int] = {}
        key_data = next((d for t, d in self.resources.values() if t == "KEY*"), None)
        if key_data is None:
            raise ValueError("Director file has no KEY* resource table")
        used = struct.unpack("<I", key_data[8:12])[0]
        offset = 12
        for _ in range(used):
            section, cast, tag = struct.unpack("<III", key_data[offset:offset + 12])
            offset += 12
            self.key[(cast, fourcc(tag))] = section

    def resource(self, section: int) -> bytes:
        entry = self.resources.get(section)
        return entry[1] if entry else b""

    def linked(self, member: int, tag: bytes) -> bytes:
        section = self.key.get((member, tag))
        return self.resource(section) if section is not None else b""

    def cast_lists(self) -> dict[int, list[int]]:
        """CAS* resources are cast-library member-order arrays (BE uint32)."""
        out: dict[int, list[int]] = {}
        for section, (tag, data) in self.resources.items():
            if tag != "CAS*":
                continue
            out[section] = [struct.unpack(">I", data[i:i + 4])[0]
                            for i in range(0, len(data) - 3, 4)]
        return out

    def sound_payload(self, member: int) -> bytes:
        """Read the nested sndH/sndS payload of a `snd ` Director chunk.

        `directorfile` (and LibreShockwave's generic reader) see these chunks as
        zero-length because the sound data is stored in little-endian sub-chunks
        after the outer fourcc.  The PCM really lives in the `sndS` sub-chunk.
        """
        section = self.key.get((member, SOUND_TAG))
        if section is None:
            return b""
        position = self.positions.get(section, 0)
        if not position:
            return b""
        with self.path.open("rb") as fp:
            fp.seek(position + 8)
            data = fp.read(1 << 20)
        pos = 0
        while pos + 8 <= len(data):
            tag = data[pos:pos + 4][::-1].decode("latin1")
            length = int.from_bytes(data[pos + 4:pos + 8], "little")
            payload = data[pos + 8:pos + 8 + length]
            if tag in ("sndS", "sndX"):
                return payload
            pos += 8 + length
        return b""

    def members(self) -> list[tuple[int, int, str, bytes, bytes]]:
        result = []
        for index, (tag, data) in sorted(self.resources.items()):
            if tag != "CASt" or len(data) < 12:
                continue
            member_type, info_len, data_len = struct.unpack(">III", data[:12])
            info = data[12:12 + info_len]
            specific = data[12 + info_len:12 + info_len + data_len]
            name = read_member_name(info)
            result.append((index, member_type, name, info, specific))
        return result


def read_member_name(info: bytes) -> str:
    if len(info) < 20:
        return ""
    data_offset = struct.unpack(">I", info[:4])[0]
    if data_offset <= 0 or data_offset >= len(info):
        return ""
    pos = data_offset
    if pos + 2 > len(info):
        return ""
    count = struct.unpack(">H", info[pos:pos + 2])[0]
    pos += 2
    if count <= 1 or count > 10000 or pos + count * 4 + 4 > len(info):
        return ""
    offsets = struct.unpack(">%dI" % count, info[pos:pos + count * 4])
    pos += count * 4
    items_len = struct.unpack(">I", info[pos:pos + 4])[0]
    pos += 4
    name_offset = offsets[1]
    name_end = offsets[2] if count > 2 else items_len
    if name_offset < 0 or name_end <= name_offset:
        return ""
    pos += name_offset
    if pos >= len(info):
        return ""
    length = info[pos]
    pos += 1
    if length <= 0 or pos + length > len(info):
        return ""
    return info[pos:pos + length].decode("mac_roman", errors="replace")


BITMAP_MEMBER = 1
SOUND_MEMBER = 6
TEXT_XTRA_MEMBER = 15


def bitmap_info(specific: bytes, version: int) -> tuple[int, int, int, int, int]:
    if len(specific) < 24:
        return 1, 1, 1, 1, -1
    raw_pitch = struct.unpack(">H", specific[:2])[0]
    top, left, bottom, right = struct.unpack(">hhhh", specific[2:10])
    width, height = right - left, bottom - top
    if version < 1200:
        depth = specific[20] if len(specific) > 20 else 1
        if depth == 0:
            depth = 1
        palette_id = -1
    else:
        depth = specific[23] if len(specific) > 23 else 1
        palette_id = struct.unpack(">h", specific[26:28])[0] - 1 if len(specific) >= 28 else -1
    pitch = raw_pitch & 0x3FFF
    return max(1, width), max(1, height), max(1, depth), pitch, palette_id


def decompress_rle(data: bytes, expected: int) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(data) and len(out) < expected:
        control = data[pos]
        pos += 1
        if control < 0x80:
            count = control + 1
            out.extend(data[pos:pos + count])
            pos += count
        elif control == 0x80:
            continue
        else:
            count = 257 - control
            if pos < len(data):
                out.extend(bytes([data[pos]]) * count)
                pos += 1
    return bytes(out[:expected])


def decompress_bitmap(data: bytes, expected: int) -> bytes:
    if len(data) >= expected:
        return data[:expected]
    if len(data) >= 2 and data[0] == 0x78 and data[1] in (0x01, 0x5E, 0x9C, 0xDA):
        try:
            return zlib.decompress(data)[:expected]
        except zlib.error:
            pass
    return decompress_rle(data, expected)


def palette_for(palette_id: int) -> list[tuple[int, int, int]]:
    """Map Director palette ids to the movie's original palettes.

    The exported BMPs in junkbot-code carry three custom 256-colour palettes.
    Director stores cast bitmaps with the palette order reversed relative to
    those BMPs: a BMP index ``i`` appears as ``255 - i`` in the .dir pixel data.
    The exported BMP palette's index 255 is the transparent background, which
    becomes index 0 in .dir data. Reversing here keeps .dir-derived bitmaps
    byte-identical to the originals (verified pixel-exact against the original
    sprite references).
    """
    if palette_id == -102:
        return JUNKBOT_MAIN[::-1]
    if palette_id == -8:
        return JUNKBOT_UI[::-1]
    if palette_id == -1:
        return JUNKBOT_LOSE[::-1]
    if palette_id == -3:
        return GRAYSCALE
    if palette_id == -101:
        return SYSTEM_WIN
    return SYSTEM_MAC


def decode_bitmap(data: bytes, width: int, height: int, depth: int,
                  pitch: int, palette_id: int) -> "Image.Image":
    from PIL import Image

    # Director's pitch is already in bytes per scanline. Uncompressed 16-bit
    # rows are interleaved RGB555 samples; RLE-compressed rows split each row
    # into a high-byte and low-byte plane. 32-bit rows are four byte planes.
    channels = 4 if depth == 32 else 2 if depth == 16 else 1
    expected = pitch * height
    planes = len(data) < expected
    raw = decompress_bitmap(data, expected)
    scan = width if depth == 32 else pitch

    image = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    pixels = image.load()
    palette = palette_for(palette_id)
    # Transparency is decided later by prepare_assets.py, which flood-fills
    # border-connected white; here we keep the raw palette colours verbatim.

    if depth == 8:
        for y in range(height):
            for x in range(width):
                index = raw[y * scan + x] if y * scan + x < len(raw) else 0
                pixels[x, y] = (*palette[index & 0xFF], 255)
    elif depth in (1, 2, 4):
        per_byte = 8 // depth
        mask = (1 << depth) - 1
        for y in range(height):
            for x in range(width):
                byte = raw[y * scan + x // per_byte] if y * scan + x // per_byte < len(raw) else 0
                shift = 8 - depth - (x % per_byte) * depth
                index = (byte >> shift) & mask
                pixels[x, y] = (*palette[index * 255 // mask], 255)
    elif depth == 16:
        for y in range(height):
            for x in range(width):
                if planes:
                    plane_stride = width
                    hi = raw[(y * 2) * plane_stride + x] if (y * 2) * plane_stride + x < len(raw) else 0
                    lo = raw[(y * 2 + 1) * plane_stride + x] if (y * 2 + 1) * plane_stride + x < len(raw) else 0
                    value = (hi << 8) | lo
                else:
                    pos = y * scan + x * 2
                    value = (raw[pos] << 8) | raw[pos + 1] if pos + 1 < len(raw) else 0
                r = (value >> 10) & 0x1F
                g = (value >> 5) & 0x1F
                b = value & 0x1F
                # The original exported BMP palettes scale 5-bit channels with
                # floor(v * 255 / 31). Bit replication ((v << 3) | (v >> 2))
                # is off by one for many levels, which visibly tinted digits
                # and backgrounds.
                pixels[x, y] = (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)
    elif depth == 32:
        # Director stores 32-bit bitmaps as four sequential byte planes per
        # scanline (mask, red, green, blue), not as one interleaved pixel
        # buffer.  The mask plane is not an alpha channel in the original
        # exports; transparency is derived from white by prepare_assets.py.
        for y in range(height):
            for x in range(width):
                r = raw[(y * 4 + 1) * scan + x] if (y * 4 + 1) * scan + x < len(raw) else 0
                g = raw[(y * 4 + 2) * scan + x] if (y * 4 + 2) * scan + x < len(raw) else 0
                b = raw[(y * 4 + 3) * scan + x] if (y * 4 + 3) * scan + x < len(raw) else 0
                pixels[x, y] = (r, g, b, 255)
    return image


SOUND_TAG = b"snd "


def detect_header_size(data: bytes) -> int:
    if len(data) >= 128:
        for base in (64, 96):
            for index in range(base, min(base + 32, len(data))):
                if 0x70 <= data[index] <= 0x90:
                    return base
        return 128
    return 64 if len(data) >= 64 else 0


def sound_sample_rate(data: bytes) -> int:
    rate_a = int.from_bytes(data[4:8], "big") / 6.144 if len(data) >= 8 else 22050
    rate_a = int(round(rate_a))
    if 15990 < rate_a < 16020:
        rate_a = 16000
    rate_b = int.from_bytes(data[0x16:0x18], "big") if len(data) >= 0x18 else 0
    rate_c = int.from_bytes(data[0x2A:0x2C], "big") if len(data) >= 0x2C else 0
    if rate_b == 22050:
        return 22050
    if rate_c == 44100:
        return 44100
    if 8000 <= rate_a <= 48000:
        return rate_a
    return 22050


def decode_sound(data: bytes) -> bytes | None:
    if data[:2] in (b"\xff\xfb", b"\xff\xf3", b"\xff\xf2"):
        return None
    header = detect_header_size(data)
    end = len(data)
    while end > 0 and data[end - 1] == 0xFF:
        end -= 1
    if end <= header:
        return None
    pcm = data[header:end]
    swapped = bytearray(len(pcm))
    for i in range(0, len(pcm) - 1, 2):
        swapped[i] = pcm[i + 1]
        swapped[i + 1] = pcm[i]
    rate = sound_sample_rate(data)
    out = bytearray()
    out += b"RIFF" + struct.pack("<I", 36 + len(swapped)) + b"WAVE"
    out += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    out += b"data" + struct.pack("<I", len(swapped)) + bytes(swapped)
    return bytes(out)


def extract_text(data: bytes) -> bytes:
    start = data.find(b",[info]")
    if start < 0:
        return b""
    start += 1
    end = start
    while end < len(data):
        byte = data[end]
        if byte in (9, 10, 13, 0x18) or 32 <= byte <= 126 or byte >= 128:
            end += 1
        else:
            break
    return data[start:end]


def title_from_text(text: bytes) -> str | None:
    match = re.search(rb"(?:^|\r|\n)title=([^\r\n]+)", text)
    if not match:
        return None
    return match.group(1).decode("mac_roman", errors="replace").strip()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: dir_extract.py <junkbot.dir> <output-dir>", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    for name in ("bitmaps", "sounds", "levels"):
        (output / name).mkdir(parents=True, exist_ok=True)

    archive = DirectorArchive(source)
    manifest = []
    bitmap_points: dict[str, dict[str, int]] = {}
    levels_by_member: dict[int, str] = {}
    counts = {"bitmap": 0, "sound": 0, "level": 0}

    for member, member_type, name, _info, specific in archive.members():
        safe = clean_name(name)

        if member_type == BITMAP_MEMBER:
            width, height, depth, pitch, palette_id = bitmap_info(specific, archive.version)
            reg_y, reg_x = struct.unpack(">hh", specific[18:22]) if len(specific) >= 22 else (0, 0)
            scroll_y, scroll_x = struct.unpack(">hh", specific[14:18]) if len(specific) >= 18 else (0, 0)
            bitmap_points.setdefault(name, {
                "member": member, "regX": reg_x, "regY": reg_y,
                "scrollX": scroll_x, "scrollY": scroll_y,
            })
            data = archive.linked(member, b"BITD")
            if data:
                try:
                    image = decode_bitmap(data, width, height, depth, pitch, palette_id)
                    path = output / "bitmaps" / f"{member:05d}_{safe}.png"
                    image.save(path)
                    manifest.append((member, "bitmap", name, "", "", "", f"bitmaps/{path.name}"))
                    counts["bitmap"] += 1
                except Exception as error:  # keep going; one bad cast member is not fatal
                    print(f"bitmap {member} ({name}): {error}", file=sys.stderr)

        elif member_type == SOUND_MEMBER:
            pcm = archive.sound_payload(member)
            if pcm:
                swapped = bytearray(len(pcm))
                for i in range(0, len(pcm) - 1, 2):
                    swapped[i] = pcm[i + 1]
                    swapped[i + 1] = pcm[i]
                wav = bytearray()
                wav += b"RIFF" + struct.pack("<I", 36 + len(swapped)) + b"WAVE"
                wav += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
                wav += b"data" + struct.pack("<I", len(swapped)) + swapped
                section = archive.key.get((member, SOUND_TAG), 0)
                path = output / "sounds" / f"{member:05d}_{safe}_{section}.wav"
                path.write_bytes(wav)
                manifest.append((member, "sound", name, "", section, "snd ", f"sounds/{path.name}"))
                counts["sound"] += 1
            mp3 = archive.linked(member, b"ediM")
            start = next((i for i in range(len(mp3) - 1)
                          if mp3[i] == 0xFF and (mp3[i + 1] & 0xE0) == 0xE0), -1)
            if start >= 0:
                end = len(mp3)
                while end > start and mp3[end - 1] == 0xFF:
                    end -= 1
                section = archive.key.get((member, b"ediM"), 0)
                path = output / "sounds" / f"{member:05d}_{safe}_{section}.mp3"
                path.write_bytes(mp3[start:end])
                manifest.append((member, "sound", name, "", section, "ediM", f"sounds/{path.name}"))
                if not pcm:
                    counts["sound"] += 1

        elif member_type == TEXT_XTRA_MEMBER:
            text = extract_text(archive.linked(member, b"XMED"))
            title = title_from_text(text) if text else None
            if title and b"[playfield]" in text and title != "loading level idea 2":
                path = output / "levels" / f"{title}.txt"
                path.write_bytes(text)
                levels_by_member[member] = title
                manifest.append((member, "level", title, "", "", "XMED", f"levels/{path.name}"))
                counts["level"] += 1

    # The original game iterates member(n, "levels"), whose order is stored as
    # a CAS* array of CASt member ids. Pick the array that consists entirely of
    # playfield members and emit the canonical listing from it.
    level_members = set(levels_by_member)
    order = next((members for members in archive.cast_lists().values()
                  if members and all(item in level_members for item in members)), None)
    listing = [levels_by_member[member] for member in order] if order else []
    (output / "level_listing.txt").write_text(
        "\n".join(listing) + ("\n" if listing else ""), encoding="utf-8")
    (output / "bitmap_points.json").write_text(
        json.dumps(bitmap_points, indent=1, sort_keys=True) + "\n", encoding="utf-8")

    with (output / "manifest.tsv").open("w", encoding="utf-8") as fp:
        fp.write("member\ttype\tname\tscript\tchunk\tfourcc\tasset\n")
        for row in manifest:
            fp.write("\t".join(str(value) for value in row) + "\n")

    print("extracted " + ", ".join(f"{count} {kind}" for kind, count in counts.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
