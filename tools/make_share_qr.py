#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把分发链接渲染成带说明文字的二维码 PNG，方便别人扫码下载。

配合 tools/serve_zip.py 使用：
    python tools\\serve_zip.py --file <zip>          # 拿到局域网 URL
    python tools\\make_share_qr.py --url <URL>       # 出二维码图

需要 qrcode + Pillow（隔离环境已装：
  C:\\Users\\<you>\\.workbuddy\\binaries\\python\\envs\\default\\Scripts\\python.exe
）
"""

import argparse
import sys
from pathlib import Path

try:
    import qrcode
except ImportError:
    print("[X] 缺少 qrcode 库。装到隔离环境：")
    print("    <venv>\\Scripts\\python.exe -m pip install qrcode[pil]")
    sys.exit(1)

from PIL import Image, ImageDraw, ImageFont

# 中文说明写在脚本里而不是命令行参数 —— 命令行传中文在 MSYS/Git Bash 下
# 有 argv 编码风险，写死在源码（UTF-8）里最稳。要加新场景就往 PRESETS 里加一条。
PRESETS = {
    "lan": ("扫码下载 MelodySync 1.1.0", "（需与电脑连接同一个 WiFi）"),
    "public": ("扫码下载 MelodySync 1.1.0", "（任何网络均可下载，手机流量也行）"),
}
DEFAULT_PRESET = "lan"

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",     # 微软雅黑
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\simsun.ttc",
]


def load_font(size: int):
    for p in FONT_CANDIDATES:
        if Path(p).is_file():
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                continue
    print("[!] 没找到中文字体，说明文字可能显示为方块")
    return ImageFont.load_default()


def text_size(draw, text, font):
    box = draw.textbbox((0, 0), text, font=font)
    return box[2] - box[0], box[3] - box[1]


def main() -> int:
    ap = argparse.ArgumentParser(description="生成分享用二维码 PNG")
    ap.add_argument("--url", required=True, help="要编码的下载链接")
    ap.add_argument("--out", default=None, help="输出 png 路径，默认桌面")
    ap.add_argument("--preset", choices=sorted(PRESETS), default=DEFAULT_PRESET,
                    help="内置文案场景：lan=局域网（需同一 WiFi）/ public=公网隧道")
    ap.add_argument("--title", default=None, help="覆盖标题（中文参数在 MSYS 下有编码风险，慎用）")
    ap.add_argument("--note", default=None, help="覆盖说明（同上）")
    ap.add_argument("--box", type=int, default=12, help="二维码像素倍率，越大越清晰")
    args = ap.parse_args()

    def_title, def_note = PRESETS[args.preset]
    title = args.title or def_title
    note = args.note or def_note

    qr = qrcode.QRCode(
        error_correction=qrcode.constants.ERROR_CORRECT_M,  # M 级：容错够用又不至于太密
        box_size=args.box,
        border=2,
    )
    qr.add_data(args.url)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")

    pad = 48
    title_font = load_font(34)
    note_font = load_font(24)

    probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    tw, th = text_size(probe, title, title_font)
    nw, nh = text_size(probe, note, note_font)

    width = max(qr_img.width, tw, nw) + pad * 2
    height = pad + th + 20 + qr_img.height + 20 + nh + pad
    canvas = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(canvas)

    y = pad
    draw.text(((width - tw) // 2, y), title, fill="#111111", font=title_font)
    y += th + 20
    canvas.paste(qr_img, ((width - qr_img.width) // 2, y))
    y += qr_img.height + 20
    draw.text(((width - nw) // 2, y), note, fill="#666666", font=note_font)

    out = Path(args.out) if args.out else Path.home() / "Desktop" / "MelodySync-share-qr.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)

    print(f"[OK] 二维码已生成：{out}")
    print(f"     内容：{args.url}")
    print(f"     尺寸：{width}x{height}（二维码本体 {qr_img.width}px）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
