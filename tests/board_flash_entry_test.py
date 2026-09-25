"""Run the actual menu-shared flash functions with fake hardware commands."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BoardFlashEntryTest(unittest.TestCase):
    def test_run_does_not_hide_failed_build_or_flash_by_starting_monitor(self):
        for board in ('m5stack-stackchan', 'm5stack-core-s3', 'esp32-s3-touch-amoled-2.06'):
            source = (ROOT / f'scripts/eidolon/eidolon-{board}.sh').read_text()
            function = 'cmd_run() {' + source.split('cmd_run() {', 1)[1].split('\n}', 1)[0] + '\n}'
            for failure in ('build', 'flash'):
                script = f'''
set -euo pipefail
SKIP_BUILD=0
cmd_build() {{ echo build; [[ {failure} != build ]]; }}
cmd_flash() {{ echo flash; [[ {failure} != flash ]]; }}
cmd_monitor() {{ echo monitor; }}
{function}
status=0
cmd_run || status=$?
echo status:$status
'''
                result = subprocess.run(['bash', '-c', script], capture_output=True, text=True, check=True)
                self.assertNotIn('monitor', result.stdout)
                self.assertTrue(result.stdout.endswith('status:1\n'))

    def test_all_callers_observe_write_and_runtime_verification_failures(self):
        for board in ('m5stack-stackchan', 'm5stack-core-s3', 'esp32-s3-touch-amoled-2.06'):
            source = (ROOT / f'scripts/eidolon/eidolon-{board}.sh').read_text()
            function = 'cmd_flash() {' + source.split('cmd_flash() {', 1)[1].split('\n}', 1)[0] + '\n}'
            for app_only in (0, 1):
                for failure in ('none', 'write', 'verify'):
                    with self.subTest(board=board, app_only=app_only, failure=failure):
                        script = f'''
set -euo pipefail
APP_ONLY={app_only}
FLASH_PARTITIONS=()
PROJECT_ROOT=/unused
require_idf() {{ :; }}
verify_board_sdkconfig() {{ :; }}
eidolon_prepare_build() {{ :; }}
detect_port() {{ echo test-port; }}
idf() {{ echo "write:$1"; [[ {failure} != write ]]; }}
eidolon_verify_flashed() {{ echo verify; [[ {failure} != verify ]]; }}
{function}
status=0
# Menus call through an OR-list, which disables implicit errexit in functions.
cmd_flash || status=$?
echo status:$status
'''
                        result = subprocess.run(['bash', '-c', script], capture_output=True, text=True, check=True)
                        lines = result.stdout.splitlines()
                        self.assertEqual(lines[0], 'write:app-flash' if app_only else 'write:flash')
                        self.assertEqual('verify' in lines, failure != 'write')
                        self.assertEqual(lines[-1], 'status:0' if failure == 'none' else 'status:1')


if __name__ == '__main__':
    unittest.main()
