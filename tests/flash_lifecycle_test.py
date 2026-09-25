"""Execute the shared shell lifecycle; every failed phase must stop successors."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'scripts/eidolon/eidolon-common.sh').read_text()
FUNCTION = 'eidolon_flash() {' + SOURCE.split('eidolon_flash() {', 1)[1].split('\n}', 1)[0] + '\n}'

class FlashLifecycleTest(unittest.TestCase):
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
