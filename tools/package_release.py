#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MelodySync 发布打包：源码 + 已部署的可运行产物 -> 单个 zip。

打包内容
  - 全部源码（src/ ui/ tools/ resources/ installer/ bin/ CMakeLists.txt README.md 等）
  - build/bin/  已 windeployqt 的完整可运行产物（exe + Qt/FFmpeg/yt-dlp 依赖）

排除内容
  - build/ 下除 bin/ 以外的一切（CMakeFiles、*.obj、*.tlog 等中间产物）
  - build-debug/、vcpkg_installed/、__pycache__/、.vs/、.git/
  - 构建时挪走的旧 exe（*.occupied*）

用法
  python tools/package_release.py                  # 输出到桌面
  python tools/package_release.py -o D:\\x.zip     # 指定输出路径
  python tools/package_release.py --source-only    # 仅源码，不带可运行产物
  python tools/package_release.py --name foo.zip   # 指定包名
"""

import argparse
import os
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = "1.0.0"
STAMP = time.strftime("%Y%m%d")
DEFAULT_NAME = f"MelodySync-{VERSION}-win64-{STAMP}.zip"

# 整目录排除（路径中任意一段命中即排除，以覆盖嵌套的 __pycache__ 等）
SKIP_DIR_NAMES = {"__pycache__", ".vs", ".git", "vcpkg_installed", "build-debug"}
# 文件级排除
SKIP_SUFFIXES = {".obj", ".pdb", ".tlog", ".ilk", ".exp", ".iobj", ".ipdb", ".lastbuildstate", ".recipe"}


def dir_allowed(rel: Path, source_only: bool) -> bool:
    """rel 是相对 ROOT 的目录路径（顶层目录的 parts 长度为 1）。"""
    parts = rel.parts
    if not parts:
        return True
    if any(p in SKIP_DIR_NAMES for p in parts):
        return False
    if parts[0] == "build":
        if source_only:
            return False
        # 只放行 build/ 本身与 build/bin 及其子目录
        return len(parts) == 1 or parts[1] == "bin"
    return True


def file_allowed(rel: Path, source_only: bool) -> bool:
    # dir_allowed 对任意深度都做完整判断，父目录通过即代表整条路径合法
    if not dir_allowed(rel.parent, source_only):
        return False
    # build/ 下只有 build/bin/ 之内的文件算产物，顶层散落的日志/旧 exe 备份一律排除
    parts = rel.parts
    if parts[0] == "build" and (len(parts) < 3 or parts[1] != "bin"):
        return False
    if "occupied" in rel.name:
        return False
    if rel.suffix.lower() in SKIP_SUFFIXES:
        return False
    return True


def collect(source_only: bool):
    files = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        d = Path(dirpath)
        rel_dir = d.relative_to(ROOT)
        # 剪枝：不进入被排除的目录（./build 与 build 等价）
        dirnames[:] = [n for n in dirnames if dir_allowed(rel_dir / n, source_only)]
        for name in filenames:
            rel = rel_dir / name
            if file_allowed(rel, source_only):
                files.append((d / name, rel))
    files.sort(key=lambda x: str(x[1]).lower())
    return files


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"


def main() -> int:
    ap = argparse.ArgumentParser(description="MelodySync 发布打包")
    ap.add_argument("-o", "--out", default=None, help="输出 zip 路径（默认桌面）")
    ap.add_argument("--name", default=None, help="输出文件名（放在输出目录里）")
    ap.add_argument("--source-only", action="store_true", help="仅源码，不带 build/bin")
    args = ap.parse_args()

    if args.out:
        out = Path(args.out)
        if out.is_dir():
            out = out / (args.name or DEFAULT_NAME)
    else:
        out = Path(os.path.expanduser("~")) / "Desktop" / (args.name or DEFAULT_NAME)

    if not ROOT.is_dir():
        print(f"[X] 工程根目录不存在: {ROOT}")
        return 1

    print(f"[*] 工程根 : {ROOT}")
    print(f"[*] 输出   : {out}")
    print(f"[*] 模式   : {'仅源码' if args.source_only else '源码 + 可运行产物'}")
    print()

    files = collect(args.source_only)
    if not files:
        print("[X] 没有收集到任何文件")
        return 1

    total_raw = sum(f.stat().st_size for f, _ in files)
    print(f"[*] 待打包 {len(files)} 个文件，原始大小 {human(total_raw)}")
    print()

    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()

    stage = Path("MelodySync")
    t0 = time.time()
    done_raw = 0
    last_report = 0.0
    fail = 0

    try:
        # compresslevel=1：DLL/EXE 本身已接近不可压缩，等级拉高只增加耗时
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as zf:
            for src, rel in files:
                try:
                    zf.write(src, arcname=str(stage / rel).replace("\\", "/"))
                    done_raw += src.stat().st_size
                except (PermissionError, OSError) as e:
                    fail += 1
                    print(f"    [跳过] {rel} — {e}")
                now = time.time()
                if now - last_report > 1.0:
                    last_report = now
                    pct = done_raw * 100.0 / total_raw if total_raw else 100.0
                    sys.stdout.write(f"\r    ...{pct:5.1f}%  {human(done_raw)} / {human(total_raw)}   ")
                    sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n[!] 已中断，删除半成品")
        if out.exists():
            out.unlink()
        return 130

    sys.stdout.write("\r" + " " * 72 + "\r")
    zsize = out.stat().st_size
    cost = time.time() - t0
    ratio = zsize * 100.0 / total_raw if total_raw else 0

    print(f"[OK] 打包完成：{out}")
    print(f"     文件数   : {len(files) - fail} (跳过 {fail})")
    print(f"     原始大小 : {human(total_raw)}")
    print(f"     压缩后   : {human(zsize)}  (压缩率 {ratio:.1f}%)")
    print(f"     耗时     : {cost:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
