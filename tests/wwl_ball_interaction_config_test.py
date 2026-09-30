import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/eidolon/eidolon-wwl-ball-s3-lcd-1.85.sh"


class WwlBallInteractionModeTest(unittest.TestCase):
    def configure(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env.pop("EIDOLON_INTERACTION_MODE", None)
            if mode is not None:
                env["EIDOLON_INTERACTION_MODE"] = mode
            command = '''source "$1"
BUILD_DIR="$2"
SDKCONFIG_FILE="$2/sdkconfig"
SDKCONFIG_OVERLAY="$2/overlay"
write_overlay
ensure_wwl_sdkconfig
'''
            subprocess.run(["bash", "-eu", "-c", command, "test", str(SCRIPT), directory],
                           env=env, check=True, capture_output=True)
            return {name: (Path(directory) / name).read_text()
                    for name in ("sdkconfig", "overlay")}

    def test_modes_only_change_input_configuration(self):
        configs = {mode: self.configure(mode) for mode in
                   (None, "half_duplex", "ptt")}
        self.assertEqual(configs[None], configs["ptt"])
        for mode, ptt, half in (("half_duplex", False, True),
                                ("ptt", True, False)):
            text = configs[mode]["sdkconfig"]
            self.assertEqual("CONFIG_EIDOLON_INTERACTION_MODE_PTT=y" in text, ptt)
            self.assertEqual("CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX=y" in text, half)

    def test_board_metadata_is_ptt_by_default(self):
        metadata = json.loads((ROOT / "main/boards/wwl-ball-s3-lcd-1.85/config.json").read_text())
        self.assertIn("CONFIG_EIDOLON_INTERACTION_MODE_PTT=y", str(metadata))
        self.assertIn("CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX=n", str(metadata))

    def test_invalid_mode_fails(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.configure("silent")
        with self.assertRaises(subprocess.CalledProcessError):
            self.configure("full_duplex")


if __name__ == "__main__":
    unittest.main()
