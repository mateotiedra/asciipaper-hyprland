#!/usr/bin/env python3
"""Render public showcase media from the staged native engine, never a desktop.

Run after `make`: python3 tools/render_demo.py
Requires the build toolchain, ffmpeg (libx264), and system DejaVu fonts. The
shipped stage config is used unchanged. Only tools/demo-pointer.c supplies
scripted input; fish-pointer.so remains responsible for all fish animation.
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
# Render the shipped cell grid at the approved desktop's native resolution;
# only encoded animations are downscaled, never the PNG evidence.
WIDTH, HEIGHT, FPS = 2880, 1800, 24
SECONDS, WARMUP = 8, 2
# One second into the recording (three seconds into simulation): the pointer
# fish sits above/right of the video shark, keeping both subjects distinct.
POSTER_FRAME = FPS


def strip_png_metadata(path):
    """Keep only image data/colour essentials, without timestamps or text."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("native showcase output is not a PNG")
    result = bytearray(data[:8])
    offset = 8
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        end = offset + length + 12
        if end > len(data):
            raise RuntimeError("truncated showcase PNG")
        kind = data[offset + 4:offset + 8]
        if kind in (b"IHDR", b"PLTE", b"IDAT", b"IEND", b"tRNS"):
            result.extend(data[offset:end])
        offset = end
    path.write_bytes(result)


def render(stage, destination):
    engine = stage / "engine/asciipaper-engine"
    plugin = stage / "lib/fish-pointer.so"
    config = stage / "wallpaper.json"
    library = stage / "engine/wallpapers/lib"
    for required in (engine, plugin, config, library / "looks.json",
                     stage / "wallpaper.glsl", stage / "media/shark-loop.mkv.frames"):
        if not required.exists():
            raise RuntimeError("stage incomplete; run make before rendering showcase media")
    for executable in ("ffmpeg", "pkg-config"):
        if shutil.which(executable) is None:
            raise RuntimeError(f"required executable not found: {executable}")

    # A private export-only environment avoids importing personal engine options,
    # fonts, compositor state or preload libraries. /tmp also keeps LD_PRELOAD
    # entries free of whitespace even when the checkout path contains spaces.
    with tempfile.TemporaryDirectory(prefix="asciipaper-demo-", dir="/tmp") as work:
        work = Path(work)
        env = os.environ.copy()
        env.pop("LD_PRELOAD", None)
        for name in ("WAYLAND_DISPLAY", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE",
                     "XDG_RUNTIME_DIR", "FONTCONFIG_FILE", "FONTCONFIG_PATH"):
            env.pop(name, None)
        env.update({
            "HOME": str(work / "home"),
            "XDG_CONFIG_HOME": str(work / "config"),
            "XDG_DATA_HOME": str(work / "data"),
            "XDG_CACHE_HOME": str(work / "cache"),
            "LIBGL_ALWAYS_SOFTWARE": "1",
            "EGL_PLATFORM": "surfaceless",
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
        })
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            Path(env[name]).mkdir()
        helper = work / "demo-pointer.so"
        production = work / "fish-pointer.so"
        production.symlink_to(plugin)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "glesv2"], env=env, text=True))
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([
            *compiler, "-std=c11", "-O2", "-fPIC", "-shared", "-Wall",
            "-Wextra", "-Wpedantic", *flags, str(ROOT / "tools/demo-pointer.c"),
            "-o", str(helper), "-ldl", "-lm",
        ], env=env, check=True)
        native_env = env | {
            "ASCIIPAPER_FISH_MODE": "native",
            "LD_PRELOAD": f"{helper}:{production}",
        }
        raw_command = [
            str(engine), "--spec", str(config), "--lib", str(library),
            "--snapshot", "-", "--size", f"{WIDTH}x{HEIGHT}",
            "--seconds", str(SECONDS), "--record", str(FPS),
            "--warmup", str(WARMUP), "--pointer", "0.5,0.54",
        ]
        ffmpeg = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                  "-filter_complex_threads", "1"]
        filters = (
            "[0:v]split=3[fullmovie][overview][detail];"
            "[fullmovie]scale=1440:900:flags=lanczos[movie];"
            f"[overview]select=eq(n\\,{POSTER_FRAME})[poster];"
            f"[detail]select=eq(n\\,{POSTER_FRAME}),"
            "crop=960:600:1550:330[closeup]"
        )
        encode_command = [
            *ffmpeg, "-f", "rawvideo", "-pixel_format", "rgba",
            "-video_size", f"{WIDTH}x{HEIGHT}", "-framerate", str(FPS),
            "-i", "pipe:0", "-filter_complex", filters,
            "-map", "[movie]", "-an", "-map_metadata", "-1",
            "-fflags", "+bitexact", "-flags:v", "+bitexact", "-threads", "1",
            "-c:v", "libx264", "-preset", "slow", "-crf", "23",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart",
            str(work / "demo.mp4"),
            "-map", "[poster]", "-frames:v", "1", "-update", "1",
            "-map_metadata", "-1", "-flags:v", "+bitexact", "-threads", "1",
            str(work / "overview.png"),
            "-map", "[closeup]", "-frames:v", "1", "-update", "1",
            "-map_metadata", "-1", "-flags:v", "+bitexact", "-threads", "1",
            str(work / "fish-detail.png"),
        ]
        # A closed pipe (not /dev/null) also preserves the native export stdin
        # contract if the upstream loop starts polling its control descriptor.
        read_fd, write_fd = os.pipe()
        os.close(write_fd)
        try:
            producer = subprocess.Popen(raw_command, cwd=stage, env=native_env,
                                        stdin=read_fd, stdout=subprocess.PIPE)
        finally:
            os.close(read_fd)
        try:
            with producer.stdout:
                encoder = subprocess.run(encode_command, stdin=producer.stdout, env=env)
            native_status = producer.wait()
        except BaseException:
            producer.kill()
            producer.wait()
            raise
        if native_status:
            raise RuntimeError(f"native showcase renderer failed ({native_status})")
        encoder.check_returncode()
        subprocess.run([
            *ffmpeg, "-i", str(work / "demo.mp4"),
            "-filter_complex",
            "fps=12,scale=1280:-1:flags=lanczos,split[a][b];"
            "[a]palettegen=max_colors=96:stats_mode=diff[p];"
            "[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle",
            "-an", "-map_metadata", "-1", "-fflags", "+bitexact",
            "-flags:v", "+bitexact", "-threads", "1", "-loop", "0",
            str(work / "demo.gif"),
        ], env=env, check=True)
        for name in ("overview.png", "fish-detail.png"):
            strip_png_metadata(work / name)
        destination.mkdir(parents=True, exist_ok=True)
        for name in ("demo.gif", "demo.mp4", "overview.png", "fish-detail.png"):
            source = work / name
            if not source.is_file() or source.stat().st_size == 0:
                raise RuntimeError(f"missing showcase output: {name}")
        for name in ("demo.gif", "demo.mp4", "overview.png", "fish-detail.png"):
            shutil.copyfile(work / name, destination / name)
            print(f"{name}: {(destination / name).stat().st_size:,} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, default=ROOT / "build/stage")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/media")
    args = parser.parse_args()
    try:
        render(args.stage.resolve(), args.output.resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"render_demo: {error}\n")


if __name__ == "__main__":
    main()
