"""Exercise the shared build entry against a local SDK and stale remote cache."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts/eidolon/eidolon-common.sh"


class LocalLivekitSdkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "firmware"
        self.root.mkdir()
        self.sdk = self.root.parent / "vendor/client-sdk-esp32"
        component = self.sdk / "components/livekit"
        component.mkdir(parents=True)
        (component / "idf_component.yml").write_text('version: "0.3.11~1"\n')
        for args in (["init", "-q"], ["add", "."], ["-c", "user.name=Test", "-c",
                     "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                     "commit", "-qm", "SDK fixture"]):
            subprocess.run(["git", "-C", str(self.sdk), *args], check=True, capture_output=True)
        self.revision = subprocess.check_output(
            ["git", "-C", str(self.sdk), "rev-parse", "--short=12", "HEAD"], text=True,
        ).strip()

    def call(self, function, **env):
        environ = {k: v for k, v in os.environ.items() if k != "EIDOLON_LIVEKIT_SDK"}
        return subprocess.run(
            ["bash", "-c", 'set -euo pipefail; source "$1"; "$2" "$3"',
             "test", str(HELPER), function, str(self.root)],
            capture_output=True, text=True, env={**environ, **env},
        )

    def test_stamp_identity_tracks_local_edits_not_remote_lock(self):
        (self.root / "dependencies.lock").write_text("stale remote commit")
        result = self.call("eidolon_sdk_revision")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "vendor:" + self.revision)
        (self.sdk / "components/livekit/idf_component.yml").write_text("local edits\n")
        result = self.call("eidolon_sdk_revision")
        self.assertEqual(result.stdout.strip(), "vendor:" + self.revision + "+dirty")

    def test_sync_preserves_lock_cache_and_build_outputs(self):
        paths = ["dependencies.lock", "managed_components/livekit__livekit/engine.c",
                 "build/eidolon/esp-box-3/eidolon.bin"]
        for name in paths:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("sentinel")
        result = self.call("eidolon_sdk_sync")
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in paths:
            self.assertEqual((self.root / name).read_text(), "sentinel")

    def test_legacy_remote_pin_override_is_rejected(self):
        result = self.call("eidolon_sdk_sync", EIDOLON_LIVEKIT_SDK="0.3.7")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no longer selects a remote pin", result.stderr)

    def test_missing_vendor_does_not_fall_back_to_managed_component(self):
        (self.sdk / "components/livekit/idf_component.yml").unlink()
        result = self.call("eidolon_sdk_sync")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing local LiveKit SDK", result.stderr)


if __name__ == "__main__":
    unittest.main()
