"""Keep the tested input/UI pump ordering connected to the device entry point."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def validate_contract(source: str) -> None:
    load = source.index("linx_input_settings_init(&loaded_mode)")
    initial_lock = source.index("if (!bsp_lvgl_lock(1000))")
    ui_init = source.index("linx_ui_init(lv_screen_active()")
    initial_mode = source.index("linx_ui_set_back_mode(loaded_mode);")
    bind = source.index("linx_ui_set_back_mode_handler(linx_input_settings_request, NULL);")
    assert load < initial_lock < ui_init < initial_mode < bind, "NVS load must run outside the UI lock before binding settings"
    loop = source[source.index("for (;;) {"):]
    action = loop.index("linx_input_action_t action =")
    lock = loop.index("if (!bsp_lvgl_lock(100)) continue;")
    reduce = loop.index("action = input_action(&input, &event);")
    before = loop.index("const linx_ui_page_t before_tick = linx_ui_page();")
    poll = loop.index("linx_input_settings_poll(&settings_event)")
    result = loop.index("linx_ui_back_mode_result(settings_event.mode, settings_event.saved);")
    tick = loop.index("linx_ui_tick(now_ms);")
    mode_change = re.search(
        r"if\s*\(input.back_mode\s*!=\s*linx_ui_back_mode\(\)\)\s*\{\s*"
        r"linx_input_set_back_mode\(&input,\s*linx_ui_back_mode\(\),\s*\(uint64_t\)esp_timer_get_time\(\) / 1000\);\s*"
        r"action\s*=\s*LINX_INPUT_NONE;\s*\}", loop)
    assert mode_change is not None, "Same-page mode results must discard action and held gesture"
    overflow = loop[loop.index("if (atomic_exchange_explicit"):lock]
    assert "linx_input_init(" not in overflow, "Overflow must preserve the selected return mode"
    assert "linx_input_barrier(&input," in overflow, "Overflow must consume the held gesture"
    automatic = re.search(
        r"if\s*\(linx_ui_page\(\)\s*!=\s*before_tick\)\s*\{\s*"
        r"linx_input_barrier\(&input,\s*now_ms\);\s*"
        r"action\s*=\s*LINX_INPUT_NONE;\s*\}", loop)
    assert automatic is not None, "Automatic page changes must discard action and held gesture"
    deferred = re.search(
        r"else\s+if\s*\(!received\s*&&\s*uxQueueMessagesWaiting\(s_input_queue\)\s*==\s*0U\)\s*\{\s*(?:/\*.*?\*/\s*)?"
        r"action\s*=\s*linx_input_tick\(&input,\s*now_ms\);", loop, re.S)
    assert deferred is not None, "Deferred clicks must wait for an empty queue and unchanged page"
    dispatch_guard = loop.index("if (action != LINX_INPUT_NONE)")
    previous = loop.index("const linx_ui_page_t previous = linx_ui_page();")
    dispatch = loop.index("linx_ui_handle_key(ui_key(action));")
    manual = re.search(
        r"if\s*\(linx_ui_page\(\)\s*!=\s*previous\)\s*\{\s*"
        r"(?:/\*.*?\*/\s*)?linx_input_barrier\(&input,", loop, re.S)
    assert manual is not None, "Manual page changes must invalidate queued gestures"
    manual_mode = re.search(
        r"if\s*\(input.back_mode\s*!=\s*linx_ui_back_mode\(\)\)\s*\{\s*"
        r"linx_input_set_back_mode\(&input,\s*linx_ui_back_mode\(\),\s*\(uint64_t\)esp_timer_get_time\(\) / 1000\);", loop[dispatch:])
    assert manual_mode is not None, "Mode changes during dispatch must invalidate queued gestures"
    assert action < lock < reduce < before < poll < result < tick < mode_change.start() < mode_change.end() < automatic.start() < automatic.end() < deferred.start() < deferred.end() < dispatch_guard < previous < dispatch < dispatch + manual_mode.start() < dispatch + manual_mode.end() < manual.start()


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
            source.replace("!received && uxQueueMessagesWaiting(s_input_queue) == 0U", "!received", 1),
            source.replace("action = linx_input_tick(&input, now_ms);", "", 1),
            source.replace("linx_ui_set_back_mode_handler(linx_input_settings_request, NULL);", "", 1),
            source.replace("linx_input_settings_init(&loaded_mode)", "load_settings_without_worker()", 1),
            source.replace("linx_ui_back_mode_result(settings_event.mode, settings_event.saved);", "", 1),
            source.replace("linx_input_set_back_mode(&input, linx_ui_back_mode(), (uint64_t)esp_timer_get_time() / 1000);", "", 1),
            source.replace("linx_input_set_back_mode(&input, linx_ui_back_mode(), (uint64_t)esp_timer_get_time() / 1000);",
                           "linx_input_set_back_mode(&input, linx_ui_back_mode(), now_ms);", 1),
            source.replace("linx_input_barrier(&input, (uint64_t)esp_timer_get_time() / 1000);\n            xQueueReset", "linx_input_init(&input, DOUBLE_CLICK_MS);\n            xQueueReset", 1),
        ):
            with self.assertRaises((AssertionError, ValueError)):
                validate_contract(stale)


if __name__ == "__main__":
    unittest.main()
