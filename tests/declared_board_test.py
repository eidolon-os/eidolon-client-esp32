"""Declared-board helpers: one sdkconfig source per board, and ports by USB bridge."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMMON = ROOT / 'scripts/eidolon/eidolon-common.sh'
KORVO = ROOT / 'scripts/eidolon/eidolon-korvo-1.sh'


def bash(script, check=True):
    return subprocess.run(['bash', '-c', script], capture_output=True, text=True, check=check)


class BoardOverlayTest(unittest.TestCase):
    def test_overlay_is_exactly_the_release_list_plus_the_board_type(self):
        config = json.loads((ROOT / 'main/boards/korvo-1/config.json').read_text())
        expected = config['builds'][0]['sdkconfig_append']
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'overlay'
            bash(f'source "{COMMON}"; eidolon_board_overlay "{ROOT}" korvo-1 CONFIG_BOARD_TYPE_KORVO_1 "{out}"')
            lines = out.read_text().splitlines()
        self.assertTrue(lines[0].startswith('# Generated from'))
        self.assertEqual(lines[1:], ['CONFIG_BOARD_TYPE_KORVO_1=y', *expected])

    def test_overlay_refuses_a_conflicting_board_type_or_unknown_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            board = root / 'main/boards/demo'
            board.mkdir(parents=True)
            (board / 'config.json').write_text(json.dumps(
                {'builds': [{'name': 'demo', 'sdkconfig_append': ['CONFIG_BOARD_TYPE_OTHER=y']}]}))
            out = root / 'overlay'
            conflict = bash(f'source "{COMMON}"; eidolon_board_overlay "{root}" demo CONFIG_BOARD_TYPE_DEMO "{out}"',
                            check=False)
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn('CONFIG_BOARD_TYPE_OTHER', conflict.stderr)
            missing = bash(f'source "{COMMON}"; eidolon_board_overlay "{root}" demo X "{out}" nope', check=False)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("no build named 'nope'", missing.stderr)


class SdkconfigTrackTest(unittest.TestCase):
    def test_sdkconfig_is_regenerated_only_when_its_inputs_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            defaults, sdkconfig = tmp / 'defaults', tmp / 'sdkconfig'
            defaults.write_text('CONFIG_A=y\n')
            track = f'source "{COMMON}"; eidolon_sdkconfig_track "{sdkconfig}" "{defaults}"'
            bash(track)
            sdkconfig.write_text('CONFIG_A=y\nCONFIG_MENUCONFIG_TWEAK=y\n')
            bash(track)
            self.assertTrue(sdkconfig.exists(), 'unchanged inputs must keep hand edits')
            defaults.write_text('CONFIG_A=n\n')
            result = bash(track)
            self.assertFalse(sdkconfig.exists(), 'changed inputs must not be shadowed by an old sdkconfig')
            self.assertIn('regenerating', result.stdout)


class KorvoPortSelectionTest(unittest.TestCase):
    def detect(self, found, port=''):
        fake = ''.join(f'echo {p}; ' for p in found)
        script = f'''
source "{KORVO}"
require_idf() {{ :; }}
eidolon_usb_ports() {{ [[ "$1" == "{'10c4:ea60'}" ]] || exit 9; {fake}}}
eidolon_usb_port_table() {{ :; }}
PORT="{port}"
detect_port
'''
        return bash(script, check=False)

    def test_explicit_port_wins(self):
        self.assertEqual(self.detect([], port='/dev/cu.given').stdout.strip(), '/dev/cu.given')

    def test_only_a_port_behind_the_korvo_bridge_is_chosen(self):
        result = self.detect(['/dev/cu.usbserial-2140'])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), '/dev/cu.usbserial-2140')

    def test_toolchain_chatter_never_becomes_part_of_the_port(self):
        script = f'''
source "{KORVO}"
require_idf() {{ echo ">> [common] Loading ESP-IDF v6.1"; }}
eidolon_usb_ports() {{ echo /dev/cu.usbserial-2140; }}
PORT=""
printf '[%s]' "$(detect_port)"
'''
        self.assertEqual(bash(script).stdout, '[/dev/cu.usbserial-2140]')

    def test_no_bridge_or_two_bridges_refuse_instead_of_guessing(self):
        self.assertNotEqual(self.detect([]).returncode, 0)
        two = self.detect(['/dev/cu.usbserial-1', '/dev/cu.usbserial-2'])
        self.assertNotEqual(two.returncode, 0)
        self.assertEqual(two.stdout.strip(), '')


if __name__ == '__main__':
    unittest.main()
