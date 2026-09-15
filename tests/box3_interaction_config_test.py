"""Exercise the real board script without a toolchain or a firmware build."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/eidolon/eidolon-esp-box-3.sh"


class Box3InputModeTest(unittest.TestCase):
    def configure(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env.pop("EIDOLON_INTERACTION_MODE", None)
            if mode is not None:
                env["EIDOLON_INTERACTION_MODE"] = mode
            # The production helper is sourceable. Redirect only its output paths.
            command = '''source "$1"
BUILD_DIR="$2"
SDKCONFIG_FILE="$2/sdkconfig"
SDKCONFIG_OVERLAY="$2/overlay"
write_overlay
ensure_box3_sdkconfig
'''
            subprocess.run(["bash", "-eu", "-c", command, "test", str(SCRIPT), directory],
                           env=env, check=True, capture_output=True)
            return {name: (Path(directory) / name).read_text()
                    for name in ("sdkconfig", "overlay")}

    def test_modes_only_change_input_configuration(self):
        configs = {mode: self.configure(mode) for mode in
                   (None, "full_duplex", "half_duplex", "ptt")}
        self.assertEqual(configs[None], configs["full_duplex"])
        for mode, ptt, half in (("full_duplex", False, False),
                                ("half_duplex", False, True), ("ptt", True, False)):
            text = configs[mode]["sdkconfig"]
            self.assertEqual("CONFIG_EIDOLON_INTERACTION_MODE_PTT=y" in text, ptt)
            self.assertEqual("CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX=y" in text, half)
            for filename in ("sdkconfig", "overlay"):
                other = lambda s: [line for line in s.splitlines()
                                   if "EIDOLON_INTERACTION_MODE_" not in line]
                self.assertEqual(other(configs[mode][filename]),
                                 other(configs["full_duplex"][filename]))

    def test_board_metadata_keeps_original_full_duplex_default(self):
        metadata = json.loads((ROOT / "main/boards/esp-box-3/config.json").read_text())
        self.assertIn("CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX=n", str(metadata))
        self.assertIn("CONFIG_EIDOLON_INTERACTION_MODE_PTT=n", str(metadata))

    def test_invalid_mode_fails_before_writing(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.configure("silent")


if __name__ == "__main__":
    unittest.main()
