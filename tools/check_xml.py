# -*- coding: utf-8 -*-
"""
XML 良构性 + 路径解析校验脚本。

为什么需要它：MSBuild 加载 .vcxproj / .vcxproj.filters 时用的是严格 XML 解析器，
一旦注释里出现连续两个连字符（例如把命令行里的 `cmake --build` 原样抄进注释），
XML 解析就直接失败，报 "not well-formed"，而 VS 只会弹一个很含糊的对话框。
同理 resources.qrc 也要能被 Qt 的资源系统正常解析。

本脚本做三件事：
  1) 用 xml.etree 解析所有 XML 文件，任何解析错误立即定位到行列号；
  2) 额外扫描注释体，检查是否存在非法的 `--`（因为有些解析器宽容、有些严格）；
  3) 对 .vcxproj 里所有 Include= 引用的文件做真实存在性检查 —— 这一条能立刻
     抓出「项目文件在 MelodySync/MelodySync/ 但源码在 ../src/」这类相对路径错误。

用法：
    python tools/check_xml.py            # 在 MelodySync 目录下执行
退出码 0 = 全部通过，1 = 有失败项。
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

XML_FILES = [
    "MelodySync/MelodySync.vcxproj",
    "MelodySync/MelodySync.vcxproj.filters",
    "resources/resources.qrc",
]

# MSBuild 命名空间
NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"

# 这些 Include 是逻辑路径 / 生成物，不要求磁盘上真实存在
SKIP_PREFIX = ("$(ProjectDir)", "$(SolutionDir)", ".")

comments_checked = 0
failures = []


def check_comment_hyphens(path, text):
    """XML 注释里不允许出现 `--`。返回所有违规位置的行号列表。"""
    global comments_checked
    bad = []
    for m in re.finditer(r"<!--(.*?)-->", text, re.S):
        comments_checked += 1
        body = m.group(1)
        if "--" in body:
            # 定位到具体行
            start = text.count("\n", 0, m.start()) + 1
            for i, line in enumerate(body.splitlines()):
                if "--" in line:
                    bad.append((start + i, line.strip()[:90]))
    return bad


def check_wellformed(path, text):
    try:
        ET.fromstring(text)
        return None
    except ET.ParseError as e:
        return str(e)


def collect_includes(path):
    """取 .vcxproj 里所有包含文件路径的 Include 属性值，连同一并的 Condition。"""
    tree = ET.parse(path)
    root = tree.getroot()
    out = []
    for elem in root.iter():
        inc = elem.get("Include")
        if not inc:
            continue
        tag = elem.tag.replace(NS, "")
        out.append((tag, inc, elem.get("Condition") or ""))
    return out


def main():
    print("=" * 72)
    print("XML well-formedness / path resolution check")
    print("root =", ROOT)
    print("=" * 72)

    for rel in XML_FILES:
        full = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            failures.append("%s : FILE MISSING" % rel)
            print("[MISS] %s  (file does not exist)" % rel)
            continue

        with open(full, "r", encoding="utf-8") as fh:
            text = fh.read()

        print("\n--- %s  (%d bytes) ---" % (rel, len(text.encode("utf-8"))))

        err = check_wellformed(full, text)
        if err:
            failures.append("%s : XML parse error: %s" % (rel, err))
            print("  [FAIL] parse error -> %s" % err)
        else:
            print("  [ OK ] XML is well-formed")

        bad = check_comment_hyphens(full, text)
        if bad:
            failures.append("%s : illegal '--' inside XML comment" % rel)
            for line_no, snippet in bad:
                print("  [FAIL] line %d contains '--' in a comment: %s" % (line_no, snippet))
        else:
            print("  [ OK ] no illegal double hyphen inside comments")

    # --- 路径存在性 ---
    proj = os.path.join(ROOT, "MelodySync", "MelodySync.vcxproj")
    proj_dir = os.path.dirname(proj)

    if os.path.isfile(proj):
        print("\n--- resolved Include paths (relative to MelodySync/MelodySync/) ---")
        src_items = []
        other_items = []
        for tag, inc, cond in collect_includes(proj):
            # 只看真实磁盘路径：凡是含 MSBuild 变量（$(ProjectDir)/$(QtDir)...）
            # 的条目留给 VS 自己展开，这里没法校验。
            if "$(" in inc:
                other_items.append((tag, inc))
                continue
            if tag in ("ClCompile", "ClInclude", "QtRcc", "QtMoc", "QtUic",
                       "ResourceCompile", "None", "Image"):
                src_items.append((tag, inc, cond))

        missing = []
        optional = []
        for tag, inc, cond in src_items:
            resolved = os.path.normpath(os.path.join(proj_dir, inc.replace("\\", os.sep)))
            if os.path.isfile(resolved):
                continue
            # 带 Exists(...) 条件的条目本来就是可选的（例如 bin/yt-dlp.exe 由用户
            # 自行下载），文件不存在时 MSBuild 会直接跳过该条目，不算错误。
            if "Exists(" in cond:
                optional.append((tag, inc, cond.strip()))
            else:
                missing.append((tag, inc))
        print("  ConfigSpecific/Project-relative entries skipped: %d" % len(other_items))
        print("  checked source/resource Include entries: %d" % len(src_items))
        for tag, inc, cond in optional:
            print("  [opt ] <%s Include=\"%s\"> absent, guarded by %s" % (tag, inc, cond))
        if missing:
            failures.append("vcxproj has %d unresolved Include paths" % len(missing))
            for tag, inc in missing[:20]:
                print("  [FAIL] <%s Include=\"%s\"> -> not found" % (tag, inc))
        else:
            print("  [ OK ] every Include path resolves to a real file")

        # 反查：是否有漏掉的源文件（src 下有但 vcxproj 没引用）
        listed = set()
        for _tag, inc, _cond in src_items:
            listed.add(os.path.normcase(os.path.normpath(inc.replace("\\", os.sep))))
        on_disk = set()
        src_root = os.path.join(ROOT, "src")
        for dirpath, _dirs, files in os.walk(src_root):
            for fn in files:
                if fn.lower().endswith((".cpp", ".h", ".hpp", ".cc")):
                    relp = os.path.relpath(os.path.join(dirpath, fn), proj_dir)
                    on_disk.add(os.path.normcase(os.path.normpath(relp)))
        orphan = sorted(on_disk - listed)
        print("  source files on disk: %d / referenced by vcxproj: %d" % (len(on_disk), len(listed & on_disk)))
        if orphan:
            failures.append("%d source files on disk are not referenced by vcxproj" % len(orphan))
            for o in orphan:
                print("  [FAIL] not referenced: %s" % o)
        else:
            print("  [ OK ] no orphan source file")

    # --- filters 的 Filter 组名应当是逻辑分组名，不带 ..\ 前缀 ---
    filt = os.path.join(ROOT, "MelodySync", "MelodySync.vcxproj.filters")
    if os.path.isfile(filt):
        print("\n--- filters group names ---")
        with open(filt, "r", encoding="utf-8") as fh:
            ftext = fh.read()
        bad_filters = re.findall(r'<Filter Include="(\.\..*?)"', ftext)
        if bad_filters:
            failures.append("filters has %d group names polluted with '..\\'" % len(bad_filters))
            for b in bad_filters[:10]:
                print("  [FAIL] <Filter Include=\"%s\"> should be a logical name" % b)
        else:
            print("  [ OK ] all <Filter Include=...> are logical group names")

        # item 项应当带 ..\ 前缀
        items_without = []
        for m in re.finditer(r'<(ClCompile|ClInclude|QtRcc|ResourceCompile|None|Image)\s+Include="([^"]+)"', ftext):
            if not m.group(2).startswith("..") and "$(" not in m.group(2):
                items_without.append(m.group(2))
        if items_without:
            failures.append("filters has %d item entries missing '..\\'" % len(items_without))
            for i in items_without[:10]:
                print("  [FAIL] item path missing '..\\': %s" % i)
        else:
            print("  [ OK ] all item paths carry the '..\\' prefix")

    print("\n" + "=" * 72)
    print("comments scanned: %d" % comments_checked)
    if failures:
        print("RESULT: FAIL (%d)" % len(failures))
        for f in failures:
            print("  - " + f)
        print("=" * 72)
        return 1
    print("RESULT: ALL CHECKS PASSED")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
