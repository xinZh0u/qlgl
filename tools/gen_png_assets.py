#!/usr/bin/env python3
"""生成 test/assets/png/ 下的 PNG 对拍样例（需要 Pillow）。

每个 `<name>.png` 配一个 `<name>.rgba`：Pillow 解出来的 **RGBA8888 原始字节**
（w*h*4，行间不留 padding）。C 侧 `test_png_assets` 用自家解码器解同一份 PNG，
逐字节比对 —— 这样 inflate 的动态 Huffman 与各颜色类型都有真实压缩流兜底。

只放**程序生成的无损小图**（纯色 / 渐变 / 四色块），不放照片：
golden 与对拍都不希望基准随编码器版本抖动。
"""
import os
import sys

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "assets", "png")


def rgba_baseline(path):
    """PNG 规范口径的 RGBA8888 基准。

    16 位灰度要**取高字节**（`v >> 8`），而 Pillow 的 `I;16 -> RGBA` 是**裁剪**
    （4096 变成 255），那个口径跟 PNG 不一致，所以这里自己算。
    """
    im = Image.open(path)
    if im.mode == "I;16":
        raw = im.tobytes()
        out = bytearray()
        for i in range(0, len(raw), 2):
            v = ((raw[i] | (raw[i + 1] << 8)) >> 8) & 0xFF
            out += bytes((v, v, v, 255))
        return bytes(out)
    return im.convert("RGBA").tobytes()


def save(name, img):
    """**按原模式**存 PNG（这样颜色类型/位深才有多样性），
    再把解出来的 RGBA8888 落成 .rgba 基准。"""
    path = os.path.join(OUT, name + ".png")
    img.save(path, optimize=False, compress_level=9)
    back = Image.open(path)
    with open(os.path.join(OUT, name + ".rgba"), "wb") as f:
        f.write(rgba_baseline(path))
    print("%-14s %s %dx%d %d bytes" % (name, img.mode, back.width, back.height,
                                       os.path.getsize(path)))


def main():
    os.makedirs(OUT, exist_ok=True)

    # 1. RGBA8 渐变（动态 Huffman + 每行都有 data 变化）
    im = Image.new("RGBA", (64, 48))
    im.putdata([(x * 4 % 256, y * 5 % 256, (x + y) * 2 % 256, 255 - x * 3 % 256)
                for y in range(48) for x in range(64)])
    save("rgba8_gradient", im)

    # 2. RGB8 四色块 + 一条对角线（压出来更接近真实素材）
    im = Image.new("RGB", (33, 17))
    px = im.load()
    for y in range(17):
        for x in range(33):
            if x == y:
                px[x, y] = (255, 255, 255)
            elif x < 16 and y < 8:
                px[x, y] = (200, 30, 40)
            elif x >= 16 and y < 8:
                px[x, y] = (30, 200, 40)
            elif x < 16:
                px[x, y] = (30, 40, 200)
            else:
                px[x, y] = (240, 200, 20)
    save("rgb8_quad", im)

    # 3. 8 位灰度渐变
    im = Image.new("L", (40, 20))
    im.putdata([(x * 6 + y * 3) % 256 for y in range(20) for x in range(40)])
    save("gray8_ramp", im)

    # 4. 1 位黑白（位深 1 的打包路径）
    im = Image.new("1", (37, 9))
    im.putdata([1 if ((x // 3) + (y // 2)) % 2 else 0
                for y in range(9) for x in range(37)])
    save("bw1_checker", im)

    # 5. 调色板 4 位（PIL 会按需降位深）
    im = Image.new("P", (24, 12))
    im.putpalette([v for i in range(16) for v in (i * 16, 255 - i * 16, (i * 37) % 256)])
    im.putdata([(x // 2 + y) % 16 for y in range(12) for x in range(24)])
    save("pal4_blocks", im)

    # 6. 16 位灰度（高低字节都要对）
    im = Image.new("I;16", (16, 8))
    im.putdata([((x * 4096 + y * 512) & 0xFFFF) for y in range(8) for x in range(16)])
    save("gray16_ramp", im)

    # 7. 带全透明区域的 RGBA（大面积重复 → 长 RUN，压得很小）
    im = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    for y in range(8, 24):
        for x in range(8, 24):
            im.putpixel((x, y), (255, 128, 0, 255))
    save("rgba8_sprite", im)


if __name__ == "__main__":
    try:
        main()
    except ImportError:
        sys.exit("需要 Pillow：pip install pillow")
