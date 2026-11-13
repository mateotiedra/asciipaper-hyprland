"""Installer behavior through its CLI; never uses the real desktop or services."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class InstallLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="asciipaper install % ")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.home = self.base / "home space %"
        self.home.mkdir()
        self.data_home = self.base / "data space %"
        self.config_home = self.base / "config space %"
        self.cache_home = self.base / "cache space %"
        self.data = self.data_home / "asciipaper-hyprland"
        self.config = self.config_home / "asciipaper-hyprland/wallpaper.json"
        self.command = self.home / ".local/bin/asciipaper-hyprland"
        self.unit = self.config_home / "systemd/user/asciipaper-hyprland.service"
        self.stage = self.base / "stage space %"
        fixtures = {
            "engine/asciipaper-engine": b"fixture engine payload\n",
            "engine/wallpapers/lib/looks.json": b"{}\n",
            "engine/tools/import_media.py": b"# fixture payload\n",
            "engine/LICENSE": b"engine license\n",
            "lib/fish-pointer.so": b"fixture native payload\n",
            "wallpaper.glsl": b"// fixture shader version one\n",
            "media/shark-loop.mkv": b"fixture video\n",
            "media/shark-loop.mkv.frames": b"fixture frames\n",
            "launcher.py": b"# fixture launcher payload\n",
            "wallpaper.json": json.dumps({
                "shader": "wallpaper.glsl", "media": "media/shark-loop.mkv",
                "uniforms": {"matrixBrightness": 0.5},
            }).encode(),
        }
        for name, contents in fixtures.items():
            target = self.stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
        (self.stage / "engine/asciipaper-engine").chmod(0o755)
        self.trap_marker = self.base / "unexpected-service-call"
        trap_dir = self.base / "traps"
        trap_dir.mkdir()
        for name in ("systemctl", "hyprctl", "pkill", "killall"):
            trap = trap_dir / name
            trap.write_text('#!/bin/sh\nprintf invoked > "$SERVICE_TRAP"\nexit 97\n')
            trap.chmod(0o755)
        self.env = {
            "PATH": str(trap_dir) + os.pathsep + os.environ.get("PATH", ""),
            "HOME": str(self.home), "XDG_DATA_HOME": str(self.data_home),
            "XDG_CONFIG_HOME": str(self.config_home),
            "XDG_CACHE_HOME": str(self.cache_home),
            "SERVICE_TRAP": str(self.trap_marker),
            "LANG": "C.UTF-8",
        }

    def manage(self, operation, *, success=True, env=None):
        args = [sys.executable, str(ROOT / "tools/manage.py"), operation]
        if operation == "install":
            args += ["--stage", str(self.stage)]
        result = subprocess.run(args, env=env or self.env, cwd=self.base,
                                capture_output=True, text=True, timeout=30)
        self.assertFalse(self.trap_marker.exists(), "lifecycle invoked a desktop/service command")
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, "unmanaged collision was accepted")
        return result

    def test_install_resolves_assets_and_preserves_unrelated_desktop_files(self):
        sentinels = [self.home / ".local/lib/ascii-pointer/user.so",
                     self.config_home / "systemd/user/asciipaper.service",
                     self.config_home / "hypr/hyprland.conf"]
        for path in sentinels:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"user-owned desktop data\n")
        self.manage("install")
        config = json.loads(self.config.read_text())
        self.assertEqual(Path(config["shader"]), self.data / "wallpaper.glsl")
        self.assertEqual(Path(config["media"]), self.data / "media/shark-loop.mkv")
        self.assertTrue(os.access(self.command, os.X_OK))
        self.assertTrue(self.unit.is_file())
        self.manage("uninstall")
        self.assertFalse(self.command.exists())
        self.assertFalse(self.unit.exists())
        self.assertFalse((self.data / "wallpaper.glsl").exists())
        self.assertTrue(self.config.exists(), "uninstall must retain user configuration")
        for path in sentinels:
            self.assertEqual(path.read_bytes(), b"user-owned desktop data\n")

    def test_upgrade_preserves_custom_config_media_and_updates_owned_assets(self):
        self.manage("install")
        custom = self.data / "media/my custom % shark.mkv"
        custom.write_bytes(b"user video, not package data\n")
        config = json.loads(self.config.read_text())
        config["media"] = str(custom)
        config["uniforms"]["matrixBrightness"] = 0.17
        config["user_note"] = "keep this"
        self.config.write_text(json.dumps(config))
        (self.stage / "wallpaper.glsl").write_bytes(b"// upgraded shader\n")
        self.manage("install")
        self.assertEqual(json.loads(self.config.read_text()), config)
        self.assertEqual((self.data / "wallpaper.glsl").read_bytes(), b"// upgraded shader\n")
        self.manage("uninstall")
        self.assertEqual(custom.read_bytes(), b"user video, not package data\n")
        self.assertEqual(json.loads(self.config.read_text()), config)

    def test_uninstall_retains_modified_owned_files_and_unmanaged_neighbors(self):
        self.manage("install")
        modified = self.data / "wallpaper.glsl"
        modified.write_bytes(b"// user's modified shader\n")
        neighbor = self.command.parent / "another-application"
        neighbor.write_bytes(b"not ours\n")
        self.manage("uninstall")
        self.assertEqual(modified.read_bytes(), b"// user's modified shader\n")
        self.assertEqual(neighbor.read_bytes(), b"not ours\n")
        self.assertFalse((self.data / "lib/fish-pointer.so").exists())

    def test_unmanaged_command_collision_is_never_overwritten(self):
        self.command.parent.mkdir(parents=True)
        self.command.write_bytes(b"unrelated command\n")
        self.manage("install", success=False)
        self.assertEqual(self.command.read_bytes(), b"unrelated command\n")

    def test_unmanaged_data_directory_collision_is_never_adopted(self):
        self.data.mkdir(parents=True)
        sentinel = self.data / "my-data"
        sentinel.write_bytes(b"unmanaged directory\n")
        self.manage("install", success=False)
        self.assertEqual(sentinel.read_bytes(), b"unmanaged directory\n")

    def test_symlink_parent_cannot_redirect_install_into_unowned_directory(self):
        outside = self.base / "unowned destination"
        outside.mkdir()
        self.data_home.mkdir(parents=True)
        self.data.symlink_to(outside, target_is_directory=True)
        self.manage("install", success=False)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertTrue(self.data.is_symlink())

    def test_default_xdg_locations_stay_under_temporary_home(self):
        env = dict(self.env)
        for key in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            env.pop(key)
        self.manage("install", env=env)
        config_path = self.home / ".config/asciipaper-hyprland/wallpaper.json"
        config = json.loads(config_path.read_text())
        self.assertEqual(Path(config["shader"]),
                         self.home / ".local/share/asciipaper-hyprland/wallpaper.glsl")
        self.manage("uninstall", env=env)
        self.assertTrue(config_path.is_file())
        self.assertFalse(self.command.exists())


if __name__ == "__main__":
    unittest.main()
