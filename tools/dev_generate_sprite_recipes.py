#!/usr/bin/env python3
"""Development-only: regenerate tools/data/sprite_recipes.json from originals.

This tool is never used by the normal build (``prepare_assets.py`` only reads
the checked-in recipe JSON).  It derives every frame from approved original
sources:

  * the ``.asset-cache/dir`` extraction of ``junkbot.dir`` -- with CASt
    registration points from the cache's ``bitmap_points.json`` -- or, when a
    Director movie is unavailable, the exported ``../junkbot-code`` BMPs;
  * ``tools/data/sprite_frames.json`` for the list of frame names the renderer
    expects.

The janitorial-android individual PNGs are used ONLY as a development-time
size/crop oracle: ``Image.open(path).size`` tells us the intended frame size,
and a difference becomes an ``outputCrop`` in the recipe.  No janitorial pixel
data is ever written to the recipe file (nor used to choose a source bitmap).
The generated JSON contains only original cast names, points names and crops.

If ``.asset-cache/dir`` is missing or older than the Director movie, this tool
runs ``tools/dir_extract.py`` first.  Override the movie with ``JUNKBOT_DIR``
and the exported BMP tree with ``JUNKBOT_CODE``; ``JANITORIAL_IMAGES`` can
point at a different oracle tree.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
DATA = TOOLS / "data"
CACHE = ROOT / ".asset-cache" / "dir"
FRAMES = json.loads((DATA / "sprite_frames.json").read_text())

sys.path.insert(0, str(TOOLS))
import prepare_assets as pa  # noqa: E402  (authoritative recipe consumer)

DEFAULT_DIR = Path.home() / "Downloads/Junkbot-DIRs/junkbot.dir"
DEFAULT_CODE = ROOT.parent / "junkbot-code"
DEFAULT_JAN = ROOT.parent / "janitorial-android" / "images"
CACHE_FILES = ("manifest.tsv", "bitmap_points.json", "level_listing.txt")

# Renderer animation name -> original cast prefix.  These spellings come from
# the original CASt member names; lookup is case-insensitive via norm_full().
MINIFIG_FAMILY = {
    "walk_r": "MINIFIG_WALK_R",
    "walk_l": "MINIFIG_WALK_L",
    "shield_walk_r": "MINIFIG_SHIELDWALK_R",
    "shield_walk_l": "MINIFIG_SHIELDWALK_L",
    "shield_on_r": "MINIFIG_SHIELDON_R",
    "shield_on_l": "MINIFIG_SHIELDON_L",
    "eat_start": "MINIFIG_EAT_R",
    "shield_eat": "MINIFIG_SHIELDEAT_R",
    "die": "MINIFIG_DEAD_GENERIC",
    "water_die": "MINIFIG_DEAD_DRIP",
}
# One original layer (walk_r frame 1's s2) has an unusable registration point
# in the cast and reuses frame 6's layer. The left 9/10 layers exist and must
# use their own art: aliasing them to frame 6 visibly skewes Junkbot's walk.
MINIFIG_S2_ALIAS = {
    ("walk_r", "1"): "MINIFIG_WALK_R_6_s2",
}
CLIMB_FAMILY = {
    "u": "HAZ_CLIMBER_FLOAT_UP",
    "d": "HAZ_CLIMBER_FLOAT_DOWN",
    "r": "HAZ_CLIMBER_WALK_R",
    "l": "HAZ_CLIMBER_walk_l",
}
GEAR_FAMILY = {
    "l": ("haz_walker_walk_l", "haz_walker_walk_l"),
    "r": ("haz_walker_WALK_R", "haz_walker_walk_R"),
}
FLY_PREFIX = "HAZ_DUMBFLOAT_r"
EYE_PREFIX = {
    "eyebot": "HAZ_FLOAT_inactive",
    "eyebot_active": "HAZ_FLOAT_Active",
    "eyebot_active_1fix": "HAZ_FLOAT_Active",
}

# A handful of janitorial frame names were assigned to a neighbouring
# original cast member during development.  These are original-to-original
# aliases; they are kept so the generated recipe stays bit-for-bit equivalent.
SINGLE_ALIASES = {
    "brick_slickjump_active_1": "brick_slickJump_active_3",
    "brick_slickjump_dormant_1": "brick_slickJump_active_5",
    "haz_slickjump_active_3": "haz_slickJump_active_1",
    "haz_slickjump_active_5": "haz_slickJump_dormant_1",
}


def ensure_dir_extraction(dir_path: Path) -> None:
    """Run dir_extract.py when the cache is absent or older than the movie."""
    required = [CACHE / name for name in CACHE_FILES]
    stale = any(not path.exists() for path in required)
    if not stale and dir_path.exists():
        newest_cache = min(path.stat().st_mtime for path in required)
        stale = newest_cache < dir_path.stat().st_mtime
    if stale:
        subprocess.run(
            [sys.executable, str(TOOLS / "dir_extract.py"), str(dir_path), str(CACHE)],
            check=True)


def load_source() -> pa.Source:
    """Prefer the extracted Director cache; fall back to exported BMPs."""
    env_dir = os.environ.get("JUNKBOT_DIR")
    env_code = os.environ.get("JUNKBOT_CODE")
    dir_path = Path(env_dir).resolve() if env_dir else (
        DEFAULT_DIR if DEFAULT_DIR.exists() else None)
    code_path = Path(env_code).resolve() if env_code else (
        DEFAULT_CODE if DEFAULT_CODE.exists() else None)

    if env_code or (dir_path is None and code_path):
        if not code_path or not code_path.exists():
            raise SystemExit(f"Junkbot code source not found: {code_path}")
        points_path = CACHE / "bitmap_points.json"
        if not points_path.exists():
            raise SystemExit(
                f"Missing {points_path}: regenerate the Director cache with "
                "JUNKBOT_DIR before using the BMP source.")
        source = pa.Source.from_code(code_path)
        # Never fall back to the stale tools/data/bitmap_points.json.
        source.points = pa.load_points(points_path)
        return source

    if not dir_path or not dir_path.exists():
        raise SystemExit(
            "No original Junkbot source found. Set JUNKBOT_DIR to junkbot.dir "
            "or JUNKBOT_CODE to the junkbot-code tree.")
    ensure_dir_extraction(dir_path)
    source = pa.Source.from_dir(dir_path)
    points_path = CACHE / "bitmap_points.json"
    if not points_path.exists() or not source.points:
        raise SystemExit(f"No CASt registration points in {points_path}")
    source.points = pa.load_points(points_path)
    return source


def load_original_names(source: pa.Source) -> tuple[dict[str, str], dict[str, list[str]]]:
    """Map normalised cast keys back to original member names.

    The Director manifest preserves the cast spelling (and case); the BMP
    export uses the same spelling as the file stem.  Either way, only names are
    recorded -- bitmaps are always resolved through ``Source`` at build time.
    """
    full: dict[str, str] = {}
    short: dict[str, list[str]] = {}
    manifest = CACHE / "manifest.tsv"
    if source.name.startswith("junkbot.dir") and manifest.exists():
        with manifest.open(encoding="utf-8") as fp:
            next(fp, None)
            for line in fp:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 7 and parts[1] == "bitmap":
                    name = parts[2]
                    full.setdefault(pa.norm_full(name), name)
                    short.setdefault(pa.norm(name), []).append(name)
    else:
        for path in source.bitmaps_full.values():
            name = path.stem
            full.setdefault(pa.norm_full(name), name)
            short.setdefault(pa.norm(name), []).append(name)
    return full, short


class RecipeBuilder:
    def __init__(self, source: pa.Source) -> None:
        self.source = source
        self.full, self.short = load_original_names(source)
        self.jan = Path(os.environ.get("JANITORIAL_IMAGES", str(DEFAULT_JAN)))
        self.unresolved: list[str] = []

    def original(self, candidate: str, exact: bool = False) -> str | None:
        """Resolve a cast-name candidate that really exists in the source.

        ``exact`` matters for layered frames: ``norm()`` drops the ``_sN``
        suffix, so the short-name fallback would mistake a single-layer cast
        member for a missing layer.
        """
        key = pa.norm_full(candidate)
        name = self.full.get(key)
        if name is not None:
            # Keep the spelling from our rule (the cast may contain several
            # members differing only in case); lookup is normalised anyway.
            resolved = candidate
        elif not exact:
            names = self.short.get(pa.norm(candidate), [])
            resolved = names[0] if names else None
        else:
            resolved = None
        if resolved is None:
            return None
        if (pa.norm_full(resolved) not in self.source.bitmaps_full
                and pa.norm(resolved) not in self.source.bitmaps):
            return None
        return resolved

    def layers(self, candidates: list[str]) -> list[dict] | None:
        resolved = [self.original(candidate, exact=True) for candidate in candidates]
        if any(name is None for name in resolved):
            return None
        return [{"source": name} for name in resolved if name is not None]

    def single(self, name: str) -> list[dict] | None:
        key = pa.norm_full(name)
        candidates = [SINGLE_ALIASES.get(key, name)]

        brick = re.fullmatch(r"brick_(blue|red|green|yellow|white|immobile)_(\d+)",
                             name.lower())
        if brick:
            color = pa.COLOR_TO_ORIGINAL[brick.group(1)]
            studs = int(brick.group(2))
            candidates = [f"BRICK_{studs:02d}_{color}"]
            candidates += [f"BRICK_{width:02d}_{color}"
                           for width in (8, 6, 4, 3, 2, 1) if width != studs]
        for candidate in candidates:
            found = self.original(candidate)
            if found:
                return [{"source": found}]
        return None

    def minifig_layers(self, name: str) -> list[dict] | None:
        if name == "minifig_dead":
            return self.layers([f"MINIFIG_DEAD_STILL_1_s{i}" for i in range(1, 5)])
        match = re.fullmatch(r"minifig_(.+)_(\d+)", name)
        if not match:
            return None
        anim, frame = match.groups()
        prefix = MINIFIG_FAMILY.get(anim)
        if prefix is None:
            return None
        direction = "R"
        # The first five R frames are absent from the original cast; the L
        # frame 6 supplies the source layers (and EAT supplies points).
        if anim == "shield_eat" and self.original(
                f"MINIFIG_SHIELDEAT_R_{frame}_s1", exact=True) is None:
            prefix = "MINIFIG_SHIELDEAT_L"
            direction = "L"
            frame = "6"

        candidates: list[str] = []
        for suffix in range(1, 5):
            candidate = f"{prefix}_{frame}_s{suffix}"
            if suffix == 2:
                alias = MINIFIG_S2_ALIAS.get((anim, match.group(2)))
                if alias and self.original(alias, exact=True):
                    candidate = alias
            candidates.append(candidate)
        layers = self.layers(candidates)
        if layers is None:
            return None
        if anim == "shield_eat":
            for suffix, item in enumerate(layers, 1):
                item["pointsFrom"] = f"MINIFIG_EAT_{direction}_{frame}_s{suffix}"
        return layers

    def family_layers(self, name: str) -> list[dict] | None:
        match = re.fullmatch(r"climbbot_walk_([udrl])_(\d+)", name)
        if match:
            direction, frame = match.groups()
            prefix = CLIMB_FAMILY[direction]
            return self.layers([f"{prefix}_{frame}_s1", f"{prefix}_{frame}_s2"])

        match = re.fullmatch(r"gearbot_walk_([rl])_(\d+)", name)
        if match:
            direction, frame = match.groups()
            layered_prefix, single_prefix = GEAR_FAMILY[direction]
            layers = self.layers([f"{layered_prefix}_{frame}_s1",
                                  f"{layered_prefix}_{frame}_s2"])
            if layers is not None:
                return layers
            # Some animations only have a single-layer cast member (or the
            # layers have different casing); fall back to that original.
            return self.single(f"{single_prefix}_{frame}")

        match = re.fullmatch(r"flybot_(\d+)", name)
        if match:
            frame = match.group(1)
            return self.layers([f"{FLY_PREFIX}_{frame}_s1",
                                f"{FLY_PREFIX}_{frame}_s2"])

        match = re.fullmatch(r"(eyebot(?:_active)?)_(\d+)", name)
        if match:
            kind, frame = match.groups()
            prefix = EYE_PREFIX[kind]
            return self.layers([f"{prefix}_{frame}_s1", f"{prefix}_{frame}_s2"])

        if name in EYE_PREFIX:
            frame = "1" if name.endswith("_1fix") else None
            if frame:
                prefix = EYE_PREFIX[name]
                return self.layers([f"{prefix}_{frame}_s1", f"{prefix}_{frame}_s2"])
        return None

    def spec_for(self, name: str) -> list[dict] | None:
        if name == "bin":
            # The recycling can is three vertically registered flag slices,
            # not a single cast bitmap. Keep this special case when regenerating.
            return self.layers([
                "flag_blue_s1", "flag_blue_s2", "flag_blue_s3",
            ])
        if name == "word_danger":
            # Only this decal needs the enclosed white counter removed; the
            # digits must keep their interior/antialias white pixels.
            spec = self.single(name)
            if spec:
                spec[0]["keyWhite"] = True
            return spec
        if name.startswith("minifig_"):
            return self.minifig_layers(name)
        return self.family_layers(name) or self.single(name)

    def oracle_size(self, name: str, section: str) -> tuple[int, int] | None:
        """Read the janitorial frame's dimensions only -- never its pixels."""
        folder = "sprites" if section == "sprites.json" else "backgrounds"
        path = self.jan / folder / f"{name}.png"
        if not path.exists():
            return None
        with Image.open(path) as image:
            return image.size

    def recipe_for(self, name: str, section: str) -> dict | list[dict] | None:
        spec = self.spec_for(name)
        if spec is None:
            self.unresolved.append(f"{section}:{name}")
            return None
        composed = pa.compose_frame(spec, self.source)
        target = self.oracle_size(name, section)
        if composed is not None and target is not None and composed.size != target:
            # Size-only oracle: crop to the janitorial frame size (PIL pads
            # when the oracle is wider/taller).  No pixel inspection here.
            spec = {"layers": spec, "outputCrop": [0, 0, target[0], target[1]]}
        return spec


def main() -> None:
    source = load_source()
    print(f"asset source: {source.name}")
    builder = RecipeBuilder(source)

    recipes = {
        "sprites.json": {},
        "backgrounds.json": {},
        "Undercover Exclusive/sprites.json": {},
        "Undercover Exclusive/backgrounds.json": {},
    }
    for section in ("sprites.json", "backgrounds.json"):
        for name in FRAMES[section]:
            spec = builder.recipe_for(name, section)
            if spec is not None:
                recipes[section][name] = spec

    if builder.unresolved:
        print("no original source for:", ", ".join(builder.unresolved))
    out = DATA / "sprite_recipes.json"
    out.write_text(json.dumps(recipes, indent=1, sort_keys=True) + "\n",
                   encoding="utf-8")
    print("wrote", out.relative_to(ROOT),
          {key: len(value) for key, value in recipes.items()})


if __name__ == "__main__":
    main()
