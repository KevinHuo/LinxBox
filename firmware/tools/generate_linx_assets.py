#!/usr/bin/env python3
"""Generate plain LVGL fonts and original, small RGB565 UI illustrations."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "assets/fonts"
IMAGES = ROOT / "assets/images"
SOURCE = FONTS / "NotoSansCJKsc-Regular.otf"
INVENTORY = FONTS / "codepoints.txt"
ASCII_INVENTORY = FONTS / "codepoints-ascii.txt"
FONT_SOURCES = {
    "Regular": {
        "path": SOURCE,
        "sha256": "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b",
        "weight": 400,
    },
    "Bold": {
        "path": FONTS / "NotoSansCJKsc-Bold.otf",
        "sha256": "b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a",
        "weight": 700,
    },
}
# The large size is for ASCII counters only; all other variants share UI coverage.
VARIANTS = ((12, "Regular", False), (14, "Regular", False), (16, "Regular", False),
            (20, "Regular", False), (16, "Bold", False), (20, "Bold", False),
            (24, "Bold", False),
            (28, "Bold", True))
PAPER = "#F7F2E8"
INK = "#292722"
MUTED = "#746B5F"
ORANGE = "#AD391C"
TINT = "#F8E0CF"

ILLUSTRATION_TEXT = (
    "LinX 演示 78% 我的任务 工作电脑·模拟在线 登录校验修复 Codex·运行中 "
    "支付接口调整 Claude Code·等你回复 LinX暖白界面 有新回复 ↑↓选择 OK进入 长按OK返回 "
    "本轮回复 登录校验已修复 已补充边界测试 一项待验证 请确认提示文案 阅读完整回复 "
    "继续沟通 语音确认 模拟输入 不采集声音 把异常提示改得更清楚，先不要发布。 "
    "发送 重录 取消 本轮结束 修改与验证 结果阅读 任务列表"
)


def required_points() -> list[int]:
    text = ILLUSTRATION_TEXT
    for path in sorted((ROOT / "main").glob("*.[ch]")):
        # 设备运行和手机网页不在设备屏幕绘制，无须进入屏幕字库。
        if path.stem in ("linx_device", "linx_wifi_portal"):
            continue
        text += path.read_text(encoding="utf-8")
    points = set(range(32, 127))
    points.update(ord(ch) for ch in text if ord(ch) >= 128 and not ch.isspace())
    # Keep prior inventory so incremental resource generation cannot drop glyphs.
    if INVENTORY.exists():
        points.update(int(line, 16) for line in INVENTORY.read_text().splitlines())
    return sorted(points)


def byte_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[] = {{"]
    lines.extend("    " + ",".join(f"0x{x:02x}" for x in data[i:i + 20]) + ","
                 for i in range(0, len(data), 20))
    return "\n".join(lines) + "\n};\n"


def raster(font: ImageFont.FreeTypeFont, cp: int) -> tuple[bytes, tuple[int, ...]]:
    ch = chr(cp)
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    width, height = right - left, bottom - top
    if not width or not height:
        return b"", (round(font.getlength(ch) * 16), 0, 0, 0, 0)
    image = Image.new("L", (width, height))
    ImageDraw.Draw(image).text((-left, -top), ch, font=font, fill=255, anchor="ls")
    pixels = bytes((value + 8) // 17 for value in image.tobytes())
    return pixels, (round(font.getlength(ch) * 16), width, height, left, -bottom)


def glyph(font: ImageFont.FreeTypeFont, cp: int) -> tuple[bytes, tuple[int, ...]]:
    pixels, descriptor = raster(font, cp)
    if len(pixels) % 2:
        pixels += b"\0"
    packed = bytes((pixels[i] << 4) | pixels[i + 1] for i in range(0, len(pixels), 2))
    return packed, descriptor


def variant_name(size: int, weight: str) -> str:
    return f"linx_font_{size}" + ("_bold" if weight == "Bold" else "")


def font_source(size: int, weight: str, points: list[int]) -> tuple[str, dict]:
    font = ImageFont.truetype(str(FONT_SOURCES[weight]["path"]), size)
    bitmap = bytearray()
    descriptors = ["    {0},"]
    ascent = descent = 0
    for cp in points:
        packed, (advance, width, height, x, y) = glyph(font, cp)
        ascent = max(ascent, height + y)
        descent = max(descent, -y)
        descriptors.append(f"    {{.bitmap_index={len(bitmap)}, .adv_w={advance}, "
                           f".box_w={width}, .box_h={height}, .ofs_x={x}, .ofs_y={y}}},")
        bitmap.extend(packed)
    offsets = ",".join(str(cp - points[0]) for cp in points)
    name = variant_name(size, weight)
    line_height = max(size + 4, ascent + descent)
    source = (f"/* Noto Sans CJK SC {weight}; SIL Open Font License 1.1 (OFL.txt). */\n"
              '#include "lvgl.h"\n\n') + byte_array("glyph_bitmap", bitmap)
    source += "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n"
    source += "\n".join(descriptors) + "\n};\n"
    source += f"static const uint16_t unicode_list[] = {{{offsets}}};\n"
    source += (
        "static const lv_font_fmt_txt_cmap_t cmaps[] = {{\n"
        f"    .range_start={points[0]}, .range_length={points[-1] - points[0] + 1},\n"
        f"    .glyph_id_start=1, .unicode_list=unicode_list, .list_length={len(points)},\n"
        "    .type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n}};\n"
        "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
        "    .glyph_bitmap=glyph_bitmap, .glyph_dsc=glyph_dsc, .cmaps=cmaps,\n"
        "    .cmap_num=1, .bpp=4, .bitmap_format=0\n};\n"
        f"const lv_font_t {name} = {{\n"
        "    .get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,\n"
        "    .get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,\n"
        f"    .line_height={line_height}, .base_line={descent}, .dsc=&font_dsc,\n"
        "    .underline_position=-2, .underline_thickness=1\n};\n"
    )
    return source, {"size": size, "weight": weight, "weight_class": FONT_SOURCES[weight]["weight"],
                    "codepoint_count": len(points), "bitmap_bytes": len(bitmap),
                    "line_height": line_height, "base_line": descent,
                    "glyph_ascent": ascent, "glyph_descent": descent}


def verify_font(path: Path, size: int, weight: str, points: list[int]) -> None:
    """Decode emitted glyphs/descriptors and compare to independent raster pixels."""
    source = path.read_text()
    block = source.split("glyph_bitmap[] = {", 1)[1].split("};", 1)[0]
    emitted = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", block))
    descriptors = re.findall(
        r"\{\.bitmap_index=(\d+), \.adv_w=(\d+), \.box_w=(\d+), \.box_h=(\d+), "
        r"\.ofs_x=(-?\d+), \.ofs_y=(-?\d+)\}", source)
    assert len(descriptors) == len(points), f"{path.name}: descriptor count"
    font = ImageFont.truetype(str(FONT_SOURCES[weight]["path"]), size)
    offset = 0
    for cp, descriptor in zip(points, descriptors):
        index, advance, width, height, x, y = map(int, descriptor)
        pixels, expected_dsc = raster(font, cp)
        assert index == offset, f"{path.name}: U+{cp:04X} bitmap offset"
        assert (advance, width, height, x, y) == expected_dsc, f"{path.name}: U+{cp:04X} metrics"
        count = (width * height + 1) // 2
        packed = emitted[index:index + count]
        decoded = bytes(value for byte in packed for value in (byte >> 4, byte & 15))
        assert decoded[:len(pixels)] == pixels, f"{path.name}: U+{cp:04X} decoded pixels"
        assert not decoded[len(pixels):] or decoded[len(pixels):] == b"\0", "Nonzero pad nibble"
        offset += count
    assert offset == len(emitted), f"{path.name}: trailing bitmap bytes"


def illustration(kind: int) -> Image.Image:
    image = Image.new("RGB", (240, 320), PAPER)
    draw = ImageDraw.Draw(image)
    fonts = {size: ImageFont.truetype(str(SOURCE), size) for size in (14, 16, 20)}

    def text(x: int, y: int, value: str, size: int = 16, color: str = INK) -> None:
        draw.text((x, y), value, font=fonts[size], fill=color, anchor="lt")

    text(28, 16, "Lin", 20)
    text(58, 16, "X", 20, ORANGE)
    text(142, 19, "演示", 14, MUTED)
    text(185, 19, "78%", 14, MUTED)
    if kind == 1:
        text(16, 52, "我的任务", 20)
        text(16, 80, "工作电脑·模拟在线", 14, MUTED)
        rows = [("登录校验修复", "Codex·运行中"),
                ("支付接口调整", "Claude Code·等你回复"),
                ("LinX 暖白界面", "Codex·有新回复")]
        for i, (title, caption) in enumerate(rows):
            y = 104 + i * 54
            if i == 0:
                draw.rounded_rectangle((16, y, 224, y + 48), radius=5, fill=TINT)
                draw.rectangle((16, y + 4, 18, y + 44), fill=ORANGE)
            text(26, y + 5, title)
            text(26, y + 29, caption, 14, MUTED)
    elif kind == 2:
        text(16, 52, "本轮回复", 20)
        text(16, 80, "Codex·登录校验修复", 14, MUTED)
        for i, line in enumerate(["登录校验已修复", "已补充边界测试", "一项待验证", "请确认提示文案"]):
            text(20, 110 + i * 29, line, 16, ORANGE if i == 2 else INK)
        draw.rounded_rectangle((16, 234, 224, 262), radius=5, fill=TINT)
        text(24, 238, "阅读完整回复", 16, ORANGE)
    else:
        text(16, 52, "语音确认", 20)
        text(16, 80, "模拟输入·不采集声音", 14, MUTED)
        draw.rounded_rectangle((16, 108, 224, 204), radius=7, fill=TINT)
        for i, line in enumerate(["把异常提示改得更清楚，", "先不要发布。"]):
            text(24, 120 + i * 24, line, 16)
        text(24, 230, "发送    重录    取消", 16, ORANGE)
    draw.line((28, 272, 212, 272), fill="#D8D0C3")
    text(28, 281, "↑↓选择  OK进入", 14, MUTED)
    text(28, 301, "长按OK返回", 14, MUTED)
    return image


def rgb565(image: Image.Image) -> bytes:
    data = bytearray()
    for red, green, blue in image.getdata():
        value = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        data.extend((value & 255, value >> 8))
    return bytes(data)


def image_source() -> tuple[str, int]:
    source = '#include "linx_assets.h"\n\n'
    entries = []
    total = 0
    for image_id in range(1, 4):
        full = illustration(image_id)
        views = [full.resize((126, 168), Image.Resampling.LANCZOS)]
        # The midpoint lies between text rows; pad rather than slice a glyph.
        for top in (0, 160):
            detail = Image.new("RGB", (208, 168), PAPER)
            detail.paste(full.crop((16, top, 224, top + 160)), (0, 4))
            views.append(detail)
        for view, image in enumerate(views):
            name = f"image_{image_id}_{view}"
            data = rgb565(image)
            total += len(data)
            source += byte_array(name, data)
            entries.append(
                f"    {{.header={{.magic=LV_IMAGE_HEADER_MAGIC, .cf=LV_COLOR_FORMAT_RGB565, "
                f".w={image.width}, .h={image.height}, .stride={image.width * 2}}}, "
                f".data_size={len(data)}, .data={name}}},")
    source += "static const lv_image_dsc_t images[] = {\n" + "\n".join(entries) + "\n};\n"
    source += (
        "const lv_image_dsc_t *linx_asset_image(unsigned attachment_id, unsigned view)\n{\n"
        "#ifdef LINX_TEST_MISSING_IMAGE\n"
        "    if (attachment_id == 2) return NULL;\n"
        "#endif\n"
        "    if (attachment_id < 1 || attachment_id > 3 || view > 2) return NULL;\n"
        "    return &images[(attachment_id - 1) * 3 + view];\n}\n"
    )
    return source, total


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    points = required_points()
    source_records = {}
    license_path = FONTS / "OFL.txt"
    license_sha = hashlib.sha256(license_path.read_bytes()).hexdigest()
    for weight, specification in FONT_SOURCES.items():
        path = specification["path"]
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assert digest == specification["sha256"], f"Unexpected {weight} source hash"
        with TTFont(path) as source_font:
            supported = set(source_font.getBestCmap())
            assert source_font["OS/2"].usWeightClass == specification["weight"]
            assert "SIL Open Font License" in source_font["name"].getDebugName(13)
            missing = sorted(set(points) - supported)
            if missing:
                raise SystemExit(f"{weight} source font lacks: " + ", ".join(f"U+{cp:04X}" for cp in missing))
        source_records[weight] = {
            "file": path.name, "sha256": digest, "weight_class": specification["weight"],
            "url": "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/"
                   f"SimplifiedChinese/{path.name}",
            "license_file": license_path.name, "license_sha256": license_sha,
            "license_url": "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE",
        }
    assert 0x9F98 not in points, "Known-missing negative control must remain outside subset"
    if points[-1] - points[0] > 65534:
        raise SystemExit("Font cmap exceeds the 16-bit sparse range")
    ascii_points = list(range(32, 127))
    outputs = {INVENTORY: "".join(f"{cp:04X}\n" for cp in points),
               ASCII_INVENTORY: "".join(f"{cp:04X}\n" for cp in ascii_points)}
    sizes = {}
    variants = {}
    for size, weight, ascii_only in VARIANTS:
        name = variant_name(size, weight)
        inventory = ASCII_INVENTORY if ascii_only else INVENTORY
        outputs[FONTS / f"{name}.c"], variants[name] = font_source(
            size, weight, ascii_points if ascii_only else points)
        variants[name]["inventory"] = inventory.name
        variants[name]["inventory_sha256"] = hashlib.sha256(outputs[inventory].encode()).hexdigest()
        key = str(size) + ("_bold" if weight == "Bold" else "")
        sizes[key] = variants[name]["bitmap_bytes"]
    outputs[IMAGES / "linx_image_data.c"], image_bytes = image_source()
    manifest = {
        "font": "Noto Sans CJK SC Regular and Bold", "license": "SIL Open Font License 1.1",
        "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "sources": source_records, "variants": variants,
        "rasterizer": "Pillow 11.3.0", "fonttools": "4.60.1", "codepoint_count": len(points),
        "format": "LVGL plain 4bpp, high nibble first, byte-aligned glyphs",
        "bitmap_bytes": sizes,
        "line_heights": {name.removeprefix("linx_font_"): info["line_height"]
                         for name, info in variants.items()},
        "image_format": "RGB565 little endian", "image_bytes": image_bytes,
        "image_source": "Original LinX UI illustrations drawn by this generator",
    }
    assert image_bytes <= 640 * 1024
    assert sum(sizes.values()) <= 1024 * 1024
    outputs[FONTS / "manifest.json"] = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    for path, text in outputs.items():
        if args.check:
            if not path.exists() or path.read_text() != text:
                raise SystemExit(f"Generated resource is stale: {path.relative_to(ROOT)}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
    for size, weight, ascii_only in VARIANTS:
        verify_font(FONTS / f"{variant_name(size, weight)}.c", size, weight,
                    ascii_points if ascii_only else points)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    print("LinX generated assets: PASS" + (" (current source coverage and exact bitmap check)" if args.check else ""))


if __name__ == "__main__":
    main()
