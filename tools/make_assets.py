#!/usr/bin/env python3
"""Generate junkbot.otr beside the NRO for an end user.

Run with no arguments to clone the public exported source tree, or pass your own
junkbot.dir (or an exported junkbot-code directory):

    python3 make_assets.py
    python3 make_assets.py /path/to/junkbot.dir
    python3 make_assets.py --dir /path/to/junkbot.dir
    python3 make_assets.py --code /path/to/junkbot-code
    python3 make_assets.py --skip-intro
    python3 make_assets.py --levels /path/to/levels

When an intro SWF is present, the Ruffle exporter is built once from Ruffle's
pinned source and cached in a user-level directory (override with
JUNKBOT_EXPORTER_CACHE). Missing Rust and Java toolchains are downloaded into
that cache from their public distributors. `--ruffle-exporter-url` may supply a
prebuilt binary/archive instead, and `--skip-intro` omits the cinematic. The last form
supplies extra level text if the main source is missing one. If generation reports a missing
level, the script fetches the level texts from
https://github.com/1j01/janitorial-android automatically. The NRO contains no
original art or audio; this script only builds an external archive from a
source you provide or fetch yourself.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
if (HERE / "tools" / "make_otr.py").is_file():
    ROOT = HERE
elif (HERE / "make_otr.py").is_file():
    ROOT = HERE.parent
else:
    ROOT = HERE
DEFAULT_CLONE_URL = "https://github.com/JakeSteam/junkbot-code"
CLONE_DIR = ROOT / ".junkbot-code"
DEFAULT_LEVELS_CLONE_URL = "https://github.com/1j01/janitorial-android"
LEVELS_CLONE_DIR = ROOT / ".janitorial-android"
FFMPEG_URL = os.environ.get(
    "JUNKBOT_FFMPEG_URL",
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip")
RUFFLE_TAG = os.environ.get("RUFFLE_TAG", "v0.8.0")
RUFFLE_SOURCE_URL = (
    "https://github.com/ruffle-rs/ruffle/releases/download/"
    f"{RUFFLE_TAG}/ruffle-{RUFFLE_TAG.lstrip('v')}-reproducible-source.zip"
)
RUFFLE_EXPORTER_URL = os.environ.get("RUFFLE_EXPORTER_URL")
EXPORTER_NAME = "ruffle-exporter.exe" if os.name == "nt" else "ruffle-exporter"


def exporter_cache_dir() -> Path:
    override = os.environ.get("JUNKBOT_EXPORTER_CACHE")
    if override:
        return Path(override).expanduser().resolve()
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA") or
                    Path.home() / "AppData" / "Local")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
    return (base / "junkbot" / f"ruffle-{RUFFLE_TAG}").resolve()


EXPORTER_CACHE = exporter_cache_dir()
LEGACY_EXPORTER_CACHE = ROOT / ".ruffle-exporter"


def tool_cache_dir() -> Path:
    """User-level cache for helper binaries (portable ffmpeg on Windows)."""
    override = os.environ.get("JUNKBOT_TOOLS_CACHE")
    if override:
        return Path(override).expanduser().resolve()
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA") or
                    Path.home() / "AppData" / "Local")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
    return (base / "junkbot" / "tools").resolve()


def ffmpeg_root() -> Path:
    # Keep using the cache folder the old Windows launcher created.
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA") or
                    Path.home() / "AppData" / "Local")
        return (base / "JunkbotTools" / "ffmpeg").resolve()
    return (tool_cache_dir() / "ffmpeg").resolve()


FFMPEG_ROOT = ffmpeg_root()


def find_ffmpeg_exe(root: Path) -> Path | None:
    if not root.is_dir():
        return None
    direct = root / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
    if direct.is_file():
        return direct
    matches = sorted(path for path in root.rglob("*")
                     if path.is_file() and path.name.lower() == "ffmpeg.exe")
    return matches[0] if matches else None
JAVA_ROOT = EXPORTER_CACHE / "java"
JAVA_VERSION = os.environ.get("JUNKBOT_JAVA_VERSION", "17")
JAVA_URL = os.environ.get("JUNKBOT_JAVA_URL")
RUSTUP_URL = os.environ.get("JUNKBOT_RUSTUP_URL")
RUST_ROOT = EXPORTER_CACHE / "rust"
CARGO_HOME = RUST_ROOT / "cargo"
RUSTUP_HOME = RUST_ROOT / "rustup"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?",
                        help="optional junkbot.dir file or junkbot-code directory")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--dir", dest="dir_path", type=Path,
                        help="original junkbot.dir movie")
    source.add_argument("--code", dest="code_path", type=Path,
                        help="exported junkbot-code tree")
    parser.add_argument("--intro-swf", type=Path,
                        help="optional intro_anim.swf (used with --dir)")
    parser.add_argument("--skip-intro", action="store_true",
                        help="omit the SWF intro even when one is available")
    parser.add_argument("--ruffle-exporter-url",
                        help="optional prebuilt Ruffle exporter binary/archive "
                             "URL; otherwise build from pinned source")
    parser.add_argument("--levels", type=Path,
                        help="extra level .txt directory used to fill gaps")
    parser.add_argument("--levels-repo", default=DEFAULT_LEVELS_CLONE_URL,
                        help=f"level fallback repository (default: {DEFAULT_LEVELS_CLONE_URL})")
    parser.add_argument("--no-levels-fetch", action="store_true",
                        help="do not fetch level texts when the source is incomplete")
    parser.add_argument("--out", type=Path, default=ROOT / "junkbot.otr",
                        help="output archive (default: beside junkbot.nro)")
    parser.add_argument("--clone-url", default=DEFAULT_CLONE_URL,
                        help=f"source repository (default: {DEFAULT_CLONE_URL})")
    parser.add_argument("--no-clone", action="store_true",
                        help="require --dir/--code instead of cloning")
    return parser.parse_args()


def ensure_python_deps() -> None:
    required = (("pillow", "PIL"), ("numpy", "numpy"))
    missing = [name for name, module in required
               if importlib.util.find_spec(module) is None]
    if not missing:
        return
    try:
        subprocess.run([sys.executable, "-m", "pip", "--version"],
                       check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        try:
            subprocess.run([sys.executable, "-m", "ensurepip", "--upgrade"],
                           check=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError):
            raise SystemExit("pip is unavailable; install pillow and numpy manually")
    cmd = [sys.executable, "-m", "pip", "install",
           "--disable-pip-version-check", *missing]
    if sys.prefix == sys.base_prefix:
        cmd.append("--user")
    print(f"python: installing {', '.join(missing)}")
    try:
        subprocess.run(cmd, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(
            f"could not install {', '.join(missing)} automatically ({exc}). "
            "Install them with pip, or run the generator in a virtual environment.")


def ensure_ffmpeg() -> str | None:
    found = shutil.which("ffmpeg")
    if found:
        return found
    if os.name != "nt":
        return None
    exe = find_ffmpeg_exe(FFMPEG_ROOT)
    if exe is None:
        print("ffmpeg: downloading a portable build into the user cache")
        with tempfile.TemporaryDirectory(prefix="jb_ffmpeg_") as tmp:
            archive = Path(tmp) / "ffmpeg.zip"
            try:
                download_url(FFMPEG_URL, archive, label="ffmpeg")
                with zipfile.ZipFile(archive) as zf:
                    members = [name for name in zf.namelist()
                               if Path(name).name.lower() == "ffmpeg.exe"]
                    if not members:
                        raise ValueError("archive has no ffmpeg.exe")
                    exe = FFMPEG_ROOT / "ffmpeg.exe"
                    part = exe.with_suffix(".part")
                    exe.parent.mkdir(parents=True, exist_ok=True)
                    part.write_bytes(zf.read(members[0]))
                    part.replace(exe)
            except Exception as exc:
                print(f"ffmpeg: download failed: {exc}")
                return None
    os.environ["PATH"] = str(exe.parent) + os.pathsep + os.environ.get("PATH", "")
    return shutil.which("ffmpeg")


def check_dependencies() -> None:
    ensure_python_deps()
    missing: list[str] = []
    for module in ("PIL", "numpy"):
        if importlib.util.find_spec(module) is None:
            missing.append(module)
    if ensure_ffmpeg() is None:
        missing.append("ffmpeg")
    if missing:
        raise SystemExit("missing dependencies: " + ", ".join(missing))


def github_archive_url(url: str, ref: str) -> str:
    base = url.strip().rstrip("/")
    if base.endswith(".git"):
        base = base[:-4]
    if ref == "HEAD":
        return f"{base}/archive/HEAD.zip"
    return f"{base}/archive/refs/heads/{ref}.zip"


def fetch_repo(url: str, dest: Path, expected: str, label: str) -> Path:
    if dest.exists():
        if (dest / expected).is_dir():
            print(f"using cached {label} created by this script: {dest}")
            return dest
        raise SystemExit(f"{dest} exists but is not a valid {label} tree")
    print(f"fetching {label} from {url}")
    with tempfile.TemporaryDirectory(prefix="jb_repo_") as tmp:
        temp = Path(tmp)
        archive = temp / "source.zip"
        error: Exception | None = None
        for ref in ("HEAD", "main", "master"):
            try:
                download_url(github_archive_url(url, ref), archive, label=label)
                error = None
                break
            except Exception as exc:
                error = exc
        if error is not None:
            raise SystemExit(f"could not fetch {label}: {error}")
        extracted = temp / "extracted"
        extract_download(archive, extracted)
        roots = [path for path in extracted.iterdir() if path.is_dir()]
        if len(roots) != 1:
            raise SystemExit(f"unexpected archive layout while fetching {label}")
        shutil.move(str(roots[0]), str(dest))
    if not (dest / expected).is_dir():
        raise SystemExit(f"fetched {label} did not contain {expected}: {dest}")
    return dest


def clone_source(url: str) -> Path:
    return fetch_repo(url, CLONE_DIR, "files", "source")


def clone_levels(url: str) -> Path:
    return fetch_repo(url, LEVELS_CLONE_DIR, "levels", "level texts")


def cached_exporter() -> str | None:
    explicit = os.environ.get("RUFFLE_EXPORTER")
    if explicit and Path(explicit).is_file():
        return str(Path(explicit).resolve())
    found = shutil.which("ruffle-exporter")
    if found:
        return found
    for root in (EXPORTER_CACHE, LEGACY_EXPORTER_CACHE):
        cached = root / EXPORTER_NAME
        if cached.is_file():
            return str(cached)
    return None


def installer_dest() -> Path:
    return EXPORTER_CACHE / EXPORTER_NAME


def install_exporter(found: Path | str) -> str:
    source = Path(found).resolve()
    dest = installer_dest()
    dest.parent.mkdir(parents=True, exist_ok=True)
    if source != dest:
        shutil.copy2(source, dest)
    os.chmod(dest, 0o755)
    return str(dest)


def java_bin_name() -> str:
    return "java.exe" if os.name == "nt" else "java"


def java_usable(java_bin: Path) -> bool:
    try:
        result = subprocess.run(
            [str(java_bin), "-version"], text=True, capture_output=True)
        return result.returncode == 0
    except OSError:
        return False


def find_java(root: Path) -> Path | None:
    if not root.is_dir():
        return None
    for path in sorted(root.rglob(java_bin_name())):
        if path.is_file() and java_usable(path):
            return path
    return None


def apply_java(java_bin: Path) -> None:
    bin_dir = java_bin.parent
    os.environ["JAVA_HOME"] = str(bin_dir.parent)
    os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ.get("PATH", "")


def java_available() -> bool:
    found = shutil.which("java")
    if found and java_usable(Path(found)):
        return True
    candidates: list[Path] = []
    java_home = os.environ.get("JAVA_HOME")
    if java_home:
        candidates.append(Path(java_home).expanduser() / "bin")
    if sys.platform == "darwin":
        try:
            result = subprocess.run(
                ["/usr/libexec/java_home"], text=True, capture_output=True)
            if result.returncode == 0 and result.stdout.strip():
                candidates.append(Path(result.stdout.strip()) / "bin")
        except OSError:
            pass
    for candidate in candidates:
        exe = candidate / java_bin_name()
        if exe.is_file() and java_usable(exe):
            apply_java(exe)
            return True
    cached = find_java(JAVA_ROOT)
    if cached:
        apply_java(cached)
        return True
    return False


def host_platform() -> tuple[str, str]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        arch = "aarch64"
    elif machine in ("x86_64", "amd64"):
        arch = "x64"
    elif machine in ("i386", "i486", "i586", "i686", "x86"):
        arch = "x86"
    elif machine.startswith("armv7"):
        arch = "arm"
    else:
        arch = machine
    if system == "darwin":
        return "mac", "aarch64" if arch == "aarch64" else "x64"
    if system == "windows":
        return "windows", arch
    return "linux", arch


def java_download_url() -> str:
    if JAVA_URL:
        return JAVA_URL
    os_key, arch = host_platform()
    return (f"https://api.adoptium.net/v3/binary/latest/{JAVA_VERSION}/ga/"
            f"{os_key}/{arch}/jre/hotspot/normal/eclipse")


def install_java() -> Path | None:
    cached = find_java(JAVA_ROOT)
    if cached:
        apply_java(cached)
        return cached
    if JAVA_ROOT.exists():
        shutil.rmtree(JAVA_ROOT)
    archive = EXPORTER_CACHE / "java-download"
    try:
        download_url(java_download_url(), archive)
    except Exception as exc:
        print(f"ruffle: Java download failed: {exc}")
        return None
    try:
        extract_download(archive, JAVA_ROOT, java_bin_name())
    except Exception as exc:
        print(f"ruffle: could not unpack Java runtime: {exc}")
        return None
    found = find_java(JAVA_ROOT)
    if not found:
        print("ruffle: Java download did not contain a runtime")
        return None
    apply_java(found)
    return found


def ensure_java() -> bool:
    if java_available():
        return True
    print("ruffle: downloading a private Java runtime from Adoptium (one time)")
    found = install_java()
    if not found:
        return False
    print(f"ruffle: Java runtime ready at {found}")
    return True


def cargo_bin_name() -> str:
    return "cargo.exe" if os.name == "nt" else "cargo"


def cached_cargo() -> Path | None:
    roots = (CARGO_HOME / "bin", Path.home() / ".cargo" / "bin")
    for root in roots:
        cargo = root / cargo_bin_name()
        if cargo.is_file() and os.access(cargo, os.X_OK):
            return cargo
    return None


def apply_cargo(cargo: Path) -> None:
    bin_dir = cargo.parent
    os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ.get("PATH", "")
    if bin_dir.parent == CARGO_HOME:
        os.environ["CARGO_HOME"] = str(CARGO_HOME)
        os.environ["RUSTUP_HOME"] = str(RUSTUP_HOME)


def rust_env() -> dict[str, str]:
    env = os.environ.copy()
    cargo = cached_cargo()
    if cargo:
        env["PATH"] = str(cargo.parent) + os.pathsep + env.get("PATH", "")
        if cargo.parent.parent == CARGO_HOME:
            env["CARGO_HOME"] = str(CARGO_HOME)
            env["RUSTUP_HOME"] = str(RUSTUP_HOME)
    return env


def rustup_download_url() -> str:
    if RUSTUP_URL:
        return RUSTUP_URL
    if os.name == "nt":
        machine = platform.machine().lower()
        arch = "aarch64" if machine in ("arm64", "aarch64") else "x86_64"
        return f"https://win.rustup.rs/{arch}"
    return "https://sh.rustup.rs"


def install_cargo() -> Path | None:
    cached = cached_cargo()
    if cached:
        apply_cargo(cached)
        return cached
    CARGO_HOME.parent.mkdir(parents=True, exist_ok=True)
    installer = EXPORTER_CACHE / (
        "rustup-init.exe" if os.name == "nt" else "rustup-init")
    if not installer.is_file():
        try:
            download_url(rustup_download_url(), installer)
        except Exception as exc:
            print(f"ruffle: Rust installer download failed: {exc}")
            return None
    if os.name != "nt":
        os.chmod(installer, 0o755)
    env = os.environ.copy()
    env["CARGO_HOME"] = str(CARGO_HOME)
    env["RUSTUP_HOME"] = str(RUSTUP_HOME)
    result = subprocess.run(
        [str(installer), "-y", "--profile", "minimal",
         "--default-toolchain", "stable", "--no-modify-path"],
        env=env, text=True, capture_output=True)
    if result.returncode != 0:
        print("ruffle: Rust installer failed")
        print((result.stderr or result.stdout)[-1200:])
        return None
    cargo = cached_cargo()
    if not cargo:
        print("ruffle: Rust installer did not provide cargo")
        return None
    apply_cargo(cargo)
    return cargo


def ensure_cargo() -> bool:
    if shutil.which("cargo") or cached_cargo():
        return True
    print("ruffle: downloading a private Rust toolchain via rustup (one time)")
    if not install_cargo():
        return False
    print(f"ruffle: Rust toolchain ready at {cached_cargo()}")
    return True


def intro_requested(kind: str, source: Path, intro_swf: Path | None) -> bool:
    env_swf = os.environ.get("INTRO_SWF")
    if env_swf and Path(env_swf).is_file():
        return True
    if intro_swf and intro_swf.is_file():
        return True
    if kind == "code" and (source / "files/loading/intro_anim.swf").is_file():
        return True
    return False


def safe_target(root: Path, name: str) -> Path:
    root = root.resolve()
    target = (root / name).resolve()
    if target != root and root not in target.parents:
        raise SystemExit(f"unsafe path in downloaded archive: {name}")
    return target


def download_url(url: str, dest: Path, label: str = "ruffle") -> None:
    print(f"{label}: downloading {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if shutil.which("curl"):
        result = subprocess.run(
            ["curl", "-fsSL", url, "-o", str(dest)],
            text=True, capture_output=True)
        if result.returncode == 0:
            return

    import ssl
    context = ssl.create_default_context()
    try:
        import certifi
        context = ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    request = urllib.request.Request(
        url, headers={"User-Agent": "junkbot-asset-generator"})
    with urllib.request.urlopen(request, timeout=120, context=context) as response:
        with dest.open("wb") as output:
            shutil.copyfileobj(response, output)


def extract_download(archive: Path, dest: Path, fallback_name: str | None = None) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            for name in zf.namelist():
                safe_target(dest, name)
            zf.extractall(dest)
    elif tarfile.is_tarfile(archive):
        with tarfile.open(archive) as tf:
            for member in tf.getmembers():
                safe_target(dest, member.name)
                if not (member.isfile() or member.isdir()):
                    raise SystemExit(
                        f"unsafe path in downloaded archive: {member.name}")
            try:
                tf.extractall(dest, filter="data")
            except TypeError:
                tf.extractall(dest)
    else:
        target = dest / (fallback_name or EXPORTER_NAME)
        shutil.copy2(archive, target)
        os.chmod(target, 0o755)


def find_exporter(root: Path) -> Path | None:
    wanted = {EXPORTER_NAME.lower(), "exporter",
              "exporter.exe" if os.name == "nt" else "exporter"}
    matches = sorted(path for path in root.rglob("*")
                     if path.is_file() and path.name.lower() in wanted)
    for path in matches:
        if os.name == "nt" or os.access(path, os.X_OK):
            return path
    return matches[0] if matches else None


def download_exporter(url: str) -> str | None:
    with tempfile.TemporaryDirectory(prefix="jb_ruffle_dl_") as tmp:
        temp = Path(tmp)
        archive = temp / "download"
        try:
            download_url(url, archive)
        except Exception as exc:
            print(f"ruffle: download failed: {exc}")
            return None
        extracted = temp / "extracted"
        try:
            extract_download(archive, extracted)
        except Exception as exc:
            print(f"ruffle: could not unpack download: {exc}")
            return None
        found = find_exporter(extracted)
        if not found:
            print("ruffle: download did not contain an exporter binary")
            return None
        path = install_exporter(found)
    print(f"ruffle: exporter ready at {path}")
    return path


def cargo_build_exporter(source: Path, target: Path) -> bool:
    cargo = cached_cargo() or shutil.which("cargo")
    if not cargo:
        print("ruffle: cargo is unavailable")
        return False
    env = rust_env()
    env["CARGO_TARGET_DIR"] = str(target)
    result = subprocess.run(
        [cargo, "build", "--locked", "--release", "--package", "exporter"],
        cwd=source, text=True, capture_output=True, env=env)
    if result.returncode != 0:
        print("ruffle: exporter build failed")
        print((result.stderr or result.stdout)[-1200:])
        return False
    built = target / "release" / (
        "exporter.exe" if os.name == "nt" else "exporter")
    if not built.is_file():
        print("ruffle: build finished without an exporter binary")
        return False
    path = install_exporter(built)
    print(f"ruffle: exporter ready at {path}")
    return True


def valid_ruffle_source(path: Path) -> bool:
    return ((path / "Cargo.toml").is_file() and
            (path / "exporter" / "Cargo.toml").is_file())


def prepare_ruffle_source(build_root: Path) -> Path:
    source = build_root / "source"
    if valid_ruffle_source(source):
        print("ruffle: reusing cached Ruffle source")
        return source
    if source.exists():
        shutil.rmtree(source)
    source.mkdir(parents=True, exist_ok=True)

    archive = build_root / "source.zip"
    if archive.is_file():
        print("ruffle: reusing cached Ruffle source archive")
    else:
        download_url(RUFFLE_SOURCE_URL, archive)
    extract_download(archive, source)

    if not (source / "Cargo.toml").is_file():
        nested = sorted(source.glob("*/Cargo.toml"))
        if not nested:
            raise RuntimeError("Ruffle source archive has no Cargo.toml")
        nested_root = nested[0].parent
        for item in list(nested_root.iterdir()):
            target = source / item.name
            if target.exists():
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
            shutil.move(str(item), str(target))
        shutil.rmtree(nested_root, ignore_errors=True)
    if not valid_ruffle_source(source):
        raise RuntimeError("Ruffle source archive has no exporter crate")
    return source


def build_exporter() -> str | None:
    if not ensure_cargo():
        print("ruffle: cargo toolchain is unavailable")
        return None
    if not ensure_java():
        print("ruffle: Java runtime is unavailable")
        return None
    build_root = EXPORTER_CACHE / "build"
    build_root.mkdir(parents=True, exist_ok=True)
    target = build_root / "target"
    if target.is_dir():
        print("ruffle: reusing cargo build cache")
    print(f"ruffle: building pinned exporter from Ruffle {RUFFLE_TAG} "
          "(first run takes a few minutes; later runs use the cache)")

    try:
        source = prepare_ruffle_source(build_root)
        if cargo_build_exporter(source, target):
            return str(installer_dest())
    except Exception as exc:
        print(f"ruffle: source archive build failed: {exc}")

    if shutil.which("git"):
        source = build_root / "git-source"
        if not valid_ruffle_source(source):
            print("ruffle: retrying from the Ruffle repository")
            result = subprocess.run(
                ["git", "clone", "--depth", "1", "--branch", RUFFLE_TAG,
                 "https://github.com/ruffle-rs/ruffle", str(source)],
                text=True, capture_output=True)
            if result.returncode != 0:
                print((result.stderr or result.stdout)[-1200:])
                return None
        if cargo_build_exporter(source, target):
            return str(installer_dest())
    return None


def ensure_ruffle_exporter(needs_intro: bool, explicit_url: str | None = None) -> None:
    if not needs_intro:
        return
    existing = cached_exporter()
    if existing:
        os.environ["RUFFLE_EXPORTER"] = existing
        print(f"ruffle: using exporter {existing}")
        return

    print("ruffle: exporter not found; provisioning once into the user cache")
    explicit_url = explicit_url or RUFFLE_EXPORTER_URL
    if explicit_url:
        path = download_exporter(explicit_url)
        if path:
            os.environ["RUFFLE_EXPORTER"] = path
            return
        print("ruffle: supplied exporter download failed; building from source")

    path = build_exporter()
    if path:
        os.environ["RUFFLE_EXPORTER"] = path
        return

    raise SystemExit(
        "could not build the Ruffle exporter automatically.\n"
        "Set RUFFLE_EXPORTER=/path/to/exporter, pass --ruffle-exporter-url URL,\n"
        "override JUNKBOT_JAVA_URL/JUNKBOT_RUSTUP_URL for restricted networks,\n"
        "or pass --skip-intro to generate a package without the cinematic.")


def resolve_source(args: argparse.Namespace) -> tuple[str, Path]:
    if args.source:
        if args.dir_path or args.code_path:
            raise SystemExit("pass the source either positionally or with --dir/--code")
        path = Path(args.source).expanduser()
        if not path.exists():
            raise SystemExit(f"source not found: {path}")
        path = path.resolve()
        if path.is_dir():
            return "code", path
        if path.suffix.lower() == ".dir":
            return "dir", path
        raise SystemExit(f"expected a .dir file or a directory: {path}")
    if args.dir_path:
        path = args.dir_path.expanduser().resolve()
        if not path.is_file():
            raise SystemExit(f"not a file: {path}")
        return "dir", path
    if args.code_path:
        path = args.code_path.expanduser().resolve()
        if not path.is_dir():
            raise SystemExit(f"not a directory: {path}")
        return "code", path
    if args.no_clone:
        raise SystemExit("no source supplied and cloning is disabled")
    return "code", clone_source(args.clone_url)


def main() -> int:
    args = parse_args()
    kind, source = resolve_source(args)
    check_dependencies()

    intro_swf = args.intro_swf
    if kind == "dir" and intro_swf is None:
        nearby = source.parent / "intro_anim.swf"
        if nearby.is_file():
            intro_swf = nearby
    if intro_swf is not None:
        intro_swf = intro_swf.expanduser().resolve()
        if not intro_swf.is_file():
            raise SystemExit(f"not a file: {intro_swf}")

    ensure_ruffle_exporter(
        not args.skip_intro and intro_requested(kind, source, intro_swf),
        args.ruffle_exporter_url)

    maker = ROOT / "tools" / "make_otr.py"
    if not maker.is_file():
        raise SystemExit(f"missing generator script: {maker}")
    out = args.out.expanduser().resolve()
    cmd = [sys.executable, str(maker), f"--{kind}", str(source), "-o", str(out)]
    if intro_swf is not None:
        cmd += ["--intro-swf", str(intro_swf)]
    if args.skip_intro:
        cmd += ["--skip-intro"]

    if not (ROOT / "junkbot.nro").is_file() and not (ROOT / "build" / "junkbot.nro").is_file():
        print(f"note: junkbot.nro is not beside this script ({ROOT})")
    levels = args.levels.resolve() if args.levels else None

    retried = False
    while True:
        run_cmd = cmd + (["--levels", str(levels)] if levels else [])
        print(f"generating {out} from {source}")
        result = subprocess.run(run_cmd, cwd=ROOT, text=True,
                                capture_output=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode == 0:
            break
        message = result.stdout + result.stderr
        if (not retried and levels is None and not args.no_levels_fetch and
                "level(s) missing from source" in message):
            print("source is missing level data; fetching canonical level texts")
            levels = clone_levels(args.levels_repo) / "levels"
            retried = True
            continue
        raise SystemExit(result.returncode)

    print(f"done. Copy junkbot.nro and {out.name} to SD:/switch/junkbot/ on the console.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
