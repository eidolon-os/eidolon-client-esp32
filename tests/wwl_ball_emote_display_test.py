"""Exercise the real WWL Ball emote-display contract without a firmware build.

The WWL Ball S3 LCD 1.85 board runs the emote::EmoteDisplay renderer. These
tests pin the script, board metadata, Kconfig allowlist, CMake wiring and asset
manifests that make an emote build real, and — when the managed
esp_emote_assets component is present — run its actual packer to prove the
expression assets fit the board's assets partition.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/eidolon/eidolon-wwl-ball-s3-lcd-1.85.sh"
BOARD_CONFIG = ROOT / "main/boards/wwl-ball-s3-lcd-1.85/config.json"
BOARD_ASSETS = ROOT / "main/boards/wwl-ball-s3-lcd-1.85/assets"
RESOLUTION_DIR = BOARD_ASSETS / "360_360"
KCONFIG = ROOT / "main/Kconfig.projbuild"
CMAKE = ROOT / "main/CMakeLists.txt"
EMOTE_ASSETS = ROOT / "managed_components/espressif2022__esp_emote_assets"
BOARD_SOURCE = ROOT / "main/boards/wwl-ball-s3-lcd-1.85/wwl_ball_s3_lcd_1_85.cc"

# partitions/v2/16m_eidolon.csv: assets, data, spiffs, 0x8C0000, 0x740000
ASSETS_PARTITION_SIZE = 0x740000

# LVGL 9 bitmap fonts store a raw lv_font_t header. On a 32-bit build the three
# function pointers (get_glyph_dsc, get_glyph_bitmap, release_glyph) occupy the
# first 12 bytes; the int32_t line_height immediately follows. See
# managed_components/lvgl__lvgl/src/font/lv_font.h (struct _lv_font_t).
FONT_LINE_HEIGHT_OFFSET = 12
FONT_LINE_HEIGHT_SIZE = 4


def _layout_entry(name):
    entries = json.loads((RESOLUTION_DIR / "layout.json").read_text())
    for entry in entries:
        if entry.get("name") == name:
            return entry
    raise AssertionError(f"layout.json entry not found: {name}")


def _read_font_line_height(font_bin):
    """Read the int32 line_height from an LVGL 9 bitmap font header.

    Only the bounded header region is read; no field other than line_height is
    decoded, so a font swap that keeps line_height intact cannot fail this.
    """
    with open(font_bin, "rb") as handle:
        header = handle.read(FONT_LINE_HEIGHT_OFFSET + FONT_LINE_HEIGHT_SIZE)
    if len(header) < FONT_LINE_HEIGHT_OFFSET + FONT_LINE_HEIGHT_SIZE:
        raise ValueError(f"font header shorter than expected: {font_bin}")
    return int.from_bytes(
        header[FONT_LINE_HEIGHT_OFFSET:FONT_LINE_HEIGHT_OFFSET + FONT_LINE_HEIGHT_SIZE],
        "little")


class WwlBallEmoteDisplayTest(unittest.TestCase):
    def configure(self):
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            command = '''source "$1"
BUILD_DIR="$2"
SDKCONFIG_FILE="$2/sdkconfig"
SDKCONFIG_OVERLAY="$2/overlay"
write_overlay
ensure_wwl_sdkconfig
'''
            subprocess.run(["bash", "-eu", "-c", command, "test", str(SCRIPT), directory],
                           env=env, check=True, capture_output=True)
            return (Path(directory) / "sdkconfig").read_text()

    def test_script_selects_emote_style_and_expression_assets(self):
        text = self.configure()
        self.assertIn("CONFIG_USE_EMOTE_MESSAGE_STYLE=y", text)
        self.assertNotIn("CONFIG_USE_DEFAULT_MESSAGE_STYLE=y", text)
        self.assertIn("CONFIG_FLASH_EXPRESSION_ASSETS=y", text)
        for key in ("FLASH_DEFAULT_ASSETS", "FLASH_CUSTOM_ASSETS", "FLASH_NONE_ASSETS"):
            self.assertNotIn(f"CONFIG_{key}=y", text)

    def test_board_metadata_selects_emote_style_and_expression_assets(self):
        metadata = json.loads(BOARD_CONFIG.read_text())
        text = str(metadata)
        self.assertIn("CONFIG_USE_EMOTE_MESSAGE_STYLE=y", text)
        self.assertIn("CONFIG_USE_DEFAULT_MESSAGE_STYLE=n", text)
        self.assertIn("CONFIG_FLASH_EXPRESSION_ASSETS=y", text)
        for key in ("FLASH_DEFAULT_ASSETS", "FLASH_CUSTOM_ASSETS", "FLASH_NONE_ASSETS"):
            self.assertIn(f"CONFIG_{key}=n", text)

    def test_kconfig_emote_style_allowlist_includes_wwl_ball(self):
        text = KCONFIG.read_text()
        block = re.search(r"config USE_EMOTE_MESSAGE_STYLE\b(.*?)endchoice", text, re.DOTALL)
        self.assertIsNotNone(block, "USE_EMOTE_MESSAGE_STYLE block not found")
        self.assertIn("BOARD_TYPE_WWL_BALL_S3_LCD_1_85", block.group(1))

    def test_cmake_wwl_block_wires_360_360_external_assets(self):
        text = CMAKE.read_text()
        block = re.search(
            r"elseif\(CONFIG_BOARD_TYPE_WWL_BALL_S3_LCD_1_85\)(.*?)(?=elseif\(|endif\(\))",
            text, re.DOTALL)
        self.assertIsNotNone(block, "WWL Ball CMake board block not found")
        self.assertIn('set(EMOTE_RESOLUTION "360_360")', block.group(1))
        self.assertIn("EMOTE_EXTERNAL_PATH", block.group(1))
        self.assertIn("boards/wwl-ball-s3-lcd-1.85/assets", block.group(1))

    def test_board_assets_manifests_exist_and_parse(self):
        for name in ("config.json", "emote.json"):
            self.assertTrue((RESOLUTION_DIR / name).is_file(),
                            f"missing board asset manifest: {RESOLUTION_DIR / name}")
        config = json.loads((RESOLUTION_DIR / "config.json").read_text())
        self.assertEqual(config.get("emoji_collection"), "emoji_small")
        emotes = json.loads((RESOLUTION_DIR / "emote.json").read_text())
        self.assertIsInstance(emotes, list)
        self.assertTrue(emotes, "board emote manifest is empty")
        for entry in emotes:
            self.assertIn("src", entry, f"emote entry missing src: {entry}")
            self.assertTrue(entry["src"], f"emote entry has empty src: {entry}")

    def test_emote_sources_reference_configured_eaf_files(self):
        emotes = json.loads((RESOLUTION_DIR / "emote.json").read_text())
        collection = json.loads((RESOLUTION_DIR / "config.json").read_text())["emoji_collection"]
        emoji_collection = EMOTE_ASSETS / collection
        for entry in emotes:
            src = entry["src"]
            self.assertTrue((emoji_collection / src).is_file(),
                            f"emote '{entry['emote']}' references missing EAF {src}")

    def test_layout_eye_anim_and_emerg_dlg_use_upper_face_position(self):
        for name in ("eye_anim", "emerg_dlg"):
            entry = _layout_entry(name)
            self.assertEqual(entry["x"], 95, f"{name} x should center the small face pair")
            self.assertEqual(entry["y"], -92, f"{name} y should place the face above the chrome")
            self.assertEqual(entry["align"], "GFX_ALIGN_LEFT_MID", f"{name} align")
            self.assertEqual(entry["anim"]["mirror"], "auto", f"{name} mirror")

    def test_layout_toast_label_position(self):
        toast = _layout_entry("toast_label")
        self.assertEqual(toast["y"], 90)
        self.assertEqual(toast["x"], 0)

    def test_font_puhui_common_20_4_line_height(self):
        config = json.loads((RESOLUTION_DIR / "config.json").read_text())
        font_bin = EMOTE_ASSETS / "font" / f"{config['text_font']}.bin"
        self.assertTrue(font_bin.is_file(), f"missing font binary: {font_bin}")
        self.assertEqual(_read_font_line_height(font_bin), 25)

    def test_board_applies_chrome_layout_deltas(self):
        source = BOARD_SOURCE.read_text()
        self.assertRegex(
            source,
            r"SetChromeLayoutDeltas\s*\(\s*\{\s*128\s*,\s*-50\s*,\s*148\s*\}",
            "WWL board must place the chrome below the upper face and preserve the caption position")

    def test_upper_chrome_clears_face_and_bottom_caption_stays_put(self):
        eye = _layout_entry("eye_anim")
        source = BOARD_SOURCE.read_text()
        deltas = re.search(
            r"SetChromeLayoutDeltas\s*\(\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", source)
        self.assertIsNotNone(deltas)
        top_delta, bottom_delta = (int(value) for value in deltas.groups())
        face_top = (360 - 80) // 2 + eye["y"]
        face_bottom = face_top + 80
        mode_top = 8 + top_delta
        state_bottom = 34 + top_delta + 28
        caption_top = 360 - 26 - 4 + bottom_delta
        self.assertLessEqual(face_bottom, mode_top)
        self.assertLessEqual(state_bottom, caption_top)
        self.assertEqual(caption_top, 280)

    def test_state_label_moves_below_caption_with_default_compatibility(self):
        source = BOARD_SOURCE.read_text()
        deltas = re.search(
            r"SetChromeLayoutDeltas\s*\(\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
            source)
        self.assertIsNotNone(deltas)
        top_delta, bottom_delta, state_delta = (int(value) for value in deltas.groups())
        caption_top = 360 - 26 - 4 + bottom_delta
        state_top = 34 + top_delta + state_delta
        self.assertGreaterEqual(state_top, caption_top + 26)
        self.assertLessEqual(state_top + 28, 360)
        self.assertIn("int16_t state_y = 0;", (ROOT / "main/display/emote_display.h").read_text())
        self.assertIn("chrome_layout_deltas_.state_y", (ROOT / "main/display/emote_display.cc").read_text())

    def test_packer_output_fits_assets_partition(self):
        build_all = EMOTE_ASSETS / "scripts/spiffs_assets/build_all.py"
        if not build_all.is_file():
            self.skipTest("esp_emote_assets component not present")
        with tempfile.TemporaryDirectory() as directory:
            output_bin = Path(directory) / "expression_assets.bin"
            subprocess.run(
                [sys.executable, str(build_all),
                 "--resolution", "360_360",
                 "--output", str(output_bin),
                 "--name_length", "32",
                 "--external_path", str(BOARD_ASSETS)],
                check=True, capture_output=True, text=True)
            self.assertTrue(output_bin.is_file(), "packer produced no output")
            self.assertLessEqual(output_bin.stat().st_size, ASSETS_PARTITION_SIZE)


if __name__ == "__main__":
    unittest.main()
