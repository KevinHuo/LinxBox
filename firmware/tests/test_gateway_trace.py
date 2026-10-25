"""抓包校验不能仅接受根入口任意 data，也不能吞掉配对错误。"""
import importlib.util
from pathlib import Path
import unittest
import json
import io
import runpy
import tempfile
from contextlib import redirect_stdout
from unittest.mock import patch
from jsonschema.exceptions import ValidationError

SPEC = importlib.util.spec_from_file_location("trace_check", Path(__file__).resolve().parents[1] / "tools/check_gateway_trace.py")
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def hello():
    return {"direction": "device", "packet": {"id": "f1", "type": "device.hello", "payload": {"mac_address": "34:00:00:00:00:01"}}}


def response(reply="f1", data=None):
    return {"direction": "server", "packet": {"id": "g1", "type": "response", "reply_to": reply,
            "payload": {"ok": True, "data": data if data is not None else {"device_id": "34:00:00:00:00:01", "desktop_client_id": None}}}}


class TraceTest(unittest.TestCase):
    def test_valid_pair(self):
        self.assertEqual(CHECK.check_trace([hello(), response()]), {"device": 1, "server": 1, "unanswered": 0})

    def test_missing_direction_or_pair(self):
        for trace in ([], [hello()], [response()], [hello(), response("wrong")],
                      [hello(), hello()], [{"direction": "wrong", "packet": {}}], [hello(), response(), response()]):
            with self.subTest(trace=trace), self.assertRaises(ValueError):
                CHECK.check_trace(trace)

    def test_wrong_data_for_operation(self):
        with self.assertRaises(ValidationError):
            CHECK.check_trace([hello(), response(data={"items": [], "next_cursor": None})])

    def test_invalid_shape(self):
        bad = hello(); bad["packet"]["extra"] = True
        with self.assertRaises(ValidationError):
            CHECK.check_trace([bad, response()])

    def test_notification_and_cli(self):
        trace = [hello(), response(), {"direction": "server", "packet": {
            "id": "g2", "type": "device.bound", "payload": {
                "device_id": "34:00:00:00:00:01", "desktop_client_id": "desktop"}}}]
        self.assertEqual(CHECK.check_trace(trace)["server"], 2)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(trace))
            output = io.StringIO()
            with patch("sys.argv", [str(SPEC.origin), str(path)]), redirect_stdout(output):
                runpy.run_path(str(SPEC.origin), run_name="__main__")
            self.assertEqual(json.loads(output.getvalue())["unanswered"], 0)


if __name__ == "__main__":
    unittest.main()
