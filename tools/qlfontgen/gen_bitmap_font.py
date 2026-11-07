#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 TTF 生成 QLGL 内置位图字体（C 源文件）。

用法：
    python3 tools/qlfontgen/gen_bitmap_font.py
    python3 tools/qlfontgen/gen_bitmap_font.py --check   # CI：校验生成物是否最新

输出 src/text/qlfont_ascii.c，内容为 4bpp 抗锯齿字形的 ASCII(32..126) 字体。
字体来源：DejaVu Sans Mono（Bitstream Vera 许可，允许嵌入与再分发）。
"""

import argparse
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "src" / "text" / "qlfont_ascii.c"

TTF_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
]

# (C 标识符, 像素字号)
FONTS = [
    ("ql_font_mono_12", 12),
    ("ql_font_mono_16", 16),
]

FIRST = 32
LAST = 126


def load_font(size):
    for path in TTF_CANDIDATES:
        if pathlib.Path(path).exists():
            return ImageFont.truetype(path, size)
    sys.exit("找不到 DejaVuSansMono.ttf，请安装 fonts-dejavu-core")


def baseline_of(font, ascent):
    return ascent


def render_glyph(font, ch):
    """返回 (adv_w, box_w, box_h, ofs_x, ofs_y, alpha_bytes(4bpp packed))"""
    ascent, descent = font.getmetrics()
    adv = int(round(font.getlength(ch)))

    # 先画到足够大的画布上，再裁剪出墨迹范围
    canvas_w = max(adv + 8, 16)
    canvas_h = ascent + descent + 8
    img = Image.new("L", (canvas_w, canvas_h), 0)
    draw = ImageDraw.Draw(img)
    origin_y = 4  # 顶部留白
    draw.text((4, origin_y), ch, font=font, fill=255)

    bbox = img.getbbox()
    if bbox is None:                       # 空格之类
        return adv, 0, 0, 0, 0, b""

    x0, y0, x1, y1 = bbox
    glyph = img.crop(bbox)
    box_w = x1 - x0
    box_h = y1 - y0

    # 相对基线：基线在画布上的 y = origin_y + ascent
    ofs_x = x0 - 4
    ofs_y = y0 - (origin_y + ascent)

    # 转 4bpp（每行 (w+1)//2 字节，高半字节在左）
    row_bytes = (box_w + 1) // 2
    out = bytearray(row_bytes * box_h)
    px = glyph.load()
    for y in range(box_h):
        for x in range(box_w):
            v = px[x, y] >> 4            # 0..15
            idx = y * row_bytes + x // 2
            if x % 2 == 0:
                out[idx] |= (v << 4) & 0xF0
            else:
                out[idx] |= v & 0x0F
    return adv, box_w, box_h, ofs_x, ofs_y, bytes(out)


def emit_font(font, ident):
    ascent, descent = font.getmetrics()
    glyphs = []
    blob = bytearray()
    for cp in range(FIRST, LAST + 1):
        adv, bw, bh, ox, oy, data = render_glyph(font, chr(cp))
        offset = len(blob)
        blob += data
        glyphs.append((cp, adv, bw, bh, ox, oy, offset))

    lines = []
    lines.append("static const uint8_t %s_bitmap[] = {" % ident)
    for i in range(0, len(blob), 16):
        chunk = ", ".join("0x%02X" % b for b in blob[i:i + 16])
        lines.append("    " + chunk + ",")
    if not blob:
        lines.append("    0x00,")
    lines.append("};")
    lines.append("")

    lines.append("static const QLGlyph %s_glyphs[] = {" % ident)
    for cp, adv, bw, bh, ox, oy, off in glyphs:
        lines.append(
            "    { %3d, %2d, %2d, %2d, %3d, %3d, %5d },   /* '%s' */"
            % (cp, adv, bw, bh, ox, oy, off,
               chr(cp) if cp != 92 else "\\\\")
        )
    lines.append("};")
    lines.append("")

    lines.append("const QLFont %s = {" % ident)
    lines.append('    .name        = "DejaVuSansMono-%d",' % font.size)
    lines.append("    .bpp         = 4,")
    lines.append("    .line_height = %d," % (ascent + descent))
    lines.append("    .ascent      = %d," % ascent)
    lines.append("    .descent     = %d," % descent)
    lines.append("    .fallback    = 63,   /* '?' */")
    lines.append("    .glyph_count = (uint16_t)(sizeof(%s_glyphs) / sizeof(%s_glyphs[0])),"
                 % (ident, ident))
    lines.append("    .glyphs      = %s_glyphs," % ident)
    lines.append("    .bitmap      = %s_bitmap," % ident)
    lines.append("};")
    lines.append("")
    return "\n".join(lines), len(blob)


def build():
    header = [
        "/* 本文件由 tools/qlfontgen/gen_bitmap_font.py 生成，请勿手工修改。 */",
        "/* 字体来源：DejaVu Sans Mono（Bitstream Vera 许可），4bpp 抗锯齿，ASCII 32..126。 */",
        "#include <qlgl/qlgl.h>",
        "",
    ]
    body = []
    total = 0
    for ident, size in FONTS:
        font = load_font(size)
        text, size_bytes = emit_font(font, ident)
        body.append(text)
        total += size_bytes
        print("%s: %d 字节位图" % (ident, size_bytes))
    print("合计 %d 字节字形数据" % total)
    return "\n".join(header) + "\n" + "\n".join(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    text = build()
    old = OUT.read_text(encoding="utf-8") if OUT.exists() else None
    if old == text:
        print("已是最新：%s" % OUT.relative_to(ROOT))
        return 0
    if args.check:
        print("与 TTF 不同步：%s" % OUT.relative_to(ROOT), file=sys.stderr)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")
    print("已写出：%s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
