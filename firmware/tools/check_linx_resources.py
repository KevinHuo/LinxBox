#!/usr/bin/env python3
"""Dependency-free checks for fixed LinX strings and generated Flash resources."""

from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]
fonts = ROOT / "assets/fonts"
points = {int(line, 16) for line in (fonts / "codepoints.txt").read_text().splitlines()}
required = set(range(32, 127))
for source in (ROOT / "main").glob("*.[ch]"):
    required.update(ord(c) for c in source.read_text() if ord(c) >= 128 and not c.isspace())
missing = required - points
assert not missing, "Missing font inventory: " + ", ".join(f"U+{cp:04X}" for cp in sorted(missing))
assert 0x9F98 not in points, "Negative glyph control unexpectedly included"
manifest = json.loads((fonts / "manifest.json").read_text())
expected_variants = {
    "linx_font_12": (12, "Regular", False),
    "linx_font_14": (14, "Regular", False),
    "linx_font_16": (16, "Regular", False),
    "linx_font_20": (20, "Regular", False),
    "linx_font_16_bold": (16, "Bold", False),
    "linx_font_20_bold": (20, "Bold", False),
    "linx_font_24_bold": (24, "Bold", False),
    "linx_font_28_bold": (28, "Bold", True),
}
expected_hashes = {
    "Regular": "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b",
    "Bold": "b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a",
}
assert set(manifest["variants"]) == set(expected_variants), "Font variant inventory changed"
assert manifest["codepoint_count"] == len(points)
license_sha = hashlib.sha256((fonts / "OFL.txt").read_bytes()).hexdigest()
for weight, digest in expected_hashes.items():
    provenance = manifest["sources"][weight]
    assert hashlib.sha256((fonts / provenance["file"]).read_bytes()).hexdigest() == digest
    assert provenance["sha256"] == digest and provenance["license_sha256"] == license_sha
    assert provenance["url"].startswith("https://raw.githubusercontent.com/notofonts/noto-cjk/")
    assert provenance["weight_class"] == (700 if weight == "Bold" else 400)
for name, (size, weight, ascii_only) in expected_variants.items():
    variant = manifest["variants"][name]
    inventory = fonts / variant["inventory"]
    expected_points = set(range(32, 127)) if ascii_only else points
    variant_points = {int(value, 16) for value in inventory.read_text().splitlines()}
    assert variant_points == expected_points, f"{name}: inventory mismatch"
    assert hashlib.sha256(inventory.read_bytes()).hexdigest() == variant["inventory_sha256"]
    assert variant["size"] == size and variant["weight"] == weight
    assert variant["codepoint_count"] == len(expected_points)
    assert variant["weight_class"] == (700 if weight == "Bold" else 400)
    source = (fonts / f"{name}.c").read_text()
    values = re.search(r"unicode_list\[\] = \{([^}]+)\}", source).group(1)
    generated = [int(value) + min(expected_points) for value in values.split(",")]
    assert generated == sorted(expected_points), f"{name}: cmap/inventory mismatch"
    assert 0x9F98 not in generated
    assert f".range_start={min(expected_points)}, .range_length={max(expected_points) - min(expected_points) + 1}" in source
    assert f".list_length={len(expected_points)}," in source
    assert f"const lv_font_t {name} =" in source
    assert ".bitmap_format=0" in source
    assert f".line_height={variant['line_height']}, .base_line={variant['base_line']}" in source
    bitmap_block = source.split("glyph_bitmap[] = {", 1)[1].split("};", 1)[0]
    bitmap_bytes = len(re.findall(r"0x[0-9a-f]{2}", bitmap_block))
    key = name.removeprefix("linx_font_")
    assert bitmap_bytes == variant["bitmap_bytes"] == manifest["bitmap_bytes"][key]
    assert manifest["line_heights"][key] == variant["line_height"]
    descriptors = re.findall(
        r"\{\.bitmap_index=(\d+), \.adv_w=(\d+), \.box_w=(\d+), \.box_h=(\d+), "
        r"\.ofs_x=(-?\d+), \.ofs_y=(-?\d+)\}", source)
    assert len(descriptors) == len(expected_points), f"{name}: descriptor count"
    offset = ascent = descent = 0
    for descriptor in descriptors:
        index, advance, width, height, x, y = map(int, descriptor)
        assert index == offset, f"{name}: noncontiguous glyph bitmap"
        assert advance >= 0
        ascent, descent = max(ascent, height + y), max(descent, -y)
        offset += (width * height + 1) // 2
    assert offset == bitmap_bytes, f"{name}: bitmap length"
    assert (ascent, descent) == (variant["glyph_ascent"], variant["glyph_descent"])
    assert variant["base_line"] == descent
    assert variant["line_height"] == max(size + 4, ascent + descent)
    assert ascent <= variant["line_height"] - variant["base_line"]
    for cmake_path in (ROOT / "main/CMakeLists.txt", ROOT / "tests/linx_host/CMakeLists.txt"):
        assert f"/{name}.c\"" in cmake_path.read_text(), f"{name}: missing build registration"
    assert f"LV_FONT_DECLARE({name});" in (ROOT / "main/linx_assets.h").read_text()
assert manifest["image_bytes"] <= 640 * 1024
assert sum(manifest["bitmap_bytes"].values()) <= 1024 * 1024
image_source = (ROOT / "assets/images/linx_image_data.c").read_text()
assert image_source.count(".magic=LV_IMAGE_HEADER_MAGIC") == 9
app = (ROOT / "main/main.c").read_text()
for forbidden in ("bsp_audio_init(", "bsp_battery_init(", "esp_wifi_init(", "nimble_port_init("):
    assert forbidden not in app, f"Offline startup includes {forbidden}"
print(f"LinX resources: PASS ({len(points)} glyphs × 7 CJK variants; 95 ASCII × 28px bold; "
      "exact metrics/cmaps/source hashes; 9 RGB565 views; offline startup)")
