"""Keep the tested input/UI pump ordering connected to the device entry point."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def validate_contract(source: str) -> None:
    loop = source[source.index("for (;;) {"):]
    action = loop.index("linx_input_action_t action =")
    before = loop.index("const linx_ui_page_t before_tick = linx_ui_page();")
    tick = loop.index("linx_ui_tick(now_ms);")
    automatic = re.search(
        r"if\s*\(linx_ui_page\(\)\s*!=\s*before_tick\)\s*\{\s*"
        r"linx_input_barrier\(&input,\s*now_ms\);\s*"
        r"action\s*=\s*LINX_INPUT_NONE;\s*\}", loop)
    assert automatic is not None, "Automatic page changes must discard action and held gesture"
    dispatch_guard = loop.index("if (action != LINX_INPUT_NONE)")
    previous = loop.index("const linx_ui_page_t previous = linx_ui_page();")
    dispatch = loop.index("linx_ui_handle_key(ui_key(action));")
    manual = re.search(
        r"if\s*\(linx_ui_page\(\)\s*!=\s*previous\)\s*\{\s*"
        r"(?:/\*.*?\*/\s*)?linx_input_barrier\(&input,", loop, re.S)
    assert manual is not None, "Manual page changes must invalidate queued gestures"
    assert action < before < tick < automatic.start() < automatic.end() < dispatch_guard < previous < dispatch < manual.start()


class MainPumpContract(unittest.TestCase):
    def test_device_entrypoint_uses_tested_order(self):
        validate_contract((ROOT / "main/main.c").read_text())

    def test_negative_controls_detect_missing_barrier_and_discard(self):
        source = (ROOT / "main/main.c").read_text()
        for stale in (
            source.replace("linx_input_barrier(&input, now_ms);", "", 1),
            source.replace("linx_input_barrier(&input, now_ms);\n            action = LINX_INPUT_NONE;",
                           "linx_input_barrier(&input, now_ms);", 1),
            source.replace("linx_ui_handle_key(ui_key(action));", "/* dispatch removed */", 1),
        ):
            with self.assertRaises((AssertionError, ValueError)):
                validate_contract(stale)


if __name__ == "__main__":
    unittest.main()
