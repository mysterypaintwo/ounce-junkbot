# LEGO Junkbot for Switch 2

[![Download Junkbot](https://img.shields.io/badge/Download-Junkbot%20for%20Switch%202-2ea44f?style=for-the-badge&logo=github)](https://github.com/mysterypaintwo/ounce-junkbot/releases)

[**Download the playable `.nro` from GitHub Releases**](https://github.com/mysterypaintwo/ounce-junkbot/releases)

## Requirements

- devkitPro toolchain with libounce and the Ounce Switch 2 libraries in `/opt/devkitpro` (`DEVKITPRO` may override it).
- git with git-lfs.
- Python 3 with `Pillow` and `numpy`.
- `ffmpeg`.
- Your own legally owned original Junkbot data: a `junkbot-code` tree or `junkbot.dir`.
- Internet access and disk space for the first asset generation and any missing level texts; pass `--skip-intro` to omit the cinematic and skip the Ruffle toolchain download.

## Build

Build the small code-only NRO, then generate the external asset package from your own source:

```sh
make clean all

# From an exported junkbot-code tree:
make otr-code JUNKBOT_CODE=/path/to/junkbot-code

# Or from the original movie:
make otr-dir JUNKBOT_DIR=/path/to/junkbot.dir
```

Put `build/junkbot.nro` and the generated `junkbot.otr` in `SD:/switch/junkbot/` on the console.

`make release` creates `ounce-junkbot-v1.0.zip` with the NRO and `make-assets.sh`; the name comes from `VERSION_MAJOR`/`VERSION_MINOR` in the Makefile. Run that script with your own `junkbot.dir`, or with no arguments to clone `https://github.com/JakeSteam/junkbot-code` and generate the package for you. It writes `junkbot.otr` beside the script, downloads Rust and Java into a user-level cache if needed, builds the Ruffle exporter once from pinned source when an intro is present, and fills missing levels from `https://github.com/1j01/janitorial-android`. `--ruffle-exporter-url` can supply a prebuilt binary instead; `--skip-intro` omits the cinematic. The zip includes `readme.txt` with simple end-user steps; Windows users can double-click `make-assets.bat`, which installs Python and the other helper tools into a user cache.

Instead of generating a package on the host, you may put `junkbot.dir` in `SD:/switch/junkbot/`. The game builds `junkbot.otr` once on the console, removes the `.dir`, and starts at the title screen. This path cannot render the SWF intro.

Generated output is not committed.

## Legal note

LEGO and JUNKBOT are trademarks of their respective owners. This is an unofficial homebrew port for personal use. Do not distribute the generated `romfs/`, `junkbot.otr`, or extracted original assets; build them from your own legally owned copy of the game.
