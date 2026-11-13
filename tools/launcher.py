#!/usr/bin/env python3
"""Run the packaged engine; systemd interaction requires an explicit command."""
import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys

UNIT = "asciipaper-hyprland.service"


def config_home():
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")


def data_root():
    here = Path(__file__).resolve().parent
    if (here / "engine" / "asciipaper-engine").is_file():
        return here
    return here.parent / "build" / "stage"


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return value


def nonnegative(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be a finite nonnegative number")
    return value


def require_session():
    for key in ("WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "XDG_RUNTIME_DIR"):
        if not os.environ.get(key):
            raise RuntimeError(f"{key} is missing; run this command inside your Hyprland session")


def require_monitor():
    require_session()
    result = subprocess.run(["hyprctl", "-j", "monitors"], check=True, capture_output=True, text=True)
    monitors = json.loads(result.stdout)
    if not isinstance(monitors, list) or len(monitors) != 1:
        raise RuntimeError("global pointer mode requires exactly ONE active Hyprland output; disable other outputs first")
    if monitors[0].get("transform", 0) != 0:
        raise RuntimeError("global pointer mode does not support rotated outputs")


def native(args):
    root = data_root()
    default = config_home() / "asciipaper-hyprland" / "wallpaper.json"
    # Stage launches remain directly renderable without installation.
    installed = root.name != "stage" or root.parent.name != "build"
    config = Path(args.config).expanduser().absolute() if args.config else (default if installed else root / "wallpaper.json")
    if not config.is_file():
        raise RuntimeError(f"no wallpaper configuration: {config}")
    command = [str(root / "engine" / "asciipaper-engine"), "--spec", str(config),
               "--lib", str(root / "engine" / "wallpapers" / "lib")]
    mode = "global"
    if args.command == "run":
        require_monitor()
    else:
        mode = "native"
        command += ["--snapshot", args.output]
        for name in ("size", "seconds", "pointer", "record", "warmup"):
            value = getattr(args, name)
            if value is not None:
                command += ["--" + name, value]
    environment = os.environ.copy()
    environment["ASCIIPAPER_FISH_MODE"] = mode
    # glibc splits LD_PRELOAD on spaces: an inherited fd also handles install
    # paths containing spaces, without copying the library to a shared location.
    library = os.open(root / "lib" / "fish-pointer.so", os.O_RDONLY)
    if library < 3:
        original = library
        library = fcntl.fcntl(original, fcntl.F_DUPFD_CLOEXEC, 3)
        os.close(original)
    os.set_inheritable(library, True)
    environment["LD_PRELOAD"] = f"/proc/self/fd/{library}" + (":" + environment["LD_PRELOAD"] if environment.get("LD_PRELOAD") else "")
    reader, writer = os.pipe()
    os.close(writer)
    os.dup2(reader, 0)
    os.set_inheritable(0, True)
    if reader != 0:
        os.close(reader)
    os.execve(command[0], command, environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="foreground Hyprland wallpaper (one unrotated output)")
    run.add_argument("--config")
    render = commands.add_parser("render", help="headless native snapshot or raw RGBA export")
    render.add_argument("output", help="PNG path, or '-' for raw RGBA stdout (requires --record)")
    render.add_argument("--config")
    render.add_argument("--size", default="1920x1080", help="WxH, each dimension at least 16")
    render.add_argument("--seconds", type=nonnegative, default="3")
    render.add_argument("--pointer")
    render.add_argument("--record", type=positive, help="raw RGBA recording FPS, from 1 through 120; requires OUTPUT '-'")
    render.add_argument("--warmup", type=nonnegative)
    for name in ("start", "stop", "status"):
        commands.add_parser(name)
    args = parser.parse_args()
    if args.command in ("run", "render"):
        if args.command == "render":
            if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", args.size):
                parser.error("--size must be positive WxH")
            if any(int(dimension) < 16 for dimension in args.size.split("x")):
                parser.error("--size dimensions must each be at least 16")
            if args.pointer:
                try:
                    coordinates = [float(part) for part in args.pointer.split(",")]
                    if len(coordinates) != 2 or not all(math.isfinite(v) for v in coordinates):
                        raise ValueError
                except ValueError:
                    parser.error("--pointer must contain two finite numbers X,Y")
            if args.record and args.output != "-":
                parser.error("--record requires OUTPUT '-' (raw RGBA stdout)")
            if args.output == "-" and args.record is None:
                parser.error("OUTPUT '-' requires --record FPS (raw RGBA stdout)")
            if args.record is not None and not 1 <= float(args.record) <= 120:
                parser.error("--record FPS must be between 1 and 120")
        native(args)
    else:
        if args.command == "start":
            require_monitor()
            names = [key for key in ("WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "XDG_RUNTIME_DIR", "DISPLAY", "XDG_CURRENT_DESKTOP", "PATH") if key in os.environ]
            subprocess.run(["systemctl", "--user", "import-environment", *names], check=True)
            subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        return subprocess.run(["systemctl", "--user", args.command, UNIT]).returncode
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"asciipaper-hyprland: {error}")
