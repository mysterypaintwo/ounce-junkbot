# LEGO Junkbot for Switch 2

[![Download Junkbot NRO](https://img.shields.io/badge/Download-Switch%202%20NRO-2ea44f?style=for-the-badge&logo=github)](https://github.com/mysterypaintwo/ounce-junkbot/releases)

[**Download the playable `.nro` from GitHub Releases**](https://github.com/mysterypaintwo/ounce-junkbot/releases)

## Requirements

- devkitPro toolchain with libounce and the Ounce Switch 2 libraries in `/opt/devkitpro` (`DEVKITPRO` may override it).
- git with git-lfs.
- Python 3 with `Pillow` and `numpy`.
- `ffmpeg`.
- Your own legally owned original Junkbot data: `junkbot.dir` or a cloned `junkbot-code` tree in a neighboring directory, next to this repo.

## Build

Point the asset pipeline at your own game dump, then build:

```sh
export JUNKBOT_DIR=/path/to/junkbot.dir   # or: export JUNKBOT_CODE=/path/to/junkbot-code
make clean all
cp build/junkbot.nro junkbot.nro
```

Generated output is not committed.

## Legal note

LEGO and JUNKbot are trademarks of their respective owners. This is an unofficial homebrew port for personal use. Do not distribute the generated `romfs/` or extracted original assets; build them from your own legally owned copy of the game.
