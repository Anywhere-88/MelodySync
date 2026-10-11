# -*- coding: utf-8 -*-
"""
MelodySync 应用图标生成器
=========================

纯 PIL 实现，不依赖任何图标素材或绘图库（PIL 只用来做栅格化和缩放）。

设计：深色玻璃质感的圆角方底 + 青→紫渐变色的音符（♪）。
      右下角有一层很淡的冷色辉光，音符外侧有一圈同色光晕 —— 小尺寸下
      只剩下「深底 + 亮色音符」两层，仍然清晰可辨。

几个刻意的实现选择，写下来免得以后自己看不懂：

1) 内部按 2048×2048 绘制，最后再缩到 16~256。
   PIL 的画线/多边形原语**没有抗锯齿**，在 256 上直接画边缘全是硬台阶；
   放大到 2048 再缩回去，等于自带 8~128 倍超采样，边缘自然平滑。

2) 形状全部用「解析几何 + 多边形」构造，不用 PIL 的 rotate()。
   旋转椭圆用参数方程直接算点集，旗子用二次贝塞尔采样后按法线偏移成
   渐细的闭合多边形 —— 位置可以精确到亚像素，也不会被 rotate 的取整带偏。

3) 渐变在 64×64 上逐像素算方向投影，再放大到 2048。
   线性渐变没有高频细节，放大就是精确解；这样省掉 400 多万次循环。

4) 最后才切 alpha（先铺满整张不透明底色，再 putalpha 圆角遮罩）。
   如果一开始就把画布做成透明的，缩放时透明区的黑色会渗到边缘，
   圆角处会出现一圈脏黑边。

用法：
    python tools/make_icon.py
产物：
    resources/icons/melodysync.ico         多尺寸图标（16~256）
    resources/icons/melodysync_1024.png    设计稿预览
    resources/icons/melodysync_256.png     实际显示尺寸预览
    resources/icons/preview_sizes.png      各尺寸拼版（浅底/深底各一行）
"""

import io
import math
import os
import struct

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "resources", "icons")

# ---------------------------------------------------------------- 尺寸与配色
S = 2048          # 主画布边长
D = 1024          # 形状设计坐标空间（下文所有裸坐标都写在 0..1024 里）

MARGIN = 0.055 * S        # 背板到画布边缘的留白
CORNER = 0.232 * S        # 圆角半径（约占边长 23%，接近 Win11 / macOS 观感）

PLATE_TOP = (0x2B, 0x31, 0x3B)   # 背板渐变起点（左上）：冷调深灰
PLATE_BOT = (0x0A, 0x0C, 0x11)   # 背板渐变终点（右下）：近黑
GLOW = (0x16, 0xE0, 0xC4)        # 辉光/光晕色：偏冷的青绿
NOTE_A = (0x6E, 0xF5, 0xDC)      # 音符渐变起点：薄荷青
NOTE_B = (0x7C, 0x5C, 0xFF)      # 音符渐变终点：紫蓝

# ---------------------------------------------------------------- 音符几何定义
# 符头：一个右端朝下倾斜的椭圆
HEAD_C = (500.0, 750.0)
HEAD_A, HEAD_B = 202.0, 140.0     # 长短轴比约 1.44，接近印刷体八分音符的符头
HEAD_ROT = -24.0                  # 右端朝下倾斜

# 符干：一根竖直的胶囊，底部插进符头里以保证连成一个整体
STEM_X0, STEM_X1 = 556.0, 646.0
STEM_Y0, STEM_Y1 = 250.0, 790.0

# 旗子：从符干顶端往右下扫的二次贝塞尔曲线，沿线半径由粗到细
FLAG_P0 = (600.0, 268.0)
FLAG_P1 = (884.0, 292.0)
FLAG_P2 = (706.0, 566.0)
FLAG_R0, FLAG_R1 = 74.0, 13.0   # 旗尾收细到 13，16px 下不会糊成一坨
FLAG_SEG = 26

TARGET_H = 0.665 * S      # 音符最终高度（占画布比例）


def _clamp(v):
    v = int(v)
    return 0 if v < 0 else (255 if v > 255 else v)


# ---------------------------------------------------------------- 渐变 / 光晕
def diag_gradient(size, c0, c1, angle_deg=45.0, n=64):
    """对角线性渐变。

    angle_deg 表示渐变推进方向：45 = 从左上传向右下，135 = 从右上传向左下。
    在 n×n 上逐像素算方向投影再放大 —— 线性函数用双三次放大后依然精确。
    """
    w, h = size
    th = math.radians(angle_deg)
    dx, dy = math.cos(th), math.sin(th)
    norm = abs(dx) + abs(dy)

    small = Image.new("RGB", (n, n))
    px = small.load()
    for j in range(n):
        for i in range(n):
            u = (i + 0.5) / n - 0.5
            v = (j + 0.5) / n - 0.5
            t = (u * dx + v * dy) / norm + 0.5
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            px[i, j] = (
                _clamp(c0[0] + (c1[0] - c0[0]) * t),
                _clamp(c0[1] + (c1[1] - c0[1]) * t),
                _clamp(c0[2] + (c1[2] - c0[2]) * t),
            )
    return small.resize(size, Image.BICUBIC)


def radial_mask(size, center, radius, falloff=2.0, strength=1.0):
    """径向衰减遮罩：中心最亮(255)，到 radius 处衰减为 0。"""
    w, h = size
    r = int(radius)
    rg = Image.radial_gradient("L")            # 256²，中心 0 → 边缘 255
    m = Image.new("L", (w, h), 255)
    m.paste(rg.resize((r * 2, r * 2), Image.BICUBIC),
            (int(center[0] - r), int(center[1] - r)))
    lut = [_clamp(255.0 * strength * (1.0 - v / 255.0) ** falloff) for v in range(256)]
    return m.point(lut)


# ---------------------------------------------------------------- 音符形状
def _ellipse_pts(cx, cy, a, b, deg, n=192):
    """旋转椭圆的边界点集（参数方程直接算，避开 rotate 的取整误差）。"""
    th = math.radians(deg)
    ca, sa = math.cos(th), math.sin(th)
    pts = []
    for i in range(n):
        u = 2.0 * math.pi * i / n
        x, y = a * math.cos(u), b * math.sin(u)
        pts.append((cx + x * ca - y * sa, cy + x * sa + y * ca))
    return pts


def _quad_bezier(p0, p1, p2, t):
    m = 1.0 - t
    return (m * m * p0[0] + 2 * m * t * p1[0] + t * t * p2[0],
            m * m * p0[1] + 2 * m * t * p1[1] + t * t * p2[1])


def _flag_polygon():
    """旗子：沿贝塞尔采样一串中线点，再按法线左右各偏移 r(t)，拼成渐细多边形。

    比「沿路径叠一串半径递减的圆」稳：叠圆在两段衔接处会出现台阶。
    """
    pts = [_quad_bezier(FLAG_P0, FLAG_P1, FLAG_P2, i / (FLAG_SEG - 1.0))
           for i in range(FLAG_SEG)]
    left, right = [], []
    for i, p in enumerate(pts):
        t = i / (FLAG_SEG - 1.0)
        r = FLAG_R0 + (FLAG_R1 - FLAG_R0) * t
        a = pts[max(i - 1, 0)]
        b = pts[min(i + 1, FLAG_SEG - 1)]
        tx, ty = b[0] - a[0], b[1] - a[1]
        ln = math.hypot(tx, ty) or 1.0
        nx, ny = -ty / ln, tx / ln
        left.append((p[0] + nx * r, p[1] + ny * r))
        right.append((p[0] - nx * r, p[1] - ny * r))
    return left + right[::-1]


def note_mask(size, sc, ox, oy):
    """在 size×size 画布上画音符遮罩。

    sc/ox/oy 是「设计坐标 → 画布坐标」的缩放与平移量，
    写成参数是为了能先低成本探一遍包围盒，再按包围盒重新精确绘制。
    """
    img = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(img)
    t = lambda p: (ox + p[0] * sc, oy + p[1] * sc)

    d.polygon([t(p) for p in _ellipse_pts(HEAD_C[0], HEAD_C[1], HEAD_A, HEAD_B, HEAD_ROT)],
              fill=255)
    d.rounded_rectangle([ox + STEM_X0 * sc, oy + STEM_Y0 * sc,
                         ox + STEM_X1 * sc, oy + STEM_Y1 * sc],
                        radius=(STEM_X1 - STEM_X0) / 2.0 * sc, fill=255)
    d.polygon([t(p) for p in _flag_polygon()], fill=255)
    return img


def note_layout():
    """探测音符包围盒，算出让它居中、高度为 TARGET_H 的变换参数。"""
    probe = note_mask(D, 1.0, 0.0, 0.0)        # 设计坐标 1:1 画一遍
    bx0, by0, bx1, by1 = probe.getbbox()
    bw, bh = bx1 - bx0, by1 - by0
    sc = TARGET_H / bh
    ox = (S - bw * sc) / 2.0 - bx0 * sc
    oy = (S - TARGET_H) / 2.0 - by0 * sc
    return sc, ox, oy


# ---------------------------------------------------------------- 主图合成
def build_master():
    sc, ox, oy = note_layout()

    # 1) 底色：整张画布铺满（先不透明，最后一步才切 alpha）
    content = diag_gradient((S, S), PLATE_TOP, PLATE_BOT, 45.0)

    # 2) 右下角冷色辉光：让深色底不那么死板，有点“玻璃内部受光”的意思
    gm = radial_mask((S, S), (0.80 * S, 0.86 * S), 0.78 * S, falloff=2.4, strength=0.13)
    content = Image.composite(Image.new("RGB", (S, S), GLOW), content, gm)

    # 3) 音符遮罩 + 外发光
    note = note_mask(S, sc, ox, oy)
    halo = note.filter(ImageFilter.GaussianBlur(0.030 * S))
    halo = halo.point([_clamp(v * 0.62) for v in range(256)])
    content = Image.composite(Image.new("RGB", (S, S), GLOW), content, halo)

    # 4) 音符本体：青 → 紫渐变填充
    content = Image.composite(diag_gradient((S, S), NOTE_A, NOTE_B, 55.0), content, note)

    # 5) 顶部内描边：玻璃边缘的受光高光，越往下越弱
    ring = Image.new("L", (S, S), 0)
    inset = 0.014 * S
    ImageDraw.Draw(ring).rounded_rectangle(
        [MARGIN + inset, MARGIN + inset, S - MARGIN - inset, S - MARGIN - inset],
        radius=CORNER - inset, outline=255, width=max(1, int(0.008 * S)))
    vgrad = Image.new("L", (1, S))
    for y in range(S):
        vgrad.putpixel((0, y), _clamp(255.0 * max(0.0, 1.0 - y / float(S - 1) * 2.0) ** 1.5))
    ring = ImageChops.multiply(ring, vgrad.resize((S, S)))
    ring = ring.point([_clamp(v * 0.78) for v in range(256)])
    content = Image.composite(Image.new("RGB", (S, S), (255, 255, 255)), content, ring)

    # 6) 切出圆角外形
    plate = Image.new("L", (S, S), 0)
    ImageDraw.Draw(plate).rounded_rectangle(
        [MARGIN, MARGIN, S - MARGIN, S - MARGIN], radius=CORNER, fill=255)

    out = content.convert("RGBA")
    out.putalpha(plate)
    return out


# ---------------------------------------------------------------- 导出
def _dib(im):
    """编码 ICO 内嵌的 BMP(DIB) 块：BITMAPINFOHEADER + 自下而上的 BGRA + AND 掩码。"""
    w, h = im.size
    raw = im.tobytes()                      # RGBA，自上而下
    buf = bytearray(raw)
    for i in range(0, len(buf), 4):         # RGBA → BGRA
        buf[i], buf[i + 2] = buf[i + 2], buf[i]
    rows = [bytes(buf[y * w * 4:(y + 1) * w * 4]) for y in range(h - 1, -1, -1)]
    body = b"".join(rows)
    stride = ((w + 31) // 32) * 4           # AND 掩码每行 4 字节对齐
    and_mask = bytes(stride * h)            # 32bpp 图标靠 alpha 通道，掩码留全 0
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(body), 0, 0, 0, 0)
    return hdr + body + and_mask


def write_ico(path, imgs):
    """手写 ICO 容器：小尺寸用 BMP（兼容性最好），256 用 PNG（体积小得多）。"""
    blobs = []
    for size, im in imgs:
        if size >= 256:
            buf = io.BytesIO()
            im.convert("RGBA").save(buf, format="PNG", optimize=True)
            blobs.append(buf.getvalue())
        else:
            blobs.append(_dib(im.convert("RGBA")))

    n = len(imgs)
    offset = 6 + 16 * n
    out = bytearray(struct.pack("<HHH", 0, 1, n))
    for (size, _im), blob in zip(imgs, blobs):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                           len(blob), offset)
        offset += len(blob)
    for blob in blobs:
        out += blob
    with open(path, "wb") as fh:
        fh.write(bytes(out))


def preview_sheet(imgs, zoom_max=128):
    """把各尺寸用最近邻放大并排贴出来，模拟真机上的像素观感。

    分浅底 / 深底两行 —— 图标必须在两种桌面背景上都看得清。
    """
    cell = zoom_max
    gap = 16
    sizes = [s for s, _ in imgs]
    width = gap + sum(cell + gap for _ in sizes)
    height = gap + (cell + gap) * 2

    light = (0xF2, 0xF3, 0xF5, 255)
    dark = (0x1A, 0x1C, 0x20, 255)
    sheet = Image.new("RGBA", (width, height), light)
    ImageDraw.Draw(sheet).rectangle([0, cell + gap * 2 - gap // 2, width, height], fill=dark)

    for row in range(2):
        y = gap + row * (cell + gap)
        x = gap
        for size, im in imgs:
            z = min(cell, max(size, 16))
            big = im.resize((z, z), Image.NEAREST)
            sheet.alpha_composite(big, (x + (cell - z) // 2, y + (cell - z) // 2))
            x += cell + gap
    return sheet


def audit_ico(path, expect_sizes):
    """按字节独立解析一遍 ICO 容器，确认结构自洽。

    手写容器格式最容易错在偏移/长度上，而这类错误 PIL 往往能容忍、
    Windows 却直接读不出图标 —— 所以这里不经过 PIL 的读取路径，
    直接核对：ICONDIR 头 -> 每个 ICONDIRENTRY -> 数据块是否首尾相接。
    返回问题列表，空列表表示通过。
    """
    with open(path, "rb") as fh:
        data = fh.read()
    problems = []

    reserved, rtype, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or rtype != 1:
        problems.append("ICONDIR reserved/type wrong: %d/%d" % (reserved, rtype))
    if count != len(expect_sizes):
        problems.append("ICONDIR count %d != expected %d" % (count, len(expect_sizes)))

    got = []
    cursor = 6 + 16 * count
    for i in range(count):
        w, h, colors, res, planes, bpp, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + 16 * i)
        # 规范里 0 表示 256（一个字节存不下 256）
        got.append(256 if w == 0 else w)
        if w != h:
            problems.append("entry %d: not square (%d x %d)" % (i, w, h))
        if bpp != 32:
            problems.append("entry %d: bitcount %d != 32" % (i, bpp))
        if planes != 1:
            problems.append("entry %d: planes %d != 1" % (i, planes))
        if offset != cursor:
            problems.append("entry %d: offset %d != expected %d (gap or overlap)"
                            % (i, offset, cursor))
        if offset + size > len(data):
            problems.append("entry %d: data runs past end of file" % i)

        blob = data[offset:offset + size]
        if blob[:8] != b"\x89PNG\r\n\x1a\n":
            # 非 PNG 的必须是 BITMAPINFOHEADER，且 biHeight 是真实高度的两倍
            # （ICO 里的 BMP 把 XOR 位图与 AND 掩码记为"两层"，高度翻倍）
            if len(blob) < 40:
                problems.append("entry %d: DIB header truncated" % i)
            else:
                hsz, hw, hh = struct.unpack_from("<Iii", blob, 0)
                if hsz != 40:
                    problems.append("entry %d: biSize %d != 40" % (i, hsz))
                if hw != w or hh != h * 2:
                    problems.append("entry %d: DIB is %dx%d, expected %dx%d"
                                    % (i, hw, hh, w, h * 2))
        cursor = offset + size

    if cursor != len(data):
        problems.append("trailing bytes: parsed %d of %d" % (cursor, len(data)))
    if sorted(got) != sorted(expect_sizes):
        problems.append("embedded sizes %s != expected %s"
                        % (sorted(got), sorted(expect_sizes)))
    return problems


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    master = build_master()

    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    imgs = [(s, master.resize((s, s), Image.LANCZOS)) for s in sizes]

    master.resize((1024, 1024), Image.LANCZOS).save(
        os.path.join(OUT_DIR, "melodysync_1024.png"))
    master.resize((256, 256), Image.LANCZOS).save(
        os.path.join(OUT_DIR, "melodysync_256.png"))
    preview_sheet(imgs).save(os.path.join(OUT_DIR, "preview_sizes.png"))

    ico = os.path.join(OUT_DIR, "melodysync.ico")
    write_ico(ico, imgs)

    print("ico written : %s (%d bytes)" % (ico, os.path.getsize(ico)))
    problems = audit_ico(ico, sizes)
    if problems:
        for p in problems:
            print("  [FAIL] %s" % p)
        raise SystemExit(1)
    print("container   : [ OK ] %d frames, offsets contiguous, all 32bpp"
          % len(sizes))
    print("sizes       : %s" % (sizes,))
    print("png preview : melodysync_1024.png / melodysync_256.png / preview_sizes.png")


if __name__ == "__main__":
    main()
