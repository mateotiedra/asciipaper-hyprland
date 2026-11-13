"""Pixel-level tests against the built renderer, not a simulated GL interface.

Run after `make` with Pillow and software EGL installed. No Wayland connection,
Hyprland IPC, or real pointer movement is needed.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageChops


ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "build/stage"
SIZE = (960, 600)
BACKGROUND = (16, 16, 20)


class NativeRenderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for relative in ("launcher.py", "engine/asciipaper-engine",
                         "lib/fish-pointer.so", "wallpaper.json", "wallpaper.glsl",
                         "media/shark-loop.mkv.frames"):
            if not (STAGE / relative).is_file():
                raise RuntimeError(f"Missing build/stage/{relative}; run make before testing")
        cls.temporary = tempfile.TemporaryDirectory(prefix="asciipaper native % ")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.env = {key: value for key, value in os.environ.items()
                   if key not in {"WAYLAND_DISPLAY", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE",
                                  "LD_PRELOAD", "ASCIIPAPER_FISH_MODE"}}
        cls.env.update({"HOME": str(cls.base / "home"),
                        "XDG_CONFIG_HOME": str(cls.base / "config"),
                        "XDG_DATA_HOME": str(cls.base / "data"),
                        "XDG_CACHE_HOME": str(cls.base / "cache"),
                        "XDG_RUNTIME_DIR": str(cls.base / "runtime"),
                        "LIBGL_ALWAYS_SOFTWARE": "1", "EGL_PLATFORM": "surfaceless"})
        Path(cls.env["XDG_RUNTIME_DIR"]).mkdir(mode=0o700)
        cls.template = json.loads((STAGE / "wallpaper.json").read_text())
        cls.template["shader"] = str(STAGE / "wallpaper.glsl")
        cls.template["media"] = str(STAGE / "media/shark-loop.mkv")
        cls.cache = {}

    def render(self, *, uniforms=None, pointer="0,0", seconds=5, fresh=False):
        config = json.loads(json.dumps(self.template))
        config["uniforms"].update(uniforms or {})
        key = (json.dumps(config, sort_keys=True), pointer, seconds)
        if not fresh and key in self.cache:
            return self.cache[key].copy()
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            directory = Path(directory)
            spec = directory / "wallpaper.json"
            output = directory / "snapshot.png"
            spec.write_text(json.dumps(config))
            command = [sys.executable, str(STAGE / "launcher.py"), "render", str(output),
                       "--config", str(spec), "--size", f"{SIZE[0]}x{SIZE[1]}",
                       "--seconds", str(seconds)]
            if pointer is not None:
                command.append(f"--pointer={pointer}")
            result = subprocess.run(
                command, env=self.env, cwd=self.base, capture_output=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            with Image.open(output) as source:
                image = source.convert("RGB")
            self.assertEqual(image.size, SIZE)
        self.cache[key] = image.copy()
        return image

    @staticmethod
    def ink_mask(image):
        background = Image.new("RGB", image.size, BACKGROUND)
        return ImageChops.difference(image, background).convert("L").point(
            lambda value: 255 if value > 2 else 0)

    @staticmethod
    def ink_count(image):
        return NativeRenderTests.ink_mask(image).histogram()[255]

    def test_omitting_native_pointer_leaves_blank_canvas_without_corner_fish(self):
        image = self.render(uniforms={"brightness": -4, "matrixDensity": 0},
                            pointer=None)
        background = Image.new("RGB", SIZE, BACKGROUND)
        self.assertIsNone(ImageChops.difference(image, background).getbbox(),
                          "an absent pointer must not create a fish at the origin")

    def test_native_pointer_places_fish_and_repeat_renders_are_deterministic(self):
        isolated = {"brightness": -4, "matrixDensity": 0}
        left = self.render(uniforms=isolated, pointer="0.3,0.35")
        repeat = self.render(uniforms=isolated, pointer="0.3,0.35", fresh=True)
        right = self.render(uniforms=isolated, pointer="0.75,0.7")
        self.assertEqual(left.tobytes(), repeat.tobytes(),
                         "native snapshot must not depend on monotonic wall time")
        for image, pointer in ((left, (0.3, 0.35)), (right, (0.75, 0.7))):
            mask = self.ink_mask(image)
            bounds = mask.getbbox()
            self.assertIsNotNone(bounds, "native pointer fish was not rendered")
            self.assertGreater(mask.histogram()[255], 50)
            x0, y0, x1, y1 = bounds
            # The head is at the pointer and the articulated tail extends left.
            self.assertLess(abs(x1 - pointer[0] * SIZE[0]), 0.06 * SIZE[0])
            self.assertLess(abs((y0 + y1) / 2 - pointer[1] * SIZE[1]), 0.06 * SIZE[1])
            self.assertLess(x1 - x0, SIZE[1] * 0.3)
            self.assertLess(y1 - y0, SIZE[1] * 0.16)
        self.assertGreater(self.ink_mask(right).getbbox()[0],
                           self.ink_mask(left).getbbox()[2])

    def test_rain_off_controls_agree_and_brightness_changes_only_rain_ink(self):
        common = {"brightness": -4, "matrixDensity": 4,
                  "matrixTrailLength": 12, "matrixSpeed": 3}
        density_off = self.render(uniforms={**common, "matrixDensity": 0})
        brightness_off = self.render(uniforms={**common, "matrixBrightness": 0})
        self.assertEqual(density_off.tobytes(), brightness_off.tobytes())
        low = self.render(uniforms={**common, "matrixBrightness": 0.2})
        high = self.render(uniforms={**common, "matrixBrightness": 1})
        changed = ImageChops.difference(high, density_off)
        self.assertGreater(sum(changed.convert("L").histogram()[1:]), 100)
        # Ignore the fish at the top-left: compare rain's distance from canvas.
        box = (0, SIZE[1] // 10, SIZE[0], SIZE[1])
        def energy(image):
            difference = ImageChops.difference(image.crop(box),
                                              density_off.crop(box))
            return sum(value * count for channel in difference.split()
                       for value, count in enumerate(channel.histogram()))
        self.assertGreater(energy(high), energy(low))
        self.assertGreater(energy(low), 0)

    def test_rain_never_overwrites_visible_video_or_pointer_foreground(self):
        off = self.render(uniforms={"matrixDensity": 0}, pointer="0.7,0.6")
        on = self.render(uniforms={"matrixDensity": 6, "matrixBrightness": 1,
                                   "matrixTrailLength": 20}, pointer="0.7,0.6")
        foreground = self.ink_mask(off)
        self.assertGreater(foreground.histogram()[255], 500,
                           "fixture must contain visible shark/video foreground")
        difference = ImageChops.difference(off, on)
        masked = Image.composite(difference, Image.new("RGB", SIZE), foreground)
        self.assertIsNone(masked.getbbox(), "rain painted over the existing foreground")
        self.assertIsNotNone(difference.getbbox(), "rain must still appear in empty cells")

    def test_rain_speed_zero_freezes_descent_and_mutation_interval_controls_glyphs(self):
        static = {"brightness": -4, "matrixBrightness": 1, "matrixDensity": 6,
                  "matrixTrailLength": 16, "matrixSpeed": 0,
                  "matrixChangeInterval": 0}
        first = self.render(uniforms=static, seconds=1)
        later = self.render(uniforms=static, seconds=17)
        self.assertEqual(first.tobytes(), later.tobytes(),
                         "zero speed and disabled mutations should freeze rain")
        moving = {**static, "matrixSpeed": 5}
        self.assertNotEqual(self.render(uniforms=moving, seconds=1).tobytes(),
                            self.render(uniforms=moving, seconds=17).tobytes())
        mutating = {**static, "matrixChangeInterval": 0.5}
        self.assertNotEqual(self.render(uniforms=mutating, seconds=1).tobytes(),
                            self.render(uniforms=mutating, seconds=17).tobytes())

    def test_longer_rain_trails_cover_more_background(self):
        common = {"brightness": -4, "matrixBrightness": 1, "matrixDensity": 6,
                  "matrixSpeed": 0, "matrixChangeInterval": 0}
        short = self.render(uniforms={**common, "matrixTrailLength": 2})
        long = self.render(uniforms={**common, "matrixTrailLength": 20})
        self.assertGreater(self.ink_count(long), self.ink_count(short) + 100)

    def test_stdout_recording_exports_complete_rgba_frames_without_log_pollution(self):
        config = json.loads(json.dumps(self.template))
        config["uniforms"].update({"brightness": -4, "matrixDensity": 0})
        spec = self.base / "raw export.json"
        spec.write_text(json.dumps(config))
        width, height, frames = 320, 200, 2
        result = subprocess.run(
            [sys.executable, str(STAGE / "launcher.py"), "render", "-",
             "--config", str(spec), "--size", f"{width}x{height}",
             "--seconds", "0.5", "--record", "4", "--warmup", "0.5",
             "--pointer=0.6,0.5"],
            env=self.env, cwd=self.base, capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        frame_size = width * height * 4
        self.assertEqual(len(result.stdout), frames * frame_size)
        first = Image.frombytes("RGBA", (width, height), result.stdout[:frame_size])
        self.assertEqual(first.getchannel("A").getextrema(), (255, 255))
        self.assertGreater(self.ink_count(first.convert("RGB")), 30,
                           "raw export must contain the native pointer fish")
        self.assertEqual(result.stdout[:frame_size], result.stdout[frame_size:],
                         "a resting fish on a blank canvas must remain stationary")


if __name__ == "__main__":
    unittest.main()
