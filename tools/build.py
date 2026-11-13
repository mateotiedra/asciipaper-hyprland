#!/usr/bin/env python3
"""Build pinned sources without consulting an installed engine."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True, cwd=ROOT)


def checkout(path, pin):
    fresh = not path.exists()
    if fresh:
        run("git", "clone", "--no-checkout", pin["url"], path)
    if not (path / ".git").is_dir():
        raise RuntimeError(f"not a source checkout: {path}")
    origin = subprocess.check_output(["git", "-C", str(path), "remote", "get-url", "origin"], text=True).strip()
    if origin != pin["url"]:
        raise RuntimeError(f"unexpected source remote at {path}: {origin}")
    if not fresh:
        dirty = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"], text=True)
        if dirty:
            raise RuntimeError(f"refusing to overwrite modified pinned source: {path}")
    run("git", "-C", path, "fetch", "--depth", "1", "origin", pin["revision"])
    run("git", "-C", path, "checkout", "--detach", pin["revision"])
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != pin["revision"]:
        raise RuntimeError(f"source pin mismatch at {path}")


def main():
    pins = json.loads((ROOT / "UPSTREAM.json").read_text())
    BUILD.mkdir(exist_ok=True)
    upstream, protocols = BUILD / "upstream", BUILD / "wayland-protocols"
    checkout(upstream, pins["engine"])
    checkout(protocols, pins["protocols"])
    run("make", "-C", upstream / "native", f"PROTOCOLS={protocols}")
    stage = BUILD / "stage"
    # A stage is disposable build output, never an installation or user's media.
    if stage.is_symlink():
        raise RuntimeError(f"refusing symlink stage: {stage}")
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "engine" / "tools").mkdir(parents=True)
    (stage / "lib").mkdir()
    shutil.copy2(upstream / "native" / "asciipaper-engine", stage / "engine" / "asciipaper-engine")
    shutil.copytree(upstream / "wallpapers" / "lib", stage / "engine" / "wallpapers" / "lib")
    shutil.copy2(upstream / "tools" / "import_media.py", stage / "engine" / "tools" / "import_media.py")
    shutil.copy2(upstream / "LICENSE", stage / "engine" / "LICENSE")
    shutil.copytree(upstream / "licenses", stage / "engine" / "licenses")
    shutil.copy2(protocols / "COPYING", stage / "engine" / "licenses" / "wayland-protocols-COPYING")
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(ROOT / name, stage / name)
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client", "glesv2"], text=True))
    run(*shlex.split(os.environ.get("CC", "cc")), *shlex.split(os.environ.get("CPPFLAGS", "")),
        *shlex.split(os.environ.get("CFLAGS", "-O2 -Wall -Wextra")), "-shared", "-fPIC",
        ROOT / "src" / "hide-cursor.c", ROOT / "src" / "cursor-tracker.c", ROOT / "src" / "fish-motion.c",
        "-o", stage / "lib" / "fish-pointer.so", *flags, "-pthread", "-ldl", "-lm",
        *shlex.split(os.environ.get("LDFLAGS", "")))
    run(sys.executable, ROOT / "tools" / "compose.py", "--lib", upstream / "wallpapers" / "lib", "--output", stage / "wallpaper.glsl")
    run(sys.executable, stage / "engine" / "tools" / "import_media.py", ROOT / "assets" / "shark-loop.mkv",
        "--out", stage / "media", "--name", "shark-loop", "--frame-width", "384", "--fps", "24")
    (stage / "media" / "shark-loop.json").unlink()
    shutil.copy2(ROOT / "config" / "wallpaper.json", stage / "wallpaper.json")
    shutil.copy2(ROOT / "tools" / "launcher.py", stage / "launcher.py")
    print(f"Built self-contained payload: {stage}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"build: {error}")
