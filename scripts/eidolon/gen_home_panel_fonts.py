#!/usr/bin/env python3
"""Regenerate the korvo-1 smart home panel's fonts in main/eidolon/fonts/.

    python3 scripts/eidolon/gen_home_panel_fonts.py [--material PATH]

Outputs (LVGL, 4 bpp, uncompressed):
  font_home_icons_20.c  inline icons, the text font's fallback
  font_home_icons_24.c  tile discs, microphone, result card
  font_home_icons_48.c  system page
  font_home_text_20.c   Chinese the builtin text subset lacks (system page copy)
  font_home_clock_34.c  clock digits

Which icon goes into which size is read from main/eidolon/views/home_panel_icons.h
("// U+E37B  24 48"); the Chinese supplement is every character of
main/eidolon/views/home_panel_copy.{h,cc} that font_puhui_basic_20_4 cannot draw,
so the system page reads correctly before the assets partition's full text font
is loaded.

Sources, none downloaded:
  Material Icons  MaterialIcons-Regular.otf (Apache-2.0), shipped with Flutter at
                  <flutter>/bin/cache/artifacts/material_fonts/; --material or
                  MATERIAL_ICONS_FONT overrides the lookup.
  Text, digits    managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf
  Converter       lv_font_conv 1.5.3 via npx (LV_FONT_CONV overrides the command).
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ICONS_H = ROOT / "main/eidolon/views/home_panel_icons.h"
COPY_SOURCES = [ROOT / "main/eidolon/views/home_panel_copy.h", ROOT / "main/eidolon/views/home_panel_copy.cc"]
FONTS = ROOT / "managed_components/78__xiaozhi-fonts"
BASIC_FONT = FONTS / "src/font_puhui_basic_20_4.c"
PUHUI = FONTS / "ttf/puhui-common.ttf"
OUT = ROOT / "main/eidolon/fonts"

# Material icons sit on the baseline with their full em box above it; lowered
# by this much they share the optical centre of the 20 px CJK text they follow.
INLINE_ICON_DROP_PX = 3


def material_font(explicit):
    candidates = [explicit, os.environ.get("MATERIAL_ICONS_FONT")]
    flutter = shutil.which("flutter")
    if flutter:
        candidates.append(Path(flutter).resolve().parent / "cache/artifacts/material_fonts/MaterialIcons-Regular.otf")
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    sys.exit("MaterialIcons-Regular.otf not found: pass --material or set MATERIAL_ICONS_FONT "
             "(Flutter ships it in bin/cache/artifacts/material_fonts/)")


def icon_sizes():
    sizes = {}
    pattern = re.compile(r'#define HOME_ICON_\w+\s+"[^"]*"\s+//\s+U\+([0-9A-F]+)\s+([\d ]+)$')
    for line in ICONS_H.read_text().splitlines():
        match = pattern.search(line.strip())
        if not match:
            continue
        for size in match.group(2).split():
            sizes.setdefault(int(size), []).append(int(match.group(1), 16))
    if not sizes:
        sys.exit(f"no icons found in {ICONS_H}")
    return sizes


def builtin_code_points():
    """Code points font_puhui_basic_20_4 carries, read from its cmap table."""
    text = BASIC_FONT.read_text()
    lists = {name: [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body)]
             for name, body in re.findall(r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", text, re.S)}
    points = set()
    for start, length, ulist in re.findall(
            r"\.range_start = (\d+), \.range_length = (\d+).*?\.unicode_list = (\w+)", text, re.S):
        start, length = int(start), int(length)
        if ulist == "NULL":
            points.update(range(start, start + length))
        else:
            points.update(start + offset for offset in lists[ulist])
    return points


def copy_supplement():
    literals = []
    for source in COPY_SOURCES:
        literals += re.findall(r'"((?:[^"\\]|\\.)*)"', source.read_text())
    wanted = {ch for literal in literals for ch in literal if ord(ch) > 0x7F}
    have = builtin_code_points()
    return "".join(sorted(ch for ch in wanted if ord(ch) not in have))


MATERIAL_NOTICE = (" * Glyphs: Material Icons, Copyright Google LLC, licensed under the Apache License,\n"
                   " * Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0).\n")


def convert(converter, name, size, args, notice=""):
    out = OUT / f"{name}.c"
    cmd = converter + ["--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
                       *args, "-o", str(out), "--lv-include", "lvgl.h"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    text = out.read_text()
    # Keep the recorded options free of this machine's paths.
    text = re.sub(r"--font \S+/([^/\s]+)", r"--font \1", text)
    text = re.sub(r"-o \S+/([^/\s]+\.c)", r"-o \1", text)
    if notice:
        text = text.replace(" * Opts: ", notice + " * Opts: ", 1)
    out.write_text(text)
    return out


def drop_glyphs(path, pixels):
    text = path.read_text()
    start = text.index("glyph_dsc[] = {")
    end = text.index("};", start)
    body = "\n".join(line if "reserved" in line else
                     re.sub(r"\.ofs_y = (-?\d+)\}", lambda m: f".ofs_y = {int(m.group(1)) - pixels}}}", line)
                     for line in text[start:end].split("\n"))
    text = text[:start] + body + text[end:]
    text = text.replace(" * Opts: ", f" * Post-processed: every glyph lowered by {pixels} px (gen_home_panel_fonts.py).\n"
                        " * Opts: ", 1)
    path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--material", help="path to MaterialIcons-Regular.otf")
    options = parser.parse_args()
    material = material_font(options.material)
    converter = shlex.split(os.environ.get("LV_FONT_CONV", "npx --yes lv_font_conv@1.5.3"))
    OUT.mkdir(parents=True, exist_ok=True)

    for size, points in sorted(icon_sizes().items()):
        ranges = ",".join(hex(p) for p in sorted(set(points)))
        path = convert(converter, f"font_home_icons_{size}", size, ["--font", str(material), "--range", ranges],
                       MATERIAL_NOTICE)
        if size == 20:
            drop_glyphs(path, INLINE_ICON_DROP_PX)
        print(f"{path.relative_to(ROOT)}: {len(set(points))} icons")

    supplement = copy_supplement()
    if supplement:
        path = convert(converter, "font_home_text_20", 20, ["--font", str(PUHUI), "--symbols", supplement])
        print(f"{path.relative_to(ROOT)}: {len(supplement)} characters")
    path = convert(converter, "font_home_clock_34", 34, ["--font", str(PUHUI), "--symbols", "0123456789:-"])
    print(f"{path.relative_to(ROOT)}: clock digits")


if __name__ == "__main__":
    main()
