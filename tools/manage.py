#!/usr/bin/env python3
"""Install/uninstall only hash-tracked package files, without contacting services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile

NAME = "asciipaper-hyprland"
ROOT = Path(__file__).resolve().parents[1]


def locations():
    home = Path.home().absolute()
    def xdg(name, fallback):
        value = Path(os.environ.get(name) or home / fallback).expanduser()
        if not value.is_absolute():
            raise RuntimeError(f"{name} must be an absolute path")
        return value
    data = xdg("XDG_DATA_HOME", ".local/share") / NAME
    config = xdg("XDG_CONFIG_HOME", ".config")
    # Read XDG_CACHE_HOME as part of path validation, but installation needs no cache.
    xdg("XDG_CACHE_HOME", ".cache")
    return data, config / NAME / "wallpaper.json", home / ".local/bin" / NAME, config / "systemd/user" / (NAME + ".service")


def safe_path(path):
    if ".." in path.parts:
        raise RuntimeError(f"parent traversal is not allowed: {path}")
    for candidate in (path, *path.parents):
        if candidate.is_symlink():
            raise RuntimeError(f"refusing symlink path: {candidate}")
        if candidate != path and candidate.exists() and not candidate.is_dir():
            raise RuntimeError(f"parent is not a directory: {candidate}")
    if path.exists() and not path.is_file() and not path.is_dir():
        raise RuntimeError(f"refusing special file: {path}")


def digest(path):
    safe_path(path)
    if not path.exists():
        return None
    if not path.is_file():
        raise RuntimeError(f"expected a regular file: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode()


def replace(path, content, mode=0o644):
    safe_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".asciipaper-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(content)
            os.fchmod(output.fileno(), mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def allowed(path, roots):
    data, config, command, unit = roots
    return path in (config, command, unit) or (path.is_relative_to(data) and path != data and path != data / ".manifest.json")


def load_manifest(path, roots):
    safe_path(path)
    if not path.exists():
        return {"version": 1, "files": {}, "directories": [], "assets": {}}
    value = json.loads(path.read_text())
    if not isinstance(value, dict) or value.get("version") != 1 or not isinstance(value.get("files"), dict) or not isinstance(value.get("directories"), list):
        raise RuntimeError(f"invalid ownership manifest: {path}")
    for filename, checksum in value["files"].items():
        target = Path(filename)
        if not target.is_absolute() or not allowed(target, roots) or not isinstance(checksum, str) or len(checksum) != 64:
            raise RuntimeError(f"invalid ownership entry: {filename}")
        safe_path(target)
    for directory in value["directories"]:
        target = Path(directory)
        if not target.is_absolute() or not (target == roots[0] or target.is_relative_to(roots[0]) or target == roots[1].parent):
            raise RuntimeError(f"invalid owned directory: {directory}")
        safe_path(target)
    return value


def systemd_quote(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%").replace("$", "$$").replace("\n", "\\n").replace("\r", "\\r") + '"'


def install(stage, roots):
    data, config, command, unit = roots
    manifest = data / ".manifest.json"
    for path in (*roots, manifest):
        safe_path(path)
    previous = load_manifest(manifest, roots)
    if data.exists() and not manifest.exists():
        raise RuntimeError(f"refusing unmanaged data directory: {data}")
    stage = stage.expanduser().absolute()
    if not stage.is_dir() or stage.is_symlink():
        raise RuntimeError(f"no regular stage directory: {stage}; run make first")
    template = json.loads((stage / "wallpaper.json").read_text())
    assets = {"shader": str(data / "wallpaper.glsl"), "media": str(data / "media/shark-loop.mkv")}
    template.update(assets)
    desired = {}
    for source in sorted(stage.rglob("*")):
        if source.is_symlink():
            raise RuntimeError(f"stage symlinks are not supported: {source}")
        if source.is_file() and source != stage / "wallpaper.json":
            target = data / source.relative_to(stage)
            if target == manifest:
                raise RuntimeError("stage cannot supply an ownership manifest")
            desired[target] = (source.read_bytes(), stat.S_IMODE(source.stat().st_mode))
    for required in ("engine/asciipaper-engine", "lib/fish-pointer.so", "launcher.py", "wallpaper.glsl", "media/shark-loop.mkv", "media/shark-loop.mkv.frames"):
        if data / required not in desired:
            raise RuntimeError(f"incomplete stage: missing {required}")
    wrapper = "#!/usr/bin/python3\nimport runpy\nrunpy.run_path(" + repr(str(data / "launcher.py")) + ", run_name='__main__')\n"
    desired[command] = (wrapper.encode(), 0o755)
    service = ("[Unit]\nDescription=Asciipaper Hyprland wallpaper\nAfter=graphical-session.target\nPartOf=graphical-session.target\n\n"
               "[Service]\nType=simple\nExecStart=/usr/bin/python3 " + systemd_quote(data / "launcher.py") + " run --config " + systemd_quote(config) + "\nRestart=on-failure\nRestartSec=3\n\n"
               "[Install]\nWantedBy=graphical-session.target\n")
    desired[unit] = (service.encode(), 0o644)
    if config.exists():
        if str(config) not in previous["files"]:
            raise RuntimeError(f"refusing unmanaged configuration: {config}")
        content = config.read_bytes()
        current = json.loads(content)
        changed = False
        for key, new in assets.items():
            if current.get(key) == previous.get("assets", {}).get(key) and current.get(key) != new:
                current[key] = new
                changed = True
        desired[config] = (encoded(current) if changed else content, stat.S_IMODE(config.stat().st_mode))
    else:
        desired[config] = (encoded(template), 0o644)
    # Preflight every destination before writing anything.
    preserve = set()
    for target in desired:
        safe_path(target)
        current = digest(target)
        old = previous["files"].get(str(target))
        if current is not None and old is None:
            raise RuntimeError(f"refusing unmanaged collision: {target}")
        if current is not None and current != old and target != config:
            preserve.add(target)
    files = dict(previous["files"])
    directories = set(previous["directories"])
    for target, (content, mode) in desired.items():
        if target in preserve:
            print(f"Preserved modified file: {target}")
            continue
        for directory in (target.parent, *target.parent.parents):
            if directory == data or directory.is_relative_to(data) or directory == config.parent:
                if not directory.exists():
                    directories.add(str(directory))
        replace(target, content, mode)
        files[str(target)] = hashlib.sha256(content).hexdigest()
    # Drop obsolete unchanged package files, never user-added files.
    for filename in previous["files"]:
        target = Path(filename)
        if target not in desired and target != config and digest(target) == previous["files"][filename]:
            target.unlink()
            files.pop(filename, None)
    replace(manifest, encoded({"version": 1, "files": files, "directories": sorted(directories), "assets": assets}))
    print(f"Installed. Stop competing wallpapers manually, then start explicitly:\n  {command} start\nNo services were started or reloaded.")


def uninstall(roots):
    data, config, command, unit = roots
    manifest = data / ".manifest.json"
    if not manifest.exists():
        safe_path(manifest)
        print("No managed installation found.")
        return
    previous = load_manifest(manifest, roots)
    remaining = {}
    for filename, checksum in previous["files"].items():
        target = Path(filename)
        current = digest(target)
        if current is None:
            continue
        if target == config or current != checksum:
            remaining[filename] = checksum
            print(f"Preserved {'configuration' if target == config else 'modified file'}: {target}")
        else:
            target.unlink()
    # Retain ownership for preserved config so reinstall is safe and non-destructive.
    if remaining:
        replace(manifest, encoded({**previous, "files": remaining}))
    else:
        manifest.unlink()
    for directory in sorted(previous["directories"], key=lambda item: len(Path(item).parts), reverse=True):
        target = Path(directory)
        try:
            target.rmdir()
        except FileNotFoundError:
            pass
        except OSError:
            if not target.is_dir():
                raise
    print("Removed unchanged managed files. Configuration and modified/user files remain. No services were contacted.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    install_parser = commands.add_parser("install")
    install_parser.add_argument("--stage", type=Path, default=ROOT / "build/stage")
    commands.add_parser("uninstall")
    args = parser.parse_args()
    roots = locations()
    if args.command == "install":
        install(args.stage, roots)
    else:
        uninstall(roots)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        sys.exit(f"manage: {error}")
