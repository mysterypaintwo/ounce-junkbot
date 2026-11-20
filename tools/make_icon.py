#!/usr/bin/env python3
"""Build the 256x256 homebrew icon from the original Junkbot cast art.

Everything drawn here comes from approved original sources (the extracted
``junkbot.dir`` cast, or the ``junkbot-code`` BMP export).  No Janitorial or
other derived artwork is used.  The recipe is deterministic so the icon can be
reproduced byte-for-byte from a clean checkout:

  * metallic background colour and corner-bolt tile come from
    ``loading bkg frame`` / ``neo_title``;
  * the gold "JUNKBOT" wordmark is cropped out of ``neo_title`` and split
    between the K and B so JUNK and BOT sit on two centred lines;
  * the original cast shadow is removed and replaced with a clean offset
    shadow generated from the letter silhouettes; the metallic background is
    keyed out.

Run with ``python3 tools/make_icon.py`` (optionally ``JUNKBOT_DIR`` /
``JUNKBOT_CODE`` to pick the source tree).
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from prepare_assets import Source, frame_to_original  # noqa: E402

SIZE = 256
METAL = (160, 160, 164, 255)
OLD_SHADOW = (51, 51, 51, 255)  # cast shadow baked into neo_title
NEW_SHADOW = (28, 28, 32, 255)  # clean replacement shadow
SHADOW_OFFSET = 3
LOGO_BOX = (32, 28, 448, 132)   # gold wordmark inside neo_title
SPLIT_X = 268                   # 4px gap between the K and the B
BOLT_BOX = (6, 3, 15, 12)       # one bolt + 1px margin, in loading bkg frame


def _source() -> Source:
    return Source.detect()


def _bitmap(source: Source, name: str) -> Image.Image:
    path = frame_to_original(name, source.bitmaps, source.bitmaps_full)
    if path is None:
        raise SystemExit(f"original bitmap not found: {name}")
    with Image.open(path) as image:
        return image.convert("RGBA")


def _wordmark(title: Image.Image, box: tuple[int, int, int, int]) -> Image.Image:
    """Crop one half of the wordmark, dropping the flat cast colours."""
    piece = title.crop(box)
    data = piece.load()
    for y in range(piece.height):
        for x in range(piece.width):
            if data[x, y][:3] in (METAL[:3], OLD_SHADOW[:3]):
                data[x, y] = (255, 255, 255, 0)
    return piece


def _shadow(piece: Image.Image) -> Image.Image:
    """Build a crisp offset shadow from the wordmark's letter silhouette."""
    shifted = Image.new("L", piece.size, 0)
    shifted.paste(piece.getchannel("A"),
                  (SHADOW_OFFSET, SHADOW_OFFSET))
    shadow = Image.new("RGBA", piece.size, (0, 0, 0, 0))
    shadow.paste(NEW_SHADOW, (0, 0), shifted)
    return shadow


def _bolts(frame: Image.Image) -> Image.Image:
    bolt = frame.crop(BOLT_BOX)
    if bolt.size != (9, 9):
        raise SystemExit(f"unexpected bolt tile size: {bolt.size}")
    return bolt


def build() -> Image.Image:
    source = _source()
    title = _bitmap(source, "neo_title")
    frame = _bitmap(source, "loading_bkg_frame")

    icon = Image.new("RGBA", (SIZE, SIZE), METAL)

    # Put twice as many bolts all the way around the plaque.  The tile is
    # lifted from the original frame, so the highlight/shading pixels stay
    # faithful to the game.
    bolt = _bolts(frame)
    positions = []
    for x in (18, 82, SIZE // 2 - 4, 164, SIZE - 27):
        positions += [(x, 18), (x, SIZE - 27)]
    for y in (78, SIZE // 2 - 4, 168):
        positions += [(18, y), (SIZE - 27, y)]
    for x, y in positions:
        icon.alpha_composite(bolt, (x, y))

    # Split the single-line wordmark between K and B.  Both halves keep the
    # same scale so the letters stay the same size on their two lines.
    left = _wordmark(title, (LOGO_BOX[0], LOGO_BOX[1], SPLIT_X, LOGO_BOX[3]))
    right = _wordmark(title, (SPLIT_X, LOGO_BOX[1], LOGO_BOX[2], LOGO_BOX[3]))
    scale = 200 / left.width
    new_size = (round(left.width * scale), round(left.height * scale))
    left = left.resize(new_size, Image.Resampling.NEAREST)
    right = right.resize((round(right.width * scale), new_size[1]),
                         Image.Resampling.NEAREST)

    gap = 12
    total_h = new_size[1] * 2 + gap
    top = (SIZE - total_h) // 2
    left_x = (SIZE - left.width) // 2
    right_x = (SIZE - right.width) // 2
    right_y = top + new_size[1] + gap
    # Shadow first, then the gold letters, for each of the two lines.
    icon.alpha_composite(_shadow(left), (left_x, top))
    icon.alpha_composite(left, (left_x, top))
    icon.alpha_composite(_shadow(right), (right_x, right_y))
    icon.alpha_composite(right, (right_x, right_y))

    return icon.convert("RGB")


def main() -> None:
    icon = build()
    destination = ROOT / "icon.png"
    icon.save(destination)
    print(f"wrote {destination.relative_to(ROOT)} ({icon.size[0]}x{icon.size[1]})")


if __name__ == "__main__":
    main()
