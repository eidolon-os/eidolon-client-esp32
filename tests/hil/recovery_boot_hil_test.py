"""The verifier must not turn absent or failing hardware evidence into PASS."""
import unittest

from recovery_boot_hil import assess, redact


BASELINE = """W (210) Application: EIDOLON-BUILDSTAMP git=abcd12345 sdk=sdk123
I (6000) LocalMdns: resolve result=ESP_OK address=192.168.3.230 ms=200
W (10000) EidolonVoice: [dispatch] type=0 wait_ms=0 run_ms=163 pending=0
I (12000) EidolonUiPresenter: visible state=READY scene=7
I (40000) SystemInfo: free sram: 50000
I (50000) SystemInfo: free sram: 50020
"""


class EvidenceTest(unittest.TestCase):
    def check(self, log):
        return assess(log, "abcd1234", "sdk123", "192.168.3.230")

    def test_ready_and_normal_heap_changes(self):
        result = self.check(BASELINE)
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["settled_heap_delta"], 20)

    def test_later_success_does_not_erase_earlier_failure(self):
        for failure in (
            "E (5000) HubHttp: GET phase=open error=ESP_ERR_HTTP_CONNECT",
            "E (5000) LiveKitSession: Failure: RTC",
            "Guru Meditation Error: panic'ed",
        ):
            with self.subTest(failure=failure):
                self.assertEqual(self.check(failure + "\n" + BASELINE)["result"], "FAIL")

    def test_missing_setup_or_wrong_baseline_cannot_pass(self):
        self.assertEqual(self.check("")["result"], "BLOCKED")
        self.assertEqual(self.check(BASELINE.replace("state=READY", "state=SETUP"))["result"], "BLOCKED")
        self.assertEqual(self.check(BASELINE.replace("sdk=sdk123", "sdk=old"))["result"], "FAIL")
        self.assertEqual(self.check(BASELINE.replace("git=abcd12345", "git=abcd12345+dirty"))["result"], "FAIL")
        self.assertEqual(self.check(BASELINE + BASELINE)["result"], "FAIL")

    def test_timeout_is_not_reported_as_a_wrong_address(self):
        result = self.check(BASELINE + "I (60000) LocalMdns: resolve result=ESP_ERR_NOT_FOUND address=- ms=8000\n"
                            "E (60010) esp-tls: getaddrinfo() returns 202\n")
        self.assertEqual(result["result"], "FAIL")
        self.assertNotIn("mDNS returned an unexpected Hub address", result["reasons"])
        self.assertEqual(self.check(BASELINE.replace("192.168.3.230", "192.168.3.17"))["result"], "FAIL")

    def test_credentials_are_redacted(self):
        self.assertEqual(redact("a=ice-pwd:secret"), "a=ice-pwd:[REDACTED]")
        self.assertEqual(redact("AGENT: user:foo psw:bar"), "AGENT: user:[REDACTED] psw:[REDACTED]")


if __name__ == "__main__":
    unittest.main()
