"""Exercise the actual embedded serial verifier across USB re-enumeration."""
import contextlib
import io
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

SOURCE = (Path(__file__).resolve().parents[1] / 'scripts/eidolon/eidolon-common.sh').read_text()
CODE = SOURCE.split("<<'PY' 2>/dev/null || true\n", 1)[1].split('\nPY\n)', 1)[0]

class ReconnectTest(unittest.TestCase):
    def run_probe(self, *, never_returns=False):
        clock = [0.0]
        instances = []
        class SerialException(OSError):
            pass
        class Serial:
            def __init__(self, **kw):
                self.rts = False
                self.closed = False
                self.index = len(instances)
                instances.append(self)
            def open(self):
                if self.index == 1 or never_returns:
                    raise SerialException('USB temporarily absent')
            def readline(self):
                clock[0] += .1
                if self.index == 0:
                    raise SerialException('USB re-enumerated after reset')
                return b'I (500) EIDOLON-BUILDSTAMP git=test sdk=test idf=5.5.4\n'
            def close(self):
                self.closed = True
        serial = types.SimpleNamespace(Serial=Serial, SerialException=SerialException)
        time = types.SimpleNamespace(monotonic=lambda: clock[0], sleep=lambda n: clock.__setitem__(0, clock[0]+n))
        out = io.StringIO()
        with patch.dict(sys.modules, serial=serial, time=time), patch.object(sys, 'argv', ['-', '/dev/test', '1']), contextlib.redirect_stdout(out):
            exec(CODE, {})
        return out.getvalue(), instances, clock[0]

    def test_recovers_after_reset_and_missing_port(self):
        out, instances, _ = self.run_probe()
        self.assertIn('EIDOLON-BUILDSTAMP git=test', out)
        self.assertEqual(len(instances), 3)
        self.assertTrue(all(p.closed for p in instances))

    def test_missing_port_remains_a_bounded_failure(self):
        out, instances, elapsed = self.run_probe(never_returns=True)
        self.assertEqual(out, '')
        self.assertLess(elapsed, 1.2)
        self.assertTrue(all(p.closed for p in instances))

if __name__ == '__main__': unittest.main()
