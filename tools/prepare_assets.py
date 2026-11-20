#!/usr/bin/env python3
"""Build romfs/ from an original Junkbot source.

Supported inputs (auto-detected, override with JUNKBOT_DIR / JUNKBOT_CODE):

  * junkbot.dir -- the original Macromedia Director movie. Cast members are
    extracted with tools/dir_extract.py into a local cache.
  * ../junkbot-code -- the original exported BMP/WAV/MP3/level tree.

No Janitorial artwork is copied: sprite atlases are packed from the original
bitmaps, and frame names from tools/data/sprite_frames.json are used only as a
list of names the renderer expects.  The bundled UI TTF is temporary.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import wave
from io import BytesIO
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from audio_gain import (BGM_TARGET_LUFS, MASTER_PEAK_DB, effective_peak_db,
                        group_target_lufs, load_audio_gains)
from music_timing import load_music_timings

HERE = Path(__file__).resolve().parent.parent
TOOLS = HERE / "tools"
DATA = TOOLS / "data"
ROMFS = HERE / "romfs"
CACHE = HERE / ".asset-cache"

SFX_FILES = {
    "turn1": "turn1", "blockpickup": "blockpickup", "blockdrop": "blockdrop",
    "blockclick": "blockclick", "h_button1": "h_button1", "fall": "fall",
    "headbonk1": "headbonk1", "eat1": "eat1", "garbage1": "garbage1",
    "switch_click": "switch_click", "switch_on": "switch_on",
    "switch_off": "switch_off", "fire": "fire", "electricity1": "electricity1",
    "robottouch4": "robottouch4", "shieldon2": "shieldon2",
    "h_powerup1": "h_powerup1", "h_powerdown3": "h_powerdown3",
    "h_powerup3": "h_powerup3",   # original tabSwitch / building page turn
    "voice_ohyeah": "voice_ohyeah", "voice_ouch": "voice_ouch",
    "voice_uhoh": "voice_uhoh", "jump3": "jump3", "fan": "fan",
    "drip1": "drip1", "drip2": "drip2", "drip3": "drip3",
}
MUSIC_FILES = [
    "intro_1.1", "intro_1.2", "intro_1.3",
    "demo6_3", "demo6_5", "demo6_6", "demo6_end",
    "lego1.1", "lego1.2", "lego1.3", "lego1.end",
    "demo_4.1", "demo_4.3", "demo_4.5", "demo_4.6", "demo_4.end",
    "demo_5.1", "demo_5.2", "demo_5.7", "demo_5.8", "demo_5.end",
    "demo_6.1", "demo_6.2", "demo_6.4", "demo_6.5", "demo_6.end",
]
# The Undercover Exclusive sounds are absent from the approved main-game
# sources. Keep the runtime paths stable with short silent placeholders.
SILENT_SFX = ["undercover/laser_hit", "undercover/teleport"]

# A logical music track is mixed as one continuous song, then split back into
# the runtime segments. Each group gets ONE shared pre-gain from the peak of all
# its samples together, so a hot clip such as demo_6.5 cannot be individually
# normalised to the level of the rest of the track.
MUSIC_GROUPS = [
    ("intro",  ["intro_1.1", "intro_1.2", "intro_1.3"]),
    ("demo6",  ["demo6_3", "demo6_5", "demo6_6", "demo6_end"]),
    ("lego1",  ["lego1.1", "lego1.2", "lego1.3", "lego1.end"]),
    ("demo_4", ["demo_4.1", "demo_4.3", "demo_4.5", "demo_4.6", "demo_4.end"]),
    ("demo_5", ["demo_5.1", "demo_5.2", "demo_5.7", "demo_5.8", "demo_5.end"]),
    ("demo_6", ["demo_6.1", "demo_6.2", "demo_6.4", "demo_6.5", "demo_6.end"]),
]
# Every effect is an ordered component on a mixer bus. Add, remove or reorder
# these lists to change the sound; the renderer walks them from first to last.
# The group chain sees the whole logical track before it is split into OGGs.
BGM_GROUP_CHAIN = [
    {"type": "peak_gain", "target_db": MASTER_PEAK_DB},
    {"type": "limiter", "limit": 0.989, "attack": 5, "release": 50,
     "normalize": False},
    {"type": "eq", "highpass": 20, "lowpass": 20000, "order": 2},
    {"type": "limiter", "limit": 0.989, "attack": 5, "release": 50,
     "normalize": True},
]
# Per-segment components run after the group song has been split. This is where
# the editable per-asset BGM bus trims live; the runtime 80% fader is separate.
BGM_SEGMENT_CHAIN = [
    {"type": "fader", "clamp_positive_db": 0.0},
]
# Safety offset before Vorbis encoding. The encode step then measures the
# decoded peak and nudges this until every OGG lands just under the ceiling.
BGM_ENCODE_SAFETY_DB = -0.8
BGM_ENCODE_TOLERANCE_DB = 0.05
# Keep the measured reference this far under the ceiling: another segment in
# the same group can decode a few hundredths of a dB louder after encoding.
BGM_ENCODE_HEADROOM_DB = 0.3
BGM_MAX_ENCODE_STEP_DB = 0.5

COLOR_TO_ORIGINAL = {
    "blue": "BLUE", "red": "RED", "green": "GREEN", "yellow": "YELLOW",
    "white": "WHITE", "immobile": "GRAY",
}
ANIM_TO_ORIGINAL = {
    "walk_r": "WALK_R", "walk_l": "WALK_L",
    "die": "DEAD_GENERIC", "water_die": "DEAD_DRIP",
    "eat_start": "EAT_R", "shield_on_r": "SHIELDEAT_R",
    "shield_on_l": "SHIELDEAT_L", "shield_walk_r": "SHIELDWALK_R",
    "shield_walk_l": "SHIELDWALK_L",
}
CLIMB_TO_ORIGINAL = {
    "u": "HAZ_CLIMBER_FLOAT_UP", "d": "HAZ_CLIMBER_FLOAT_DOWN",
    "r": "HAZ_CLIMBER_WALK_R", "l": "HAZ_CLIMBER_walk_l",
}


def norm(name: str) -> str:
    name = Path(name).stem.lower()
    name = re.sub(r"_s[1-4]$", "", name)
    return re.sub(r"[^a-z0-9]+", "_", name).strip("_")


def norm_full(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", Path(name).stem.lower()).strip("_")


def audio_key(name: str) -> str:
    """Strip only a real audio extension; keep dotted track names intact."""
    value = str(name)
    for extension in (".wav", ".mp3"):
        if value.lower().endswith(extension):
            value = value[:-len(extension)]
            break
    return re.sub(r"[^a-z0-9]+", "_", value.lower()).strip("_")


def prefer_audio(index: dict[str, Path], key: str, path: Path) -> None:
    """Index a sound, preferring its full-length MP3 over the short `snd` WAV.

    The original cast stores music twice: a short internal `snd ` WAV (0.2-2.2 s)
    and the full-length `ediM` MP3 (2-4 s). The manifest lists WAVs first, so
    plain setdefault() would otherwise pick the truncated variant.
    """
    old = index.get(key)
    if old is None or (old.suffix.lower() == ".wav" and path.suffix.lower() == ".mp3"):
        index[key] = path


def title_key(title: str) -> str:
    """Lookup key only: the original cast text contains literal double spaces."""
    return re.sub(r"\s+", " ", title).strip()


def title_lookup_key(title: str) -> str:
    """Punctuation-insensitive level lookup (junkbot-code uses shorter names)."""
    return re.sub(r"[^a-z0-9]+", "", title.lower())


def load_points(path: Path) -> dict[str, dict[str, int]]:
    if not path.exists():
        return {}
    return {str(name).lower(): value
            for name, value in json.loads(path.read_text()).items()}


def frame_to_original(name: str, index: dict[str, Path],
                      full: dict[str, Path] | None = None) -> Path | None:
    """Map a renderer frame name to an original cast member."""
    key = norm(name)
    if key in index:
        return index[key]

    match = re.fullmatch(r"brick_(blue|red|green|yellow|white|immobile)_(\d+)", key)
    if match:
        color, studs = match.groups()
        for width in (int(studs), 8, 6, 4, 3, 2, 1):
            candidate = f"brick_{width:02d}_{COLOR_TO_ORIGINAL[color].lower()}"
            if candidate in index:
                return index[candidate]

    match = re.fullmatch(r"minifig_(.+)_(\d+)", key)
    if match:
        anim, frame = match.groups()
        prefix = ANIM_TO_ORIGINAL.get(anim)
        if prefix:
            for suffix in ("s3", "s1", "s4", "s2"):
                candidate = f"minifig_{prefix.lower()}_{frame}_{suffix}"
                if full and candidate in full:
                    return full[candidate]

    match = re.fullmatch(r"climbbot_walk_([urdl])_(\d+)", key)
    if match:
        direction, frame = match.groups()
        for suffix in ("s1", "s2"):
            candidate = f"{CLIMB_TO_ORIGINAL[direction].lower()}_{frame}_{suffix}"
            if full and candidate in full:
                return full[candidate]

    match = re.fullmatch(r"gearbot_walk_([lr])_(\d+)", key)
    if match:
        direction, frame = match.groups()
        original = "l" if direction == "l" else "r"
        for suffix in ("s1", "s2"):
            candidate = f"haz_dumbfloat_{original}_{frame}_{suffix}"
            if full and candidate in full:
                return full[candidate]

    match = re.fullmatch(r"flybot_(\d+)", key)
    if match:
        frame = match.group(1)
        for suffix in ("s1", "s2"):
            candidate = f"haz_float_active_{frame}_{suffix}"
            if full and candidate in full:
                return full[candidate]

    # Substring fallback for names such as "lite_number_1".
    for candidate, path in index.items():
        if candidate.startswith(key + "_") or key.startswith(candidate + "_"):
            return path
    return None


class Source:
    def __init__(self) -> None:
        self.name = ""
        self.bitmaps: dict[str, Path] = {}
        self.bitmaps_full: dict[str, Path] = {}
        self.sounds: dict[str, Path] = {}
        self.levels: dict[str, Path] = {}
        self.level_order: list[str] = []
        self.points: dict[str, dict[str, int]] = {}

    @classmethod
    def detect(cls) -> "Source":
        env_dir = os.environ.get("JUNKBOT_DIR")
        env_code = os.environ.get("JUNKBOT_CODE")
        default_dir = Path.home() / "Downloads/Junkbot-DIRs/junkbot.dir"
        default_code = HERE.parent / "junkbot-code"
        dir_path = Path(env_dir).resolve() if env_dir else (
            default_dir if default_dir.exists() else None)
        code_path = Path(env_code).resolve() if env_code else (
            default_code if default_code.exists() else None)

        if env_code or (dir_path is None and code_path):
            if not code_path or not code_path.exists():
                raise SystemExit(f"Junkbot code source not found: {code_path}")
            return cls.from_code(code_path)
        if not dir_path or not dir_path.exists():
            raise SystemExit(
                "No original Junkbot source found. Set JUNKBOT_DIR to junkbot.dir "
                "or JUNKBOT_CODE to the junkbot-code tree.")
        return cls.from_dir(dir_path)

    @classmethod
    def from_dir(cls, path: Path) -> "Source":
        out = CACHE / "dir"
        out.mkdir(parents=True, exist_ok=True)
        manifest = out / "manifest.tsv"
        listing_path = out / "level_listing.txt"
        points_path = out / "bitmap_points.json"
        cache_files = [manifest, listing_path, points_path]
        needs_extract = any(not cached.exists() for cached in cache_files)
        if not needs_extract:
            newest_cache = max(cached.stat().st_mtime for cached in cache_files)
            newest_input = max(item.stat().st_mtime for item in (
                path, TOOLS / "dir_extract.py", Path(__file__)))
            needs_extract = newest_cache < newest_input
        if needs_extract:
            subprocess.run(
                ["python3", str(TOOLS / "dir_extract.py"), str(path), str(out)],
                check=True)
        source = cls()
        source.name = f"junkbot.dir ({path})"
        source.points = load_points(points_path) or load_points(DATA / "bitmap_points.json")
        if listing_path.exists():
            source.level_order = [line.strip() for line in
                                  listing_path.read_text(encoding="utf-8").splitlines()
                                  if line.strip()]
        with manifest.open(encoding="utf-8") as fp:
            next(fp, None)
            for line in fp:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 7:
                    continue
                kind, name, asset = parts[1], parts[2], parts[6]
                target = out / asset
                if not target.exists():
                    continue
                key = norm(name)
                if kind == "bitmap":
                    source.bitmaps.setdefault(key, target)
                    source.bitmaps_full.setdefault(norm_full(name), target)
                elif kind == "sound" and target.suffix in (".wav", ".mp3"):
                    prefer_audio(source.sounds, audio_key(name), target)
                elif kind == "level":
                    source.levels.setdefault(title_key(name), target)
                    source.levels.setdefault(name.strip(), target)
                    source.levels.setdefault(title_lookup_key(name), target)
        return source

    @classmethod
    def from_code(cls, path: Path) -> "Source":
        source = cls()
        source.name = f"junkbot-code ({path})"
        source.points = load_points(DATA / "bitmap_points.json")
        for image in path.rglob("*.bmp"):
            source.bitmaps.setdefault(norm(image.name), image)
            source.bitmaps_full.setdefault(norm_full(image.name), image)
        for audio in list(path.rglob("*.wav")) + list(path.rglob("*.mp3")):
            prefer_audio(source.sounds, audio_key(audio.name), audio)
        for level in (path / "files/levels").glob("*.txt"):
            source.levels.setdefault(title_key(level.stem), level)
            source.levels.setdefault(level.stem.strip(), level)
            source.levels.setdefault(title_lookup_key(level.stem), level)
        return source


def key_white(image: Image.Image) -> Image.Image:
    """Make every pure-white pixel transparent (used for UI/menu artwork)."""
    data = np.array(image.convert("RGBA"))
    data[np.all(data[:, :, :3] == 255, axis=2), 3] = 0
    data[data[:, :, 3] == 0, :3] = 255
    return Image.fromarray(data, "RGBA")


# Director's exported bitmaps use saturated primary colours as registration
# markers. Once the white background is keyed out, those single pixels are left
# floating at frame edges and show up as random red specks in game (most
# visibly on the fan-air animation). They never form real artwork: a genuine
# detail has at least two opaque 4-neighbours.
MARKER_COLORS = (
    (255, 0, 0), (0, 255, 0), (0, 0, 255),
    (255, 0, 255), (0, 255, 255), (255, 255, 0),
)


def strip_isolated_markers(image: Image.Image) -> Image.Image:
    """Drop isolated primary-colour registration pixels left by Director."""
    data = np.array(image.convert("RGBA"))
    opaque = data[:, :, 3] > 0
    marker = np.zeros(opaque.shape, dtype=bool)
    for red, green, blue in MARKER_COLORS:
        marker |= ((data[:, :, 0] == red) & (data[:, :, 1] == green) &
                   (data[:, :, 2] == blue) & opaque)
    if not marker.any():
        return image

    neighbours = np.zeros(opaque.shape, dtype=np.int8)
    neighbours[1:, :] += opaque[:-1, :]
    neighbours[:-1, :] += opaque[1:, :]
    neighbours[:, 1:] += opaque[:, :-1]
    neighbours[:, :-1] += opaque[:, 1:]
    drop = marker & (neighbours <= 1)
    if not drop.any():
        return image
    data[drop, :3] = 255
    data[drop, 3] = 0
    return Image.fromarray(data, "RGBA")


def make_transparent(image: Image.Image, colors: list[list[int]]) -> Image.Image:
    """Remove the white background by flood-filling from the image border.

    Junkbot bitmaps use white both as the transparent background *and* as a
    real colour (white bricks, eyes). Only border-connected white belongs to
    the background, so flood-fill keeps interior white opaque. A recipe may
    override the key colour; otherwise white (255,255,255) is used.
    """
    data = np.array(image.convert("RGBA"))
    key = tuple(colors[0][:3]) if colors and len(colors[0]) >= 3 else (255, 255, 255)
    white = np.all(data[:, :, :3] == key, axis=2)
    height, width = white.shape
    seen = np.zeros_like(white)
    queue = deque()
    for x in range(width):
        for y in (0, height - 1):
            if white[y, x] and not seen[y, x]:
                seen[y, x] = True
                queue.append((y, x))
    for y in range(height):
        for x in (0, width - 1):
            if white[y, x] and not seen[y, x]:
                seen[y, x] = True
                queue.append((y, x))
    while queue:
        y, x = queue.popleft()
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < height and 0 <= nx < width and white[ny, nx] and not seen[ny, nx]:
                seen[ny, nx] = True
                queue.append((ny, nx))
    data[seen, 3] = 0
    return Image.fromarray(data, "RGBA")


def compose_frame(spec: dict, source: Source) -> Image.Image | None:
    """Compose one renderer frame from original cast bitmaps and CASt points."""
    if isinstance(spec, list):
        spec = {"layers": spec}
    field = spec.get("pointField", "reg")
    layers: list[tuple[Image.Image, int, int]] = []
    for item in spec.get("layers", []):
        name = str(item["source"])
        path = (source.bitmaps_full.get(norm_full(name)) or
                source.bitmaps.get(norm(name)))
        if path is None:
            return None
        with Image.open(path) as source_image:
            image = make_transparent(source_image, item.get("transparent", []))
        if item.get("keyWhite"):
            # Some decals (DANGER, digits) use white as enclosed background
            # pixels, so border flood-fill alone leaves white holes visible.
            image = key_white(image)
        point_name = str(item.get("pointsFrom") or name).lower()
        point = source.points.get(point_name, {})
        px = int(point.get(field + "X", point.get("regX", 0)))
        py = int(point.get(field + "Y", point.get("regY", 0)))
        if "offset" in item:
            px += int(item["offset"][0])
            py += int(item["offset"][1])
        layers.append((image, py, px))
    if not layers:
        return None

    max_y = max(layer[1] for layer in layers)
    max_x = max(layer[2] for layer in layers)
    offsets = [(max_x - layer[2], max_y - layer[1]) for layer in layers]
    min_x = min(offset for offset, _ in offsets)
    min_y = min(offset for _, offset in offsets)
    width = max(offset[0] + layer[0].width for offset, layer in zip(offsets, layers)) - min_x
    height = max(offset[1] + layer[0].height for offset, layer in zip(offsets, layers)) - min_y
    out = Image.new("RGBA", (max(1, width), max(1, height)), (0, 0, 0, 0))
    for (image, _, _), (ox, oy) in zip(layers, offsets):
        out.alpha_composite(image, (ox - min_x, oy - min_y))

    crop = spec.get("outputCrop")
    if crop:
        cx, cy, cw, ch = (int(value) for value in crop)
        out = out.crop((cx, cy, cx + cw, cy + ch))
    else:
        box = out.getbbox()
        out = out.crop(box) if box else out

    # Clear Director registration pixels only after the visible bounds are
    # decided, so dropping them cannot shift or resize the frame on screen.
    out = strip_isolated_markers(out)

    # alpha_composite zeroes RGB where every layer was transparent; the
    # original exports keep the white background colour with alpha 0.
    data = np.array(out)
    data[data[:, :, 3] == 0, :3] = 255
    return Image.fromarray(data, "RGBA")


def pack_recipe_atlas(name: str, recipes: dict, source: Source,
                      out_png: Path, out_tsv: Path) -> None:
    images: dict[str, Image.Image] = {}
    for frame_name, spec in recipes.items():
        image = compose_frame(spec, source)
        if image is not None:
            images[frame_name] = image

    if not images:
        Image.new("RGBA", (1, 1), (0, 0, 0, 0)).save(out_png)
        out_tsv.write_text("")
        print(f"{out_tsv.relative_to(HERE)}: 0/{len(recipes)} frames (no original sources)")
        return

    pad = 1
    x = y = row_height = 0
    rects: dict[str, tuple[int, int, int, int]] = {}
    max_width = 2048
    for frame_name in sorted(images, key=lambda key: (-images[key].height, -images[key].width)):
        image = images[frame_name]
        if x + image.width + pad > max_width:
            x = 0
            y += row_height + pad
            row_height = 0
        rects[frame_name] = (x, y, image.width, image.height)
        x += image.width + pad
        row_height = max(row_height, image.height)

    atlas = Image.new("RGBA", (max_width, y + row_height + pad), (0, 0, 0, 0))
    for frame_name, (px, py, _width, _height) in rects.items():
        atlas.alpha_composite(images[frame_name], (px, py))
    atlas.save(out_png)

    lines = []
    for frame_name in recipes:
        if frame_name in rects:
            px, py, width, height = rects[frame_name]
            lines.append(f"{frame_name}\t{px}\t{py}\t{width}\t{height}")
    out_tsv.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
    print(f"{out_tsv.relative_to(HERE)}: {len(images)}/{len(recipes)} frames")


def decode_mono_pcm(input_path: Path, rate: int,
                    start: int | None = None, end: int | None = None
                    ) -> np.ndarray:
    """Decode one source clip to mono signed 16-bit PCM at `rate`.

    Music is trimmed before mastering so every segment is normalised against
    its own loudest sample, not against the unedited source MP3.
    """
    filters = f"aresample={rate}"
    if start is not None and end is not None:
        filters += (f",atrim=start_sample={start}:end_sample={end},"
                    "asetpts=PTS-STARTPTS")
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-i", str(input_path), "-vn",
         "-af", filters, "-ac", "1", "-ar", str(rate),
         "-f", "s16le", "pipe:1"],
        check=True, capture_output=True)
    return np.frombuffer(result.stdout, dtype="<i2").copy()


def master_samples(samples: np.ndarray, gain_db: float) -> np.ndarray:
    """Peak-normalise to 0 dBFS (with headroom), then apply the asset's trim."""
    if samples.size == 0:
        return samples
    as_float = samples.astype(np.float64)
    peak = float(np.max(np.abs(as_float))) / 32768.0
    if peak <= 0.0:
        return samples
    target = 10.0 ** (effective_peak_db(gain_db) / 20.0)
    scaled = np.rint(as_float * (target / peak))
    return np.clip(scaled, -32768, 32767).astype("<i2")


def write_mastered_wav(input_path: Path, output_path: Path, gain_db: float,
                       rate: int = 16000) -> bool:
    samples = master_samples(decode_mono_pcm(input_path, rate), gain_db)
    if samples.size == 0:
        return False
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output_path), "wb") as wave_file:
        wave_file.setnchannels(1)
        wave_file.setsampwidth(2)
        wave_file.setframerate(rate)
        wave_file.writeframes(samples.tobytes())
    return output_path.stat().st_size > 44


def apply_db(samples: np.ndarray, delta_db: float) -> np.ndarray:
    """Trim already-mastered PCM without renormalising it."""
    return np.clip(
        np.rint(samples.astype(np.float64) * (10.0 ** (delta_db / 20.0))),
        -32768, 32767).astype("<i2")


def decoded_peak(path: Path) -> int:
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-f", "s16le", "pipe:1"],
        check=True, capture_output=True)
    samples = np.frombuffer(result.stdout, dtype="<i2")
    return int(np.max(np.abs(samples.astype(np.int32)))) if samples.size else 0


def filter_mono_pcm(samples: np.ndarray, filters: str) -> np.ndarray:
    """Run a mono s16 PCM buffer through one ffmpeg filter chain."""
    if samples.size == 0:
        return samples
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", "44100",
         "-ac", "1", "-i", "pipe:0", "-af", filters,
         "-ar", "44100", "-ac", "1", "-f", "s16le", "pipe:1"],
        input=samples.tobytes(), check=True, capture_output=True)
    return np.frombuffer(result.stdout, dtype="<i2").copy()


def float_pcm(samples: np.ndarray) -> np.ndarray:
    """Convert s16 PCM to float for gain staging without intermediate clipping."""
    return samples.astype(np.float64) / 32768.0


def s16_pcm(samples: np.ndarray) -> np.ndarray:
    """Quantise float PCM back to s16 once, at the very end of the chain."""
    return np.clip(np.rint(samples * 32768.0), -32768, 32767).astype("<i2")


def filter_mono_pcm_float(samples: np.ndarray, filters: str) -> np.ndarray:
    """Run float mono PCM through an ffmpeg filter chain, preserving length."""
    if samples.size == 0:
        return samples
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", "44100",
         "-ac", "1", "-i", "pipe:0", "-af", filters,
         "-ar", "44100", "-ac", "1", "-f", "f32le", "pipe:1"],
        input=np.asarray(samples, dtype="<f4").tobytes(),
        check=True, capture_output=True)
    out = np.frombuffer(result.stdout, dtype="<f4").astype(np.float64)
    if out.size != samples.size:
        raise RuntimeError(
            f"effect changed PCM length {samples.size} -> {out.size}: {filters}")
    return out


def peak_db_value(peak: float) -> float:
    """Convert an already-measured sample peak to dBFS."""
    return 20.0 * np.log10(peak) if peak > 0.0 else -90.0


def peak_db(samples: np.ndarray) -> float:
    peak = float(np.max(np.abs(samples))) if samples.size else 0.0
    return peak_db_value(peak)


def apply_effect_chain(samples: np.ndarray, chain: list[dict],
                       gain_db: float = 0.0) -> np.ndarray:
    """Apply an ordered list of modular effects to one float PCM buffer.

    Supported components: `peak_gain`, `gain`, `fader`, `limiter`, `eq`, and
    `compressor`. Add more kinds here; every bus just supplies its own list.
    """
    out = np.asarray(samples, dtype=np.float64)
    for effect in chain:
        kind = effect["type"]
        if kind in ("gain", "fader", "peak_gain"):
            if kind == "peak_gain":
                db = float(effect["target_db"]) - peak_db(out)
            else:
                db = float(gain_db if kind == "fader" else effect["db"])
                if kind == "fader":
                    db = min(float(effect.get("clamp_positive_db", 0.0)), db)
            out = out * (10.0 ** (db / 20.0))
        elif kind == "limiter":
            normalize = "true" if effect.get("normalize", False) else "false"
            filters = (f"alimiter=limit={effect['limit']}:"
                       f"attack={effect['attack']}:"
                       f"release={effect['release']}:level={normalize}")
            out = filter_mono_pcm_float(out, filters)
        elif kind == "eq":
            filters = []
            if effect.get("highpass"):
                filters.append(f"highpass=f={effect['highpass']}:p={effect['order']}")
            if effect.get("lowpass"):
                filters.append(f"lowpass=f={effect['lowpass']}:p={effect['order']}")
            if filters:
                out = filter_mono_pcm_float(out, ",".join(filters))
        elif kind == "compressor":
            filters = (f"acompressor=threshold={effect['threshold']}:"
                       f"knee={effect['knee']}:ratio={effect['ratio']}:"
                       f"attack={effect['attack']}:release={effect['release']}")
            out = filter_mono_pcm_float(out, filters)
        else:
            raise RuntimeError(f"unknown audio effect type: {kind}")
    return out


def master_bgm_group(names: list[str], raw_segments: list[np.ndarray],
                     gains: dict[str, float]) -> tuple[list[tuple[str, np.ndarray]], float]:
    """Mix one logical track as a single song, then split it into segments."""
    lengths = [segment.size for segment in raw_segments]
    song = np.concatenate([float_pcm(segment) for segment in raw_segments])
    pre_gain_db = 0.0
    first = BGM_GROUP_CHAIN[0] if BGM_GROUP_CHAIN else {}
    if first.get("type") == "peak_gain":
        pre_gain_db = float(first["target_db"]) - peak_db(song)
    mixed = apply_effect_chain(song, BGM_GROUP_CHAIN)

    result: list[tuple[str, np.ndarray]] = []
    offset = 0
    for name, length in zip(names, lengths):
        segment = mixed[offset:offset + length]
        segment = apply_effect_chain(
            segment, BGM_SEGMENT_CHAIN, gains.get(f"{name}.ogg", 0.0))
        result.append((name, segment))
        offset += length
    return result, pre_gain_db


def _encode_vorbis(mono: np.ndarray, output_path: Path, correction: float) -> bool:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", "44100",
           "-ac", "2", "-i", "pipe:0", "-c:a", "vorbis", "-strict", "-2",
           "-q:a", "4", str(output_path)]
    stereo = np.repeat(s16_pcm(mono * (10.0 ** (
        (BGM_ENCODE_SAFETY_DB + correction) / 20.0))), 2)
    subprocess.run(cmd, input=stereo.tobytes(), check=True, capture_output=True)
    return output_path.exists() and output_path.stat().st_size > 100


def decoded_peak_db(path: Path) -> float:
    return peak_db_value(decoded_peak(path) / 32768.0)


def calibrate_encode_correction(reference: np.ndarray) -> float:
    """Find one shared encode safety correction from the loudest segment.

    Every segment in the OST uses the same correction, so the group pre-gain and
    the OST-wide LUFS match are preserved. Only Vorbis reconstruction overshoot
    is compensated here.
    """
    correction = 0.0
    if reference.size == 0:
        return correction
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "reference.ogg"
        for _ in range(12):
            if not _encode_vorbis(reference, path, correction):
                return correction
            # Aim slightly under the ceiling: one segment can decode a few
            # hundredths of a dB hotter than the measured reference.
            delta = (MASTER_PEAK_DB - BGM_ENCODE_HEADROOM_DB) - decoded_peak_db(path)
            if abs(delta) <= BGM_ENCODE_TOLERANCE_DB:
                break
            correction += max(-BGM_MAX_ENCODE_STEP_DB,
                              min(BGM_MAX_ENCODE_STEP_DB, delta))
    return correction


def encode_mastered_ogg(mono: np.ndarray, output_path: Path,
                        correction: float = 0.0) -> bool:
    """Encode one fully mastered segment with the group's shared correction."""
    if mono.size == 0:
        return False
    if not _encode_vorbis(mono, output_path, correction):
        return False
    peak = decoded_peak_db(output_path)
    if peak > MASTER_PEAK_DB + BGM_ENCODE_TOLERANCE_DB:
        print(f"music encode ceiling exceeded: {output_path.name} {peak:+.2f} dB")
        return False
    return True


def integrated_lufs(samples: np.ndarray, channels: int = 1) -> float:
    """EBU R128 integrated loudness of a whole mixed BGM group.

    Mono sources are measured as the duplicated stereo signal actually encoded
    into OGG; decoded OGGs are already stereo and measured as-is.
    """
    if samples.size == 0:
        return -90.0
    data = np.asarray(samples, dtype="<f4")
    if channels == 1:
        data = np.repeat(data, 2)
    result = subprocess.run(
        ["ffmpeg", "-v", "info", "-f", "f32le", "-ar", "44100", "-ac", "2",
         "-i", "pipe:0", "-af", "ebur128=peak=true", "-f", "null", "-"],
        input=data.tobytes(), capture_output=True)
    matches = re.findall(r"I:\s*(-?\d+(?:\.\d+)?)\s*LUFS",
                         result.stderr.decode("utf-8", "replace"))
    return float(matches[-1]) if matches else -90.0


def match_bgm_group_loudness(groups: list[dict]) -> None:
    """Second pass: make every logical BGM group match across the OST.

    The first pass masters each group as one song. This pass measures the whole
    group's integrated loudness (not individual segments, and not just a peak)
    and applies one gain per group. Per-group offsets live in audio_gain.py.
    """
    rows = []
    for group in groups:
        segments = group["pcm"]
        if not segments:
            continue
        measured = integrated_lufs(np.concatenate(segments))
        target = group_target_lufs(group["name"])
        delta = max(-12.0, min(12.0, target - measured))
        gain = 10.0 ** (delta / 20.0)
        group["pcm"] = [segment * gain for segment in segments]
        rows.append(f"{group['name']} {measured:+.1f}->{target:+.1f}LUFS")
    print("audio: OST gain match: " + ", ".join(rows))


def verify_ost_loudness(music_out: Path, groups: list[dict]) -> None:
    """Re-check the encoded OGGs: every group must land at the same LUFS."""
    levels = []
    for group in groups:
        chunks = []
        for name in group["names"]:
            result = subprocess.run(
                ["ffmpeg", "-v", "error", "-i", str(music_out / f"{name}.ogg"),
                 "-f", "f32le", "-ar", "44100", "-ac", "2", "pipe:1"],
                capture_output=True, check=True)
            chunks.append(np.frombuffer(result.stdout, dtype="<f4"))
        levels.append(integrated_lufs(np.concatenate(chunks), channels=2))
    if not levels:
        return
    spread = max(levels) - min(levels)
    print(f"audio: OST decoded LUFS {min(levels):.2f}..{max(levels):.2f} "
          f"(target {BGM_TARGET_LUFS:.1f}, spread {spread:.2f} dB)")
    if spread > 0.5:
        raise RuntimeError(f"BGM groups are not gain-matched: {spread:.2f} dB spread")


def write_silent_wav(path: Path, duration: float = 0.10, rate: int = 16000) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wave_file:
        wave_file.setnchannels(1)
        wave_file.setsampwidth(2)
        wave_file.setframerate(rate)
        wave_file.writeframes(b"\0\0" * int(rate * duration))


def prepare_audio(source: Source) -> None:
    sfx_out = ROMFS / "audio/sound-effects"
    music_out = ROMFS / "audio/music"
    sfx_out.mkdir(parents=True, exist_ok=True)
    music_out.mkdir(parents=True, exist_ok=True)

    gains = load_audio_gains()

    sfx_ok = 0
    for output, key in SFX_FILES.items():
        asset = f"{output}.wav"
        path = source.sounds.get(audio_key(key))
        if path and write_mastered_wav(path, sfx_out / asset,
                                       gains.get(asset, 0.0)):
            sfx_ok += 1
        else:
            print(f"sound not found in source: {key}")
    for output in SILENT_SFX:
        write_silent_wav(sfx_out / f"{output}.wav")

    timings = load_music_timings()
    grouped = [name for _, names in MUSIC_GROUPS for name in names]
    if sorted(grouped) != sorted(MUSIC_FILES):
        raise RuntimeError("MUSIC_GROUPS must cover MUSIC_FILES exactly once")

    groups = []
    pre_gains = []
    for group_name, names in MUSIC_GROUPS:
        group_names = []
        raw_segments = []
        for name in names:
            asset = f"{name}.ogg"
            path = source.sounds.get(audio_key(name))
            if not path:
                print(f"music not found in source: {name}")
                continue
            timing = timings.get(asset)
            if not timing:
                print(f"music timing missing for {name}")
                continue
            mono = decode_mono_pcm(path, 44100,
                                   int(timing["start"]), int(timing["end"]))
            if not mono.size:
                print(f"music decode empty: {name}")
                continue
            group_names.append(name)
            raw_segments.append(mono)
        if not raw_segments:
            continue
        mastered, pre_gain_db = master_bgm_group(group_names, raw_segments, gains)
        pre_gains.append(f"{group_name} {pre_gain_db:+.2f}dB")
        groups.append({
            "name": group_name,
            "names": group_names,
            "pcm": [pcm for _, pcm in mastered],
        })

    # Second pass, after every group has been mixed: gain-match the whole OST.
    match_bgm_group_loudness(groups)

    # One encode correction for the whole OST, measured from the single loudest
    # segment after the loudness match. Calibrating each group separately would
    # push every group back to the same peak and undo the LUFS matching.
    all_pcm = [segment for group in groups for segment in group["pcm"]]
    reference = max(all_pcm, key=lambda s: float(np.max(np.abs(s)))
                    if s.size else 0.0)
    correction = calibrate_encode_correction(reference)

    music_ok = 0
    for group in groups:
        for name, segment in zip(group["names"], group["pcm"]):
            if encode_mastered_ogg(segment, music_out / f"{name}.ogg", correction):
                music_ok += 1

    verify_ost_loudness(music_out, groups)

    print(f"audio: BGM group pre-gains: {', '.join(pre_gains)}")
    print("audio: BGM chain: shared peak gain -> peak limiter -> "
          "20Hz-20kHz EQ -> make-up limiter -> per-asset BGM fader; "
          f"SFX bus 100%, music bus 80%")
    print(f"audio: {sfx_ok}/{len(SFX_FILES)} SFX, {music_ok}/{len(MUSIC_FILES)} music, "
          f"{len(SILENT_SFX)} silent placeholders")


def emit_offsets() -> None:
    data = json.loads((DATA / "junkbot_offsets.json").read_text())
    lines = [
        "/* Generated by tools/prepare_assets.py -- do not edit. */",
        "#ifndef JUNKBOT_OFFSETS_H", "#define JUNKBOT_OFFSETS_H", "",
        "typedef struct { const char *sprite; int x, y; } JbOffset;", "",
        "static const JbOffset jb_offsets[] = {",
    ]
    count = 0
    for frames in data.values():
        for frame in frames:
            if "sprite" not in frame or "offset" not in frame:
                continue
            offset = frame["offset"]
            lines.append(f'    {{ "{frame["sprite"]}", {offset["x"]}, {offset["y"]} }},')
            count += 1
    lines += ["};", "", "#endif", ""]
    (HERE / "include/jb_offsets.h").write_text("\n".join(lines), encoding="utf-8")
    print(f"include/jb_offsets.h: {count} offsets")


def prepare_levels(source: Source) -> None:
    levels_out = ROMFS / "levels"
    levels_out.mkdir(parents=True, exist_ok=True)
    listing = source.level_order or [
        line.strip() for line in (DATA / "level_listing.txt").read_text().splitlines()
        if line.strip()]
    used = 0
    for title in listing:
        path = (source.levels.get(title_key(title)) or source.levels.get(title) or
                source.levels.get(title_lookup_key(title)))
        if not path:
            print(f"level not found in source: {title}")
            continue
        shutil.copy2(path, levels_out / f"{title}.txt")
        used += 1
    (levels_out / "_LEVEL_LISTING.txt").write_text(
        "\n".join(listing) + "\n", encoding="utf-8")
    shutil.copy2(DATA / "Title Screen.txt", levels_out / "Title Screen.txt")
    print(f"levels: {used}/{len(listing)} original levels")


def _ink_mask(text: str, font: ImageFont.FreeTypeFont) -> np.ndarray:
    """Render one text run with the original UI font at 2x pixel scale.

    The plaques in the original movie use 04B_08 at 8px, rendered at 1x and
    then scaled 2x with nearest-neighbour (2px-thick strokes). Return an RGBA
    image cropped tightly to the ink so the caller can place each word exactly.
    """
    scratch = Image.new("L", (256, 8), 0)
    ImageDraw.Draw(scratch).text((0, -1), text, font=font, fill=255)
    big = np.array(scratch.resize((scratch.width * 2, scratch.height * 2),
                                  Image.Resampling.NEAREST))
    ys, xs = np.where(big > 0)
    if len(xs) == 0:
        return np.zeros((0, 0), dtype=np.uint8)
    return big[ys.min():ys.max() + 1, xs.min():xs.max() + 1].astype(np.uint8)


def render_loading_plaque(path: Path, width: int, height: int,
                          words: list[tuple[str, int, int]]) -> None:
    """Draw a loading plaque from bundled-font text, matching the original 1:1.

    ``words`` is a list of ``(text, x, y)`` placements derived from the original
    cast layout (2px strokes, 10px line height, second line at y=15). Colours
    and glyph layout are reproduced exactly; no Janitorial pixels are used.
    """
    font = ImageFont.truetype(str(HERE / "assets/04B_08__.TTF"), 8)
    canvas = np.zeros((height, width), dtype=np.uint8)
    for text, x, y in words:
        mask = _ink_mask(text, font)
        h, w = mask.shape
        canvas[y:y + h, x:x + w] = np.maximum(canvas[y:y + h, x:x + w], mask)
    rgba = np.zeros((height, width, 4), dtype=np.uint8)
    rgba[:, :, :3] = (99, 99, 99)
    rgba[:, :, 3] = canvas
    Image.fromarray(rgba, "RGBA").save(path)


def prepare_menus(source: Source) -> None:
    menus = ROMFS / "menus"
    menus.mkdir(parents=True, exist_ok=True)
    wanted = {
        "neo_title": "neo_title", "credit_up": "credit_up",
        "credit_up_x": "credit_up_x",
        "replay_intro": "replay_intro", "replay_intro_x": "replay_intro_x",
        "skip_intro": "skip_intro", "skip_intro_x": "skip_intro_x",
    }
    for output, key in wanted.items():
        path = frame_to_original(key, source.bitmaps, source.bitmaps_full)
        if not path:
            continue
        with Image.open(path) as image:
            cleaned = strip_isolated_markers(key_white(image.convert("RGBA")))
            cleaned.save(menus / f"{output}.png")

    # Loading plaques: the original cast has no bitmap members for these, so
    # rebuild them from the bundled UI font. Word placements mirror the
    # original layout exactly (verified byte-for-byte against the reference).
    render_loading_plaque(
        menus / "download_in_progress.png", 124, 25,
        [("DOWNLOAD", 0, 0), ("IN", 106, 0), ("PROGRESS", 15, 15)])
    render_loading_plaque(
        menus / "ready_to_play.png", 92, 25,
        [("READY", 0, 0), ("TO", 70, 0), ("PLAY", 23, 15)])


def prepare_intro() -> None:
    """Render the original intro SWF into a flipbook atlas.

    Prefers Ruffle's exporter (accurate timeline, gradients and colour
    transforms) rendering native 200x200 frames with no resampling. Falls back
    to a checked-in pre-render when the exporter is unavailable, and only to the
    old swfrender simulator if that pre-render is missing. Duplicate frames are
    collapsed and a 480-entry sequence restores the original 15 fps timing.
    """
    out = ROMFS / "intro"
    code_dir = Path(os.environ["JUNKBOT_CODE"]).resolve() if os.environ.get(
        "JUNKBOT_CODE") else HERE.parent / "junkbot-code"
    swf = code_dir / "files/loading/intro_anim.swf"
    if not swf.exists():
        print("intro: no intro_anim.swf in original source (flipbook skipped)")
        return

    # Preferred pipeline: Ruffle actually emulates the Flash timeline and
    # rasterises vectors/gradients/colour transforms properly. The old
    # swfrender + display-list simulator produced solid wrong-colour regions.
    ruffle = os.environ.get("RUFFLE_EXPORTER") or shutil.which("ruffle-exporter")
    if ruffle and Path(ruffle).exists():
        result = subprocess.run(
            ["python3", str(TOOLS / "ruffle_intro.py"), str(swf), str(out), ruffle],
            capture_output=True, text=True)
        if result.returncode == 0:
            print(result.stdout.strip())
            return
        print(f"intro: Ruffle export failed, falling back: {result.stderr[-200:]}")
    elif (HERE / "assets/intro/ruffle.txt").exists():
        # Never regenerate the low-quality swfrender atlas just because the
        # Ruffle exporter isn't installed: use the checked-in pre-render.
        stored = HERE / "assets/intro"
        out.mkdir(parents=True, exist_ok=True)
        for name in ("atlas.png", "atlas.tsv", "seq.bin", "ruffle.txt"):
            shutil.copy2(stored / name, out / name)
        seq_count = len((out / "seq.bin").read_bytes()) // 2
        lines = (out / "atlas.tsv").read_text().splitlines()
        print(f"intro: {len(lines)} unique flipbook frames, {seq_count} steps (pre-rendered Ruffle)")
        return

    if shutil.which("swfrender") is None:
        print("intro: swfrender not installed (flipbook skipped)")
        return

    with tempfile.TemporaryDirectory(prefix="jb_intro_") as tmp:
        frames_dir = Path(tmp) / "frames"
        result = subprocess.run(
            ["python3", str(TOOLS / "swf_frames.py"), str(swf), str(frames_dir),
             "--start", "1", "--end", "480", "--step", "1"],
            capture_output=True, text=True)
        frames = sorted(frames_dir.glob("*.png")) if frames_dir.exists() else []
        if result.returncode != 0 or len(frames) != 480:
            print(f"intro: frame render failed ({len(frames)}/480): "
                  f"{result.stderr[-200:]}")
            return

        unique: list[bytes] = []
        lookup: dict[bytes, int] = {}
        sequence = bytearray()
        for path in frames:
            raw = path.read_bytes()
            digest = hashlib.sha256(raw).digest()
            if digest not in lookup:
                lookup[digest] = len(unique)
                unique.append(raw)
            sequence += struct.pack("<H", lookup[digest])

        cols = 16
        pad = 2
        stride = 200 + pad * 2
        rows = (len(unique) + cols - 1) // cols
        atlas = Image.new("RGBA", (cols * stride, rows * stride), (0, 0, 0, 0))
        lines = []
        for i, raw in enumerate(unique):
            with Image.open(BytesIO(raw)) as source_image:
                image = source_image.convert("RGBA")
            cell_x = (i % cols) * stride
            cell_y = (i // cols) * stride
            x, y = cell_x + pad, cell_y + pad
            # Replicate the outer pixels into the gutter, including corners.
            # Stretching an edge row across the full cell stride slightly
            # resamples it and made the top/bottom cel edges shimmer; each edge
            # must be copied at its exact 200px length instead.
            expanded = Image.new("RGBA", (stride, stride), (0, 0, 0, 0))
            expanded.paste(image, (pad, pad))
            expanded.paste(image.crop((0, 0, 1, 200)).resize((pad, 200), Image.Resampling.NEAREST),
                           (0, pad))
            expanded.paste(image.crop((199, 0, 200, 200)).resize((pad, 200), Image.Resampling.NEAREST),
                           (200 + pad, pad))
            expanded.paste(image.crop((0, 0, 200, 1)).resize((200, pad), Image.Resampling.NEAREST),
                           (pad, 0))
            expanded.paste(image.crop((0, 199, 200, 200)).resize((200, pad), Image.Resampling.NEAREST),
                           (pad, 200 + pad))
            for sx, sy, dx, dy in ((0, 0, 0, 0),
                                   (199, 0, 200 + pad, 0),
                                   (0, 199, 0, 200 + pad),
                                   (199, 199, 200 + pad, 200 + pad)):
                expanded.paste(image.getpixel((sx, sy)),
                               (dx, dy, dx + pad, dy + pad))
            atlas.paste(expanded, (cell_x, cell_y))
            lines.append(f"intro_{i:04d}\t{x}\t{y}\t200\t200\n")

    out.mkdir(parents=True, exist_ok=True)
    atlas.save(out / "atlas.png")
    (out / "atlas.tsv").write_text("".join(lines))
    (out / "seq.bin").write_bytes(sequence)
    print(f"intro: {len(unique)} unique flipbook frames, 480 steps")


def main() -> None:
    source = Source.detect()
    print(f"asset source: {source.name}")
    if ROMFS.exists():
        shutil.rmtree(ROMFS)
    ROMFS.mkdir(parents=True)

    recipes = json.loads((DATA / "sprite_recipes.json").read_text())
    sprite_sheets = ROMFS / "sprites"
    undercover = sprite_sheets / "undercover"
    sprite_sheets.mkdir(parents=True, exist_ok=True)
    undercover.mkdir(parents=True, exist_ok=True)
    pack_recipe_atlas("sprites", recipes["sprites.json"], source,
                      sprite_sheets / "sprites.png", sprite_sheets / "sprites.tsv")
    pack_recipe_atlas("backgrounds", recipes["backgrounds.json"], source,
                      sprite_sheets / "backgrounds.png", sprite_sheets / "backgrounds.tsv")
    pack_recipe_atlas("undercover sprites", recipes["Undercover Exclusive/sprites.json"],
                      source, undercover / "sprites.png", undercover / "sprites.tsv")
    pack_recipe_atlas("undercover backgrounds",
                      recipes["Undercover Exclusive/backgrounds.json"], source,
                      undercover / "backgrounds.png", undercover / "backgrounds.tsv")

    fonts = ROMFS / "fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / "assets/04B_08__.TTF", fonts / "04B_08__.TTF")
    prepare_menus(source)
    prepare_intro()
    prepare_levels(source)
    prepare_audio(source)
    emit_offsets()
    print(f"assets ready in {ROMFS.relative_to(HERE)}")


if __name__ == "__main__":
    main()
