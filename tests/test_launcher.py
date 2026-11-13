"""Consumer CLI errors must reject options the native engine would reinterpret."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RenderArgumentTests(unittest.TestCase):
    def test_render_rejects_silently_clamped_sizes_rates_and_ambiguous_stdout(self):
        cases = [
            (["snapshot.png", "--size", "8x8"], ("--size", "16")),
            (["-", "--record", ".5"], ("--record", "1", "120")),
            (["-", "--record", "240"], ("--record", "1", "120")),
            (["-"], ("--record", "stdout")),
        ]
        for arguments, required_guidance in cases:
            with self.subTest(arguments=arguments), tempfile.TemporaryDirectory(
                    prefix="asciipaper invalid render % ") as directory:
                root = Path(directory)
                env = {"PATH": os.environ.get("PATH", ""), "HOME": str(root / "home"),
                       "XDG_CONFIG_HOME": str(root / "config"),
                       "XDG_DATA_HOME": str(root / "data"),
                       "XDG_CACHE_HOME": str(root / "cache"),
                       "LIBGL_ALWAYS_SOFTWARE": "1", "EGL_PLATFORM": "surfaceless"}
                result = subprocess.run(
                    [sys.executable, str(ROOT / "tools/launcher.py"), "render", *arguments],
                    env=env, cwd=root, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0,
                                    "invalid request must be rejected, not silently changed")
                for guidance in required_guidance:
                    self.assertIn(guidance, result.stderr,
                                  "error must explain the relevant option and supported bounds")
                self.assertFalse((root / "-").exists(),
                                 "stdout requests must not create a literal '-' file")
                self.assertFalse((root / "snapshot.png").exists(),
                                 "invalid dimensions must not produce a silently resized image")


if __name__ == "__main__":
    unittest.main()
