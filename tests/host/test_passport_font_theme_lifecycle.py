"""Regression: Passport dialogue labels must survive Assets theme/font refresh.

t74u bootloop evidence (6bb6c92 app-only on MAC c4:9e:7e:12:9a:7c):
  InstrFetchProhibited Core1 PC=0 after "Assets: Refreshing display theme..."
  decoded stack: lv_font_get_glyph_width → lv_text_* → lv_label_event → refr.

Upstream LvglDisplay::SetTextFont keeps the previous font shared_ptr alive only
until SetTheme returns, then destroys it. Stock LcdDisplay::SetTheme rebinds
screen/status widgets but not Passport stage labels. Labels that cached a raw
lv_font_t* from SetupUI then call through a freed font → PC=0.

This test is source-level (no device / no LVGL host). It fails if the safety
hooks regress while keeping bottom white dialogue features.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STOPWATCH = (
    ROOT
    / "official-core/main/boards/m5stack/stopwatch/m5stack_stopwatch.cc"
)
SET_TEXT_FONT = ROOT / "official-core/main/display/lvgl_display/lvgl_display.cc"


class PassportFontThemeLifecycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.stopwatch = STOPWATCH.read_text(encoding="utf-8")
        cls.set_text_font = SET_TEXT_FONT.read_text(encoding="utf-8")

    def test_set_text_font_keeps_previous_owner_until_after_set_theme(self) -> None:
        # Contract the Passport SetTheme override relies on.
        self.assertIn("previous_light_font", self.set_text_font)
        self.assertIn("previous_dark_font", self.set_text_font)
        set_theme_pos = self.set_text_font.find("SetTheme(current_theme_);")
        reset_pos = self.set_text_font.find("previous_light_font.reset();")
        self.assertGreater(set_theme_pos, 0)
        self.assertGreater(reset_pos, set_theme_pos)

    def test_passport_overrides_set_theme_and_rebinds_dialogue_fonts(self) -> None:
        self.assertRegex(
            self.stopwatch,
            r"void\s+SetTheme\s*\(\s*Theme\s*\*\s*theme\s*\)\s*override",
        )
        self.assertIn("ApplyPassportChromeLocked", self.stopwatch)
        chrome_idx = self.stopwatch.find("void ApplyPassportChromeLocked")
        self.assertGreater(chrome_idx, 0)
        # Look at a window covering the chrome helper (not the whole file).
        chrome = self.stopwatch[chrome_idx : chrome_idx + 3500]
        for label in ("clock_label_", "bottom_status_", "bottom_speech_"):
            self.assertIn(label, chrome)
            self.assertIn(f"lv_obj_set_style_text_font({label}", chrome)

    def test_bottom_dialogue_features_retained(self) -> None:
        # Do not "fix" the crash by dropping t72u features.
        self.assertIn("bottom_status_", self.stopwatch)
        self.assertIn("bottom_speech_", self.stopwatch)
        self.assertIn("LV_LABEL_LONG_WRAP", self.stopwatch)
        self.assertIn("ApplyStatusToPassport", self.stopwatch)
        self.assertRegex(
            self.stopwatch,
            r"void\s+SetChatMessage\s*\(.*\)\s*override",
        )

    def test_set_theme_calls_chrome_after_base(self) -> None:
        m = re.search(
            r"void\s+SetTheme\s*\(\s*Theme\s*\*\s*theme\s*\)\s*override\s*\{(?P<body>.*?)\n\s*\}",
            self.stopwatch,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(m, "Passport SetTheme override body not found")
        assert m is not None
        body = m.group("body")
        base_pos = body.find("LcdDisplay::SetTheme")
        chrome_pos = body.find("ApplyPassportChromeLocked")
        self.assertGreater(base_pos, 0)
        self.assertGreater(chrome_pos, base_pos)


if __name__ == "__main__":
    unittest.main()
