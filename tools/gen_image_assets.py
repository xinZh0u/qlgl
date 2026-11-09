#!/usr/bin/env python3
"""生成 test/assets/{png,img}/ 下的图像对拍样例（需要 Pillow）。

每个 `<name>.<ext>` 配一个 `<name>.rgba`：**RGBA8888 原始字节**（w*h*4，无行间填充）。
C 侧用自家解码器解同一份文件，逐字节（JPEG 用容差）比对：

- `test/assets/png/` —— PNG：让 inflate 的动态 Huffman 与各颜色类型都有真实压缩流兜底；
- `test/assets/img/` —— GIF（调色板 / 透明 / 隔行）与 JPEG（品质 95、无子采样）。

只放**程序生成的小图**（纯色 / 渐变 / 四色块），不放照片：
golden 与对拍都不希望基准随编码器版本抖动。
"""
import os
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "assets")
OUT = os.path.join(ROOT, "png")
OUT_IMG = os.path.join(ROOT, "img")


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


def save_image(name, img, tol_dir=OUT_IMG):
    """存到 test/assets/img/（GIF / JPEG 这类"平台后端或自有 LZW"的格式）。"""
    path = os.path.join(tol_dir, name)
    img.save(path)
    back = Image.open(path)
    with open(os.path.join(tol_dir, name + ".rgba"), "wb") as f:
        f.write(rgba_baseline(path))
    print("%-16s %s %dx%d %d bytes" % (name, img.mode, back.width, back.height,
                                       os.path.getsize(path)))


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(OUT_IMG, exist_ok=True)

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


def make_img_assets():
    """GIF 与 JPEG：给 test_gif_assets / test_jpeg 用。"""
    rgb = Image.new("RGB", (40, 30))
    px = rgb.load()
    for y in range(30):
        for x in range(40):
            if x < 20 and y < 15:
                px[x, y] = (220, 30, 40)
            elif x >= 20 and y < 15:
                px[x, y] = (30, 200, 40)
            elif x < 20:
                px[x, y] = (30, 40, 220)
            else:
                px[x, y] = (240, 200, 20)

    # GIF：8 色调色板（Pillow 写出来是隔行扫描，正好覆盖那条路径）
    save_image("quad.gif", rgb.convert("P", palette=Image.ADAPTIVE, colors=8))

    # GIF：带一块透明区域
    g = Image.new("P", (16, 16), 0)
    g.putpalette([v for i in range(8) for v in (i * 32, 255 - i * 32, (i * 61) % 256)])
    for y in range(4, 12):
        for x in range(4, 12):
            g.putpixel((x, y), 1)
    save_image("trans.gif", g)

    # JPEG：品质 95、不做色度子采样（让容差比对有意义）
    rgb.save(os.path.join(OUT_IMG, "quad.jpg"), quality=95, subsampling=0)
    back = Image.open(os.path.join(OUT_IMG, "quad.jpg")).convert("RGBA")
    with open(os.path.join(OUT_IMG, "quad.jpg.rgba"), "wb") as f:
        f.write(back.tobytes())
    print("%-16s %s %dx%d %d bytes" % ("quad.jpg", "RGB", back.width, back.height,
                                       os.path.getsize(os.path.join(OUT_IMG, "quad.jpg"))))

    # JPEG：灰度（走 JCS_GRAYSCALE → 复制三通道）
    gray = rgb.convert("L")
    gray.save(os.path.join(OUT_IMG, "gray.jpg"), quality=95)
    back = Image.open(os.path.join(OUT_IMG, "gray.jpg")).convert("RGBA")
    with open(os.path.join(OUT_IMG, "gray.jpg.rgba"), "wb") as f:
        f.write(back.tobytes())
    print("%-16s %s %dx%d %d bytes" % ("gray.jpg", "L", back.width, back.height,
                                       os.path.getsize(os.path.join(OUT_IMG, "gray.jpg"))))


if __name__ == "__main__":
    try:
        main()
        make_img_assets()
    except ImportError:
        sys.exit("需要 Pillow：pip install pillow")
