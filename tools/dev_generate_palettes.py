#!/usr/bin/env python3
"""Development-only: derive Junkbot palettes from original exported BMPs."""
from __future__ import annotations

import ast
import hashlib
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tools/builtin_palettes.py"
CODE = ROOT.parent / "junkbot-code/files"

REPRESENTATIVES = {
    "MINIFIG_EAT_L_11_s2.bmp": "JUNKBOT_MAIN",
    "1.bmp": "JUNKBOT_UI",
    "level_lose.bmp": "JUNKBOT_LOSE",
}


def main() -> None:
    tree = ast.parse(OUT.read_text())
    base = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.List):
            base[node.targets[0].id] = ast.literal_eval(node.value)

    palettes: dict[str, tuple[Path, list[tuple[int, int, int]]]] = {}
    for path in CODE.rglob("*.bmp"):
        data = path.read_bytes()
        if len(data) < 54 or struct.unpack("<H", data[28:30])[0] != 8:
            continue
        count = struct.unpack("<I", data[46:50])[0] or 256
        raw = data[54:54 + count * 4]
        digest = hashlib.sha256(raw).hexdigest()[:12]
        palette = [(raw[i * 4 + 2], raw[i * 4 + 1], raw[i * 4])
                   for i in range(count)]
        palettes.setdefault(digest, (path, palette))

    derived = {}
    for _digest, (path, palette) in palettes.items():
        name = REPRESENTATIVES.get(path.name)
        if name:
            derived[name] = palette
    missing = set(REPRESENTATIVES.values()) - derived.keys()
    if missing:
        raise SystemExit(f"missing palettes: {sorted(missing)}")

    def fmt(palette: list[tuple[int, int, int]]) -> str:
        return "[" + ",".join(f"({r},{g},{b})" for r, g, b in palette) + "]"

    lines = ["# Generated from LibreShockwave tables and original junkbot-code BMP palettes."]
    for name in ("SYSTEM_MAC", "SYSTEM_WIN", "GRAYSCALE",
                 "JUNKBOT_MAIN", "JUNKBOT_UI", "JUNKBOT_LOSE"):
        palette = derived[name] if name in derived else base[name]
        lines.append(f"{name}={fmt(palette)}")
    OUT.write_text("\n".join(lines) + "\n")
    print("wrote", OUT.relative_to(ROOT), {k: len(v) for k, v in derived.items()})


if __name__ == "__main__":
    main()
