"""Test the journal and injected local bridge contract; no radio or socket peer."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import anan_hardware_run as run
from automation_probe import _map_get


class LocalBridge:
    def __init__(self, endpoint):
        self.endpoint = endpoint
        self.calls = []
        self.closed = False

    def request(self, obj, timeout_seconds=None):
        self.calls.append(obj)
        if obj == {"cmd": "ping"}:
            return {"ok": True, "version": "test-journal-only"}
        if obj["cmd"] != "get":
            raise AssertionError("Journal must never drive the app")
        self_check = {"cmd": "get", **_map_get([obj["model"]])}
        assert obj == self_check
        if obj["model"] == "radio":
            return {"ok": True, "radio": {"connected": True, "model": "ANAN-200D",
                                          "serial": "bench-fixture", "transmitting": False}}
        return {"ok": True, obj["model"]: []}

    def close(self):
        self.closed = True


class JournalTests(unittest.TestCase):
    def test_unperformed_checks_cannot_be_complete(self):
        self.assertEqual(run.outcome({}), "incomplete")
        checks = {name: "operator_pass" for name, _ in run.CHECKS}
        self.assertEqual(run.outcome(checks), "operator_checks_complete")
        checks["receive"] = "not_tested"
        self.assertEqual(run.outcome(checks), "incomplete")
        checks["receive"] = "operator_fail"
        self.assertEqual(run.outcome(checks), "operator_reported_failure")

    def test_four_case_journal_and_read_only_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.json"
            bridge = LocalBridge("injected")
            answers = iter([answer for _ in run.CASES for answer in
                            ["", *(["j"] * len(run.CHECKS)), "fixture observations"]])
            with patch("sys.argv", ["journal", "--socket", "injected", "--output", str(output)]), \
                 patch.object(run, "Bridge", return_value=bridge), \
                 patch("builtins.input", side_effect=lambda _: next(answers)), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run.main(), 0)
            report = json.loads(output.read_text())
            self.assertEqual([(c["device"], c["protocol"]) for c in report["cases"]], list(run.CASES))
            self.assertEqual(report["status"], "operator_checks_complete")
            self.assertIn("not independent RF", report["evidence"])
            self.assertTrue(bridge.closed)
            self.assertEqual({c["cmd"] for c in bridge.calls}, {"ping", "get"})

    def test_interruption_retains_partial_journal(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "partial.json"
            with patch("sys.argv", ["journal", "--socket", "injected", "--output", str(output)]), \
                 patch.object(run, "Bridge", return_value=LocalBridge("injected")), \
                 patch("builtins.input", side_effect=KeyboardInterrupt), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run.main(), 2)
            report = json.loads(output.read_text())
            self.assertEqual(report["status"], "interrupted")
            self.assertEqual(report["cases"][0]["checks"], {})
            self.assertEqual(list(Path(directory).iterdir()), [output])

    def test_wrong_mapping_is_refused(self):
        bridge = LocalBridge("injected")
        bridge.request = lambda *a, **kw: {"ok": True, "radio": {
            "connected": True, "model": "Orion MkII", "serial": "fixture"}}
        with self.assertRaises(RuntimeError):
            run.radio(bridge)

    def test_unready_device_does_not_skip_remaining_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "unready.json"
            bridge = LocalBridge("injected")
            original = bridge.request
            def disconnected(obj, **kwargs):
                if obj["cmd"] == "get":
                    return {"ok": True, "radio": {"connected": False}}
                return original(obj, **kwargs)
            bridge.request = disconnected
            with patch("sys.argv", ["journal", "--socket", "injected", "--output", str(output)]), \
                 patch.object(run, "Bridge", return_value=bridge), \
                 patch("builtins.input", return_value=""), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run.main(), 1)
            report = json.loads(output.read_text())
            self.assertEqual(len(report["cases"]), 4)
            self.assertTrue(all(case["status"] == "not_ready" for case in report["cases"]))


if __name__ == "__main__":
    unittest.main()
