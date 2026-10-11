#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""JS 作用域守门：检查 ui/index.html 内联脚本里的"未定义引用"。

为什么需要它
============
2026-09-19 事故：srcName 定义在 renderAiResults 函数内部，而顶层函数
_candText 也引用了它 —— 用户一点搜索，渲染结果时抛
"ReferenceError: srcName is not defined"，整个结果列表画不出来。
这类错误桥接自检（selftest.py）永远覆盖不到（它只调桥接方法，
不执行渲染路径），静态的 check_sources.py 也不做词法分析。

本工具用 eslint 的 no-undef 规则做完整词法作用域解析：
引用了一个在引用处不可见的名字（无论它声明在别的函数里还是压根
没声明）都会报出来。

用法
====
    python tools/check_js_scope.py            # 检查 ui/index.html
    python tools/check_js_scope.py 某文件.html # 检查其他文件
"""

import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NODE_WORKSPACE = os.path.normpath(os.path.join(
    os.environ.get("USERPROFILE", ""), ".workbuddy", "binaries", "node", "workspace"))
ESLINT = os.path.join(NODE_WORKSPACE, "node_modules", ".bin", "eslint.cmd")

# 页面里确实存在、但 eslint 不知道的全局（QWebChannel 注入环境）
# module：内嵌 QRCodeLite 的 UMD 导出保护 `typeof module !== 'undefined'`，
# 浏览器里该分支不会执行，合法。
EXTRA_GLOBALS = ["qt", "QWebChannel", "chrome", "module"]


def extract_inline_scripts(html_path):
    """抽出所有内联 <script> 块（跳过带 src= 的外链块）。"""
    with open(html_path, "r", encoding="utf-8") as fh:
        html = fh.read()
    blocks = []
    for m in re.finditer(r"<script\b([^>]*)>(.*?)</script>", html, re.S | re.I):
        attrs, code = m.group(1), m.group(2)
        if re.search(r"\bsrc\s*=", attrs, re.I):
            continue
        if code.strip():
            blocks.append(code)
    return blocks


def run_eslint(js_path):
    """返回 (是否通过, 输出文本)。"""
    if not os.path.isfile(ESLINT):
        return False, "找不到 eslint：%s（先在 node workspace 里 npm install eslint@8）" % ESLINT
    rule = json.dumps({"no-undef": "error"})
    # ⚠ eslint 8 的 --env browser 不含 ES2022 内建全局（Promise/Uint8Array 等
    #   会被误报 not defined），必须显式加 es2022。
    args = [ESLINT, "--no-eslintrc", "--env", "browser,es2022",
            "--parser-options", "ecmaVersion:2022",
            "--global", ",".join(EXTRA_GLOBALS),
            "--rule", rule, "-f", "unix", js_path]
    proc = subprocess.run(args, capture_output=True, text=True,
                          cwd=NODE_WORKSPACE, timeout=120)
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "ui", "index.html")
    if not os.path.isfile(target):
        sys.exit("找不到目标文件：%s" % target)

    blocks = extract_inline_scripts(target)
    if not blocks:
        print("没有内联 <script> 块，无事可做")
        return 0

    # ⚠ 经典 <script> 的顶层 let/const/function 进入全局词法环境，
    #   **跨 script 块可见**。分开检查会把"定义在块A、引用在块B"误报成
    #   未定义，所以必须合并成一份再检查；用空行补齐各块的行号偏移，
    #   报错行号就能直接对应回 html 原始行号。
    with open(target, "r", encoding="utf-8") as fh:
        html = fh.read()

    combined = []
    base_lines = []          # 每个块在合并文件里的起始行（1-based）
    html_starts = []         # 每个块在 html 里的起始行（1-based）
    offset = 0
    for code in blocks:
        combined.append(code)
        base_lines.append(offset + 1)
        offset += code.count("\n") + 1   # +1 是块间分隔空行
        pos = html.find(code[:200])
        html_starts.append(html[:pos].count("\n") + 1 if pos >= 0 else 1)

    tmp = tempfile.NamedTemporaryFile("w", suffix=".js", delete=False,
                                      encoding="utf-8", dir=os.path.join(tempfile.gettempdir()))
    tmp.write("\n".join(combined))
    tmp.close()
    try:
        ok, out = run_eslint(tmp.name)
    finally:
        try:
            os.remove(tmp.name)
        except OSError:
            pass

    if ok:
        print("RESULT: ALL CHECKS PASSED（%d 个内联脚本块，作用域引用全部可见）" % len(blocks))
        return 0

    # 合并文件行号 → 块内偏移 → html 原始行号
    def locate(n):
        for i in range(len(blocks) - 1, -1, -1):
            if n >= base_lines[i]:
                return html_starts[i] + (n - base_lines[i])
        return n

    print("发现未定义引用（渲染时会抛 ReferenceError）：")
    for line in out.splitlines():
        m = re.match(r".*?:(\d+):(\d+): (.*)", line)
        if m:
            print("  ui/index.html:%d  %s" % (locate(int(m.group(1))), m.group(3)))
        elif line.strip():
            print("  " + line)
    print("\nRESULT: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
