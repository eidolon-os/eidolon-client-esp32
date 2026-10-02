"""Execute the shared shell lifecycle; every failed phase must stop successors."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'scripts/eidolon/eidolon-common.sh').read_text()
FUNCTION = 'eidolon_flash() {' + SOURCE.split('eidolon_flash() {', 1)[1].split('\n}', 1)[0] + '\n}'

class FlashLifecycleTest(unittest.TestCase):
    def test_selected_board_overrides_cached_identity_for_both_builds(self):
        script = f'''
set -euo pipefail
BOARD_NAME=esp32-s3-touch-amoled-2.06
BOARD_PATH=waveshare/esp32-s3-touch-amoled-2.06
eidolon_idf_python() {{ echo gate; }}
gate() {{ :; }}
writer() {{ printf '%s\\n' "$*"; }}
eidolon_verify_flashed() {{ :; }}
{FUNCTION}
eidolon_flash /root build /dev/test app-flash writer -p /dev/test
'''
        result = subprocess.run(['bash', '-c', script], text=True, capture_output=True, check=True)
        flags = '-p /dev/test -DBOARD_NAME=esp32-s3-touch-amoled-2.06 -DBOARD_TYPE=waveshare/esp32-s3-touch-amoled-2.06 '
        self.assertEqual(result.stdout.splitlines(), [flags+'build', flags+'app-flash'])

    def test_order_and_failure_propagation_even_in_menu_or_list(self):
        for action in ('flash', 'app-flash'):
            phases = ['build', 'preflight', action, 'finish', 'verify']
            for failure in ['none'] + phases:
                script = f'''
set -euo pipefail
phase() {{ echo "$1"; [[ "$1" != "{failure}" ]]; }}
eidolon_idf_python() {{ echo gate; }}
gate() {{ phase "$2"; }}
writer() {{ phase "$1"; }}
eidolon_verify_flashed() {{ phase verify; }}
{FUNCTION}
status=0
eidolon_flash /root build /dev/test {action} writer || status=$?
echo status:$status
'''
                r = subprocess.run(['bash', '-c', script], text=True, capture_output=True, check=True)
                expected = phases if failure == 'none' else phases[:phases.index(failure)+1]
                self.assertEqual(r.stdout.splitlines(), expected + ['status:0' if failure == 'none' else 'status:1'])

if __name__ == '__main__': unittest.main()
