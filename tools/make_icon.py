#!/usr/bin/env python3
"""Hermes 模型面板的应用图标生成器（logo = 标题栏那枚 mark）。

logo 的形状来自旧版渲染层，**同一份定义**在三个地方用：
  * 标题栏 mark：旧 `index.html` 的 `<span class="mark" data-ic="activity">` +
    `styles.css` `.titlebar .mark { width:18px; height:18px; border-radius:5px;
    background: var(--hx-accent-soft); color: var(--hx-accent-strong) }`；
    脉冲折线取自 `renderer` 的 activity 图标（24×24 网格）：
    (2,12) (4.5,12) (6.9,3.6) (9.5,20.4) (12,12) (14.6,12) (22,12)。
  * 托盘图标：`Application::CreateTrayIcon`（运行时用 GDI 以 4× 超采样画同一枚 mark，
    颜色按状态换：ok 绿 / warn 琥珀 / err 红）。
  * exe 图标：本脚本产出的 `assets/hermes.ico`（多尺寸），由 `assets/hermes.rc` 编进 exe。

用法（在项目根目录）：
    python tools/make_icon.py            # 写 assets/hermes.ico + assets/hermes-256.png
    python tools/make_icon.py --preview  # 额外写 tmp 下的小尺寸预览图，便于肉眼核对

只依赖 Pillow。所有输出路径都限定在项目内（E:/AI/Zcode/projects/hermes-panel-native/）。
"""
from __future__ import annotations

import argparse
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUPERSAMPLE = 8  # 8× 画完再缩，得到接近 Direct2D 的抗锯齿边缘

# --- 与 C++/CSS 同源的几何与配色 -------------------------------------------------
# 24×24 网格里的脉冲折线（render/icons.cpp 的 activity）。
PULSE = [(2.0, 12.0), (4.5, 12.0), (6.9, 3.6), (9.5, 20.4), (12.0, 12.0),
         (14.6, 12.0), (22.0, 12.0)]
GRID = 24.0
GLYPH_SPAN = 0.62        # 脉冲折线占图标边长的比例（标题栏是 12/18 = 0.67）
TILE_INSET = 0.04        # 圆角方块四周留白（比例）
TILE_RADIUS = 0.26       # 圆角半径（旧 5/18 ≈ 0.28）
STROKE = 0.075           # 折线粗细（占边长比例；旧 mark 的 12px 字形用 1.7~2px 描边 ≈ 0.15/12）

# 深色主题 token（styles.css `:root`）。
INK = (8, 10, 14, 255)           # --hx-ink
SURF = (15, 18, 24, 255)         # --hx-surf
PLATE_A = (29, 35, 46, 255)      # --hx-plate-a  #1d232e
PLATE_B = (21, 26, 34, 255)      # --hx-plate-b  #151a22
LINE = (36, 43, 56, 255)         # --hx-line     #242b38
ACCENT = (56, 189, 219, 255)     # --hx-accent   #38bddb
ACCENT_STRONG = (125, 216, 238, 255)  # --hx-accent-strong #7dd8ee
GLOW = (56, 189, 219, 90)        # 折线后的柔光（旧 .hlamp 的 13px 模糊观感）


def draw_mark(size: int, glyph: tuple[int, int, int, int] = ACCENT_STRONG,
              tile_top: tuple[int, int, int, int] = PLATE_A,
              tile_bottom: tuple[int, int, int, int] = PLATE_B,
              glow: tuple[int, int, int, int] | None = GLOW) -> Image.Image:
    """画一枚 mark：深色圆角方块（纵向渐变 + 1px 描边）+ 状态色脉冲折线。"""
    scale = SUPERSAMPLE
    canvas = size * scale
    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    inset = canvas * TILE_INSET
    box = (inset, inset, canvas - inset, canvas - inset)
    radius = canvas * TILE_RADIUS

    # 圆角方块：渐变用「上矩形 + 下矩形」两段近似（PIL 没有原生线性渐变剪切）。
    tile = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    tile_draw = ImageDraw.Draw(tile)
    tile_draw.rounded_rectangle(box, radius=radius, fill=tile_top)
    for row in range(int(box[1]), int(box[3])):
        ratio = (row - box[1]) / max(1.0, box[3] - box[1])
        colour = tuple(int(tile_top[i] + (tile_bottom[i] - tile_top[i]) * ratio) for i in range(4))
        tile_draw.line([(box[0], row), (box[2], row)], fill=colour)
    mask = Image.new("L", (canvas, canvas), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    image.paste(tile, (0, 0), mask)

    # 描边：--hx-line，1px（按尺寸缩放，15px 以下不画，免得糊）。
    stroke_width = max(1, int(round(canvas / size)))
    if size >= 20:
        draw.rounded_rectangle(box, radius=radius, outline=LINE, width=stroke_width)

    # 折线坐标：把 24×24 网格映射到图标中心。
    span = canvas * GLYPH_SPAN
    origin = (canvas - span) / 2.0
    points = [(origin + (x / GRID) * span, origin + (y / GRID) * span) for x, y in PULSE]
    width = max(2, int(round(canvas * STROKE)))

    if glow is not None:
        # 柔光：同一折线用更粗、更淡的笔画在下面铺一层（等价旧 .hlamp 的模糊半径）。
        glow_layer = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
        ImageDraw.Draw(glow_layer).line(points, fill=glow, width=int(width * 2.6), joint="curve")
        from PIL import ImageFilter
        glow_layer = glow_layer.filter(ImageFilter.GaussianBlur(canvas * 0.035))
        image.alpha_composite(glow_layer)

    draw.line(points, fill=glyph, width=width, joint="curve")
    # 端点是圆头（旧 mark 的 stroke-linecap 是 round）。
    for point in (points[0], points[-1]):
        draw.ellipse([point[0] - width / 2, point[1] - width / 2,
                      point[0] + width / 2, point[1] + width / 2], fill=glyph)

    return image.resize((size, size), Image.LANCZOS)


def write_assets(preview: bool) -> None:
    assets = os.path.join(ROOT, "assets")
    os.makedirs(assets, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [draw_mark(size) for size in sizes]
    ico = os.path.join(assets, "hermes.ico")
    frames[-1].save(ico, format="ICO",
                    sizes=[(size, size) for size in sizes],
                    append_images=frames[:-1])
    frames[-1].save(os.path.join(assets, "hermes-256.png"))
    print("wrote", ico, "sizes:", sizes)
    if preview:
        # 证据图只写工作区的 tmp（AGENTS.md 的路径约束：只有 E:/AI/Zcode/tmp/ 可以临时写）。
        out = "E:/AI/Zcode/tmp/hermes-panel-native/evidence"
        os.makedirs(out, exist_ok=True)
        sheet = Image.new("RGBA", (16 + 20 + 24 + 32 + 48 + 64 + 128 + 8 * 8, 256), (17, 19, 24, 255))
        x = 4
        for frame in frames[:-1]:
            sheet.alpha_composite(frame, (x, 256 - frame.height - 4))
            x += frame.width + 8
        sheet.alpha_composite(frames[-1], (x, 0))
        sheet.save(os.path.join(out, "icon-sheet.png"))
        print("preview:", os.path.join(out, "icon-sheet.png"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true", help="额外输出尺寸总览图（肉眼核对）")
    write_assets(parser.parse_args().preview)
