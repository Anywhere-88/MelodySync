# -*- coding: utf-8 -*-
"""
源码清单 / 编码一致性校验。

为什么需要它：
  这个工程踩过一次很隐蔽的坑 —— 所有 .cpp/.h 其实都是 UTF-8(无 BOM) 保存的，
  但构建脚本当时关了 /utf-8，于是 MSVC 按系统 ANSI 代码页(中文 Windows = CP936)
  去解析源码，QStringLiteral("中文") 里的 UTF-8 字节被当成 GBK，
  界面中文全部乱码。修复之后必须有一个自动化的守门员，防止有人：

    1) 用记事本另存为把某个文件变成 UTF-8 **带 BOM** 或 GBK；
    2) 新增了 .cpp/.h 却忘了同步 CMakeLists.txt / MelodySync.vcxproj；
    3) 删了文件但清单里还留着（会导致 CMake 配置期直接报错）。

用法：
    python tools/check_sources.py
退出码 0 = 通过。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

BOM_UTF8 = b"\xef\xbb\xbf"
BOM_UTF16_LE = b"\xff\xfe"
BOM_UTF16_BE = b"\xfe\xff"

failures = []
warnings = []


def walk_sources():
    out = []
    for dirpath, dirs, files in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in (".git", "build")]
        for fn in sorted(files):
            if fn.lower().endswith((".cpp", ".h", ".hpp", ".cc", ".cxx")):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def check_encoding(files):
    print("\n--- encoding audit (expect: UTF-8 without BOM) ---")
    bad_bom = []
    bad_utf8 = []
    for f in files:
        with open(f, "rb") as fh:
            raw = fh.read()
        rel = os.path.relpath(f, ROOT).replace("\\", "/")
        if raw.startswith(BOM_UTF8):
            bad_bom.append((rel, "UTF-8 BOM"))
        elif raw.startswith(BOM_UTF16_LE) or raw.startswith(BOM_UTF16_BE):
            bad_bom.append((rel, "UTF-16 BOM"))
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as e:
            bad_utf8.append((rel, str(e)))

    if bad_bom:
        failures.append("%d file(s) carry a BOM" % len(bad_bom))
        for rel, kind in bad_bom:
            print("  [FAIL] %s has %s" % (rel, kind))
    else:
        print("  [ OK ] no BOM in any source file")

    if bad_utf8:
        failures.append("%d file(s) are not valid UTF-8" % len(bad_utf8))
        for rel, err in bad_utf8[:10]:
            print("  [FAIL] %s is not UTF-8: %s" % (rel, err))
    else:
        print("  [ OK ] all %d files decode cleanly as UTF-8" % len(files))

    # 中文注释覆盖率：纯英文工程在这个项目里是反模式（要求中文注释）
    with_cjk = 0
    for f in files:
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
        if re.search(r"[\u4e00-\u9fff]", txt):
            with_cjk += 1
    print("  [info] files containing Chinese comments: %d / %d" % (with_cjk, len(files)))
    if with_cjk < len(files):
        warnings.append("%d file(s) have no Chinese comment" % (len(files) - with_cjk))


def check_cmake_list(files):
    print("\n--- CMakeLists.txt source list vs disk ---")
    cmake = os.path.join(ROOT, "CMakeLists.txt")
    if not os.path.isfile(cmake):
        failures.append("CMakeLists.txt missing")
        print("  [FAIL] CMakeLists.txt not found")
        return
    with open(cmake, "r", encoding="utf-8") as fh:
        text = fh.read()

    listed = set()
    for var in ("MS_SOURCES", "MS_HEADERS"):
        m = re.search(r"set\(\s*%s\s*(.*?)\n\)" % var, text, re.S)
        if not m:
            failures.append("CMakeLists.txt: cannot find set(%s ...)" % var)
            print("  [FAIL] cannot locate set(%s ...) block" % var)
            continue
        for line in m.group(1).splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            listed.add(line.replace("/", os.sep).lower())

    on_disk = set()
    for f in files:
        on_disk.add(os.path.relpath(f, ROOT).replace("/", os.sep).lower())

    print("  listed in CMakeLists : %d" % len(listed))
    print("  on disk under src/   : %d" % len(on_disk))

    missing_on_disk = sorted(listed - on_disk)
    not_listed = sorted(on_disk - listed)

    if missing_on_disk:
        failures.append("CMakeLists references %d nonexistent file(s)" % len(missing_on_disk))
        for m_ in missing_on_disk:
            print("  [FAIL] listed but missing on disk: %s" % m_)
    else:
        print("  [ OK ] every listed file exists")

    if not_listed:
        failures.append("%d file(s) on disk are absent from CMakeLists" % len(not_listed))
        for m_ in not_listed:
            print("  [FAIL] on disk but not listed: %s" % m_)
    else:
        print("  [ OK ] no file left out of the build")


def check_utf8_flag():
    """确认 /utf-8 没有被人手滑删掉。"""
    print("\n--- /utf-8 guard ---")
    targets = [
        ("CMakeLists.txt", os.path.join(ROOT, "CMakeLists.txt")),
        ("MelodySync/MelodySync.vcxproj", os.path.join(ROOT, "MelodySync", "MelodySync.vcxproj")),
    ]
    for label, path in targets:
        if not os.path.isfile(path):
            failures.append("%s missing" % label)
            print("  [FAIL] %s not found" % label)
            continue
        with open(path, "r", encoding="utf-8") as fh:
            txt = fh.read()
        if "/utf-8" in txt:
            print("  [ OK ] %s contains /utf-8" % label)
        else:
            failures.append("%s lost its /utf-8 switch" % label)
            print("  [FAIL] %s has no /utf-8 -> Chinese text will be mojibake" % label)


def check_win_resources():
    """校验 Win32 资源脚本与它引用的图标。

    这条是防「图标链不上」的。两个坑的症状都不明显：
      a) .rc 里引用的 .ico 被移动/改名 -> rc.exe 直接报错终止构建，
         但错误信息只提文件名，不说它相对哪个目录找；
      b) .rc 里混进非 ASCII 字节 -> rc.exe 按系统 ANSI 代码页
         （中文 Windows = CP936）解析脚本，轻则乱码重则编译失败。
    所以前置到自检里，别等构建时才撞上。
    """
    print("\n--- Win32 resource script ---")
    rc = os.path.join(ROOT, "resources", "MelodySync.rc")
    if not os.path.isfile(rc):
        failures.append("resources/MelodySync.rc missing")
        print("  [FAIL] resources/MelodySync.rc not found")
        return

    with open(rc, "rb") as fh:
        raw = fh.read()
    if all(b < 128 for b in raw):
        print("  [ OK ] MelodySync.rc is pure ASCII (safe for rc.exe on any code page)")
    else:
        failures.append("MelodySync.rc contains non-ASCII bytes")
        print("  [FAIL] MelodySync.rc has non-ASCII bytes -> rc.exe may mis-decode it")

    # ICON / BITMAP 等语句引用的文件必须存在（rc.exe 相对 .rc 所在目录查找）
    rc_dir = os.path.dirname(rc)
    with open(rc, "r", encoding="ascii", errors="replace") as fh:
        text = fh.read()
    refs = re.findall(r'^\s*\S+\s+(?:ICON|BITMAP|CURSOR|FONT|HTML)\s+"([^"]+)"',
                      text, re.M | re.I)
    if not refs:
        warnings.append("MelodySync.rc declares no ICON/BITMAP resource")
        print("  [warn] no ICON/BITMAP statement found in MelodySync.rc")
    missing = []
    for rel in refs:
        target = os.path.normpath(os.path.join(rc_dir, rel.replace("/", os.sep)))
        if os.path.isfile(target):
            print("  [ OK ] resource file exists: %s" % rel)
        else:
            missing.append(rel)
    if missing:
        failures.append("%d resource file(s) referenced by .rc do not exist" % len(missing))
        for rel in missing:
            print("  [FAIL] referenced by .rc but missing: %s" % rel)

    # 两个构建入口都必须真的把它编进去，否则只改一边 = 另一种构建方式没图标
    cmake = os.path.join(ROOT, "CMakeLists.txt")
    with open(cmake, "r", encoding="utf-8") as fh:
        cm = fh.read()
    if "MS_WIN_RESOURCES" in cm and "MelodySync.rc" in cm:
        print("  [ OK ] CMakeLists.txt wires MelodySync.rc into the target")
    else:
        failures.append("CMakeLists.txt does not build MelodySync.rc")
        print("  [FAIL] CMakeLists.txt does not build MelodySync.rc")

    vcx = os.path.join(ROOT, "MelodySync", "MelodySync.vcxproj")
    with open(vcx, "r", encoding="utf-8") as fh:
        vx = fh.read()
    if 'ResourceCompile Include="..\\resources\\MelodySync.rc"' in vx:
        print("  [ OK ] MelodySync.vcxproj registers MelodySync.rc")
    else:
        failures.append("MelodySync.vcxproj does not register MelodySync.rc")
        print("  [FAIL] MelodySync.vcxproj has no ResourceCompile for MelodySync.rc")


def check_includes(files):
    """所有 #include "xxx.h" 都必须能落到真实文件上。

    解析规则和 MSVC 一致：先按「相对当前文件所在目录」找，再按
    AdditionalIncludeDirectories（这里是 src/）找。
    """
    print("\n--- quoted #include resolution ---")
    inc_re = re.compile(r'#\s*include\s+"([^"]+)"')
    unresolved = []
    total = 0
    for f in files:
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
        for m in inc_re.finditer(txt):
            total += 1
            hdr = m.group(1)
            cands = [os.path.join(os.path.dirname(f), hdr), os.path.join(SRC, hdr)]
            if not any(os.path.isfile(c) for c in cands):
                unresolved.append((os.path.relpath(f, ROOT).replace("\\", "/"), hdr))

    print("  quoted includes scanned: %d" % total)
    if unresolved:
        failures.append("%d quoted #include directive(s) cannot be resolved" % len(unresolved))
        for f, hdr in unresolved:
            print("  [FAIL] %s -> \"%s\" not found" % (f, hdr))
    else:
        print("  [ OK ] every quoted include resolves to an existing file")


def check_ms_qualification(files):
    """非 ms 命名空间的 .cpp 里，引用 ms 的类型必须写 ms:: 或有 using 指令。

    这一条是有代价换来的：main.cpp 是唯一一个不在 namespace ms 里的翻译单元，
    它却直接写了 HttpClient::init() / Database::instance() /
    Settings::instance() / FFmpegTranscoder::init()。这些名字都声明在
    namespace ms 内，编译器只会给出 C2653「不是类或命名空间名称」，
    报错位置和真实原因隔了好几层。写成自动检查就能一眼定位。
    """
    print("\n--- namespace ms qualification ---")

    # 1) 从所有头文件收集 ms 里的类/结构体名
    known = set()
    for f in files:
        if not f.lower().endswith((".h", ".hpp")):
            continue
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
        if "namespace ms" not in txt:
            continue
        for m in re.finditer(r"^\s*(?:class|struct)\s+(\w+)\s*(?::|\{|$)", txt, re.M):
            known.add(m.group(1))
    known.discard("ms")

    offenders = []
    for f in files:
        if not f.lower().endswith((".cpp", ".cc", ".cxx")):
            continue
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
        # 已经身处 ms 命名空间，或显式引入过 ms，就都没问题
        if re.search(r"^\s*namespace\s+ms\s*\{", txt, re.M) or "using namespace ms" in txt:
            continue
        for name in sorted(known):
            if re.search(r"(?<![\w:])" + re.escape(name) + r"\s*::", txt):
                offenders.append((os.path.relpath(f, ROOT).replace("\\", "/"), name))

    print("  ms types discovered in headers: %d" % len(known))
    if offenders:
        failures.append("%d unqualified ms:: reference(s) in files outside namespace ms" % len(offenders))
        for f, name in offenders:
            print("  [FAIL] %s references %s:: without qualifier / using directive" % (f, name))
    else:
        print("  [ OK ] every non-ms translation unit qualifies or imports ms")


def check_bridge_contract():
    """WebBridge 的 Q_INVOKABLE 必须都有定义；信号转发两侧签名必须一致。

    QWebChannel 桥接是 JS 唯一入口，漏一个定义就是链接期 LNK2019；
    信号转发签名写错则编译期报 connect 失败，但错误信息非常难读。
    """
    print("\n--- WebBridge JS contract ---")
    hdr = os.path.join(SRC, "bridge", "WebBridge.h")
    cpp = os.path.join(SRC, "bridge", "WebBridge.cpp")
    if not (os.path.isfile(hdr) and os.path.isfile(cpp)):
        print("  [skip] WebBridge not found")
        return
    with open(hdr, "r", encoding="utf-8") as fh:
        h = fh.read()
    with open(cpp, "r", encoding="utf-8") as fh:
        c = fh.read()

    declared = re.findall(r"Q_INVOKABLE\s+[\w:<>&*\s]*?(\w+)\s*\(", h)
    missing = [d for d in declared if not re.search(r"\bWebBridge::" + re.escape(d) + r"\s*\(", c)]
    print("  Q_INVOKABLE methods: %d" % len(declared))
    if missing:
        failures.append("%d Q_INVOKABLE method(s) declared but not defined" % len(missing))
        for m_ in missing:
            print("  [FAIL] WebBridge::%s() has no definition -> LNK2019" % m_)
    else:
        print("  [ OK ] every Q_INVOKABLE has a definition")

    def sig_table(path):
        with open(path, "r", encoding="utf-8") as fh:
            t = fh.read()
        out = {}
        for m in re.finditer(r"void\s+(\w+)\s*\(([^;{]*)\)\s*;", t):
            out[m.group(1)] = " ".join(m.group(2).split())
        return out

    wb = sig_table(hdr)
    forward = [
        ("downloadProgress", "src/download/DownloadEngine.h", "progress"),
        ("aiSearchProgress", "src/search/SearchAggregator.h", "sourceProgress"),
        ("aiSourceHits", "src/search/SearchAggregator.h", "sourceResults"),
    ]
    checked = 0
    for dst, src_rel, src_name in forward:
        src_path = os.path.join(ROOT, src_rel.replace("/", os.sep))
        if not os.path.isfile(src_path) or dst not in wb:
            continue
        src_sigs = sig_table(src_path)
        if src_name not in src_sigs:
            continue
        checked += 1
        if wb[dst] != src_sigs[src_name]:
            failures.append(
                "signal forwarding signature mismatch: %s vs %s" % (dst, src_name))
            print("  [FAIL] %s(%s)" % (dst, wb[dst]))
            print("         != %s(%s)" % (src_name, src_sigs[src_name]))
    print("  signal forwarding pairs verified: %d" % checked)


def check_frontend_bridge_await():
    """前端「调方法 + 等信号」的**顺序**守门。

    _waitSignal() 只做一件事：**注册监听器**并返回一个 Promise。真正的调用必须
    由调用方在「注册之后、await 之前」发出，也就是三段式：

        const p = _waitSignal('settingsLoaded', 8000);   // 1) 先注册
        bridge.getSettings();                            // 2) 再调用
        const r = await p;                               // 3) 后等待
        return r ? JSON.parse(r) : null;

    写错的两种方式症状完全不同，而且两种都真实发生过：

    A) 漏 await（直接 JSON.parse(p)）
       -> JSON.parse(Promise) 抛 SyntaxError，用户一点界面就炸。
          日志指纹：Unexpected token 'o', "[object Promise]" is not valid JSON

    B) 把 await 加到 _waitSignal 前面（await _waitSignal(...)）
       -> 函数**卡在等信号**上，而信号要等第 2 步的调用才可能发出 —— 第 2 步
          永远执行不到，形成死锁。必须等满超时才发出请求，那时监听器早已被移除，
          拿到的永远是 null。
       -> B 比 A 隐蔽得多：不再报错，界面只是「点了没反应 / 一直在转」；后端日志里
          请求**确实存在**，只是比用户点击晚了整整一个超时周期（解析 40s、搜索 120s），
          极易被误判成「网络慢」而往别处查。
    """
    print("\n--- frontend bridge wait order ---")
    path = os.path.join(ROOT, "ui", "index.html")
    if not os.path.isfile(path):
        failures.append("ui/index.html missing")
        print("  [FAIL] ui/index.html not found")
        return

    with open(path, "r", encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    calls = 0
    bad_order = []      # 情况 B：await 加在了注册前面
    bad_await = []      # 情况 A：结果从未被 await
    for i, line in enumerate(lines):
        if "function _waitSignal" in line:
            continue
        if "await _waitSignal(" in line:
            bad_order.append((i + 1, line.strip()))
            continue
        if "_waitSignal(" not in line:
            continue
        calls += 1
        m = re.search(r"(\w+)\s*=\s*_waitSignal\(", line)
        if not m:
            bad_order.append((i + 1, line.strip()))
            continue
        var = m.group(1)
        # 窗口 = 本行 .. 下一个 _waitSignal 调用点之前。
        # 不能取固定行数：门面里是「一行一个方法」，相隔 8 行会撞上隔壁方法的
        # await，从而把「漏 await」误判成通过（注入测试时踩过一次）。
        end = i + 1
        while end < len(lines) and "_waitSignal(" not in lines[end]:
            end += 1
        window = "\n".join(lines[i:end])
        if not re.search(r"\bawait\s+" + re.escape(var) + r"\b", window):
            bad_await.append((i + 1, var, line.strip()))

    print("  _waitSignal call sites: %d" % calls)
    if bad_order:
        failures.append("ui/index.html: await placed before the call (%d site(s))"
                        % len(bad_order))
        for ln, txt in bad_order[:6]:
            print("  [FAIL] line %d: await 加在了 _waitSignal 前面" % ln)
            print("         %s" % txt[:100])
        print("         顺序必须是「先注册 -> 再调用 -> 后 await」。写成 await _waitSignal(...)")
        print("         会死锁：信号要等后面的 bridge.xxx() 才可能发出，而那行永远执行不到，")
        print("         只能等满超时（8~120 秒）—— 现象是「点了没反应」，后端日志里请求")
        print("         会比用户点击晚整整一个超时周期才出现")

    if bad_await:
        failures.append("ui/index.html: %d _waitSignal result never awaited" % len(bad_await))
        for ln, var, txt in bad_await[:6]:
            print("  [FAIL] line %d: 变量 '%s' 从未 await" % (ln, var))
            print("         %s" % txt[:100])
        print("         JSON.parse(Promise) 会抛 SyntaxError（日志指纹：[object Promise]）")

    if not bad_order and not bad_await:
        print("  [ OK ] every _waitSignal is register -> call -> await")


def check_bridge_early_return():
    """桥接方法「提前返回也必须发信号」的守门检查。

    前端取结果靠「调方法 + 等信号」，**只看信号，不看返回值**（QWebChannel 的
    Q_INVOKABLE 返回值在 JS 侧同步拿不到，见 README §5）。所以任何提前 return
    （参数为空、未登录、模块未初始化……）都必须先把同一条 JSON 通过对应信号发出去。

    漏发的后果不是报错而是**卡住**：前端会一直等到超时（8～40 秒）才回落，用户感受是
    「点了没反应，十几秒后才弹错」，而不是立刻看到原因。

    判定范围 = 该错误构造所在**语句块起点** .. 它之后的**第一个 return**。
    这两个边界都是踩出来的：
      - 往前要找块首 —— `emit sig(j); return j;` 这种写法里 emit 在错误行**之前**，
        只看后面会误报；
      - 往后只到第一个 return —— 再往后的 emit 属于正常路径，算进来就会漏报。
    改这段逻辑后务必做一次注入式测试（见文件末尾注释）。

    接受两种写法：
        return MS_EARLY_ERR(sig, "…");            # 宏（推荐）
        emit sig(j); return j;                    # 手工配对
    """
    print("\n--- bridge early return ---")
    ui_path = os.path.join(ROOT, "ui", "index.html")
    cpp_path = os.path.join(ROOT, "src", "bridge", "WebBridge.cpp")
    for p in (ui_path, cpp_path):
        if not os.path.isfile(p):
            failures.append("missing %s" % os.path.relpath(p, ROOT))
            print("  [FAIL] %s not found" % os.path.relpath(p, ROOT))
            return

    # 1) 从前端解析出「桥接方法 → 它等待的信号」
    with open(ui_path, "r", encoding="utf-8") as fh:
        ui = fh.read().split("\n")
    mapping = {}
    for i, line in enumerate(ui):
        m_sig = re.search(r"_waitSignal\('(\w+)'", line)
        if not m_sig:
            continue
        # 同行优先（门面的单行写法），否则往后 3 行（两行写法）。
        # 不能往前找 —— 会撞上上一个方法里的 bridge 调用，映射会整体错位。
        probe = line + "\n" + "\n".join(ui[i + 1:i + 4])
        m_m = re.search(r"bridge\.(\w+)\(", probe)
        if m_m:
            mapping[m_m.group(1)] = m_sig.group(1)

    # 2) 在 WebBridge.cpp 里按方法体扫描
    with open(cpp_path, "r", encoding="utf-8") as fh:
        cpp = fh.read()
    lines = cpp.split("\n")

    def body_range(method):
        m = re.search(r"WebBridge::" + re.escape(method) + r"\s*\(", cpp)
        if not m:
            return None
        brace = cpp.find("{", m.end())
        if brace < 0:
            return None
        depth, j = 0, brace
        while j < len(cpp):
            if cpp[j] == "{":
                depth += 1
            elif cpp[j] == "}":
                depth -= 1
                if depth == 0:
                    return cpp[:brace].count("\n"), cpp[:j].count("\n")
            j += 1
        return None

    bad = []
    checked = 0
    for method, sig in sorted(mapping.items()):
        r = body_range(method)
        if not r:
            continue
        checked += 1
        a, b = r
        body = lines[a:b + 1]
        for k, ln in enumerate(body):
            if "err(QStringLiteral(" not in ln:
                continue
            if not (re.search(r"\breturn\b", ln) or "= toCompact(err(" in ln):
                continue                       # 不是"准备把错误返回出去"的位置
            if "MS_EARLY_ERR" in ln:
                continue                       # 宏已经发过信号了

            start = k
            for j in range(k, max(-1, k - 12), -1):
                if "{" in body[j]:
                    start = j
                    break
            end = k
            for j in range(k, min(k + 15, len(body))):
                end = j
                if re.search(r"\breturn\b", body[j]):
                    break
            span = "\n".join(body[start:end + 1])
            if re.search(r"\bemit\s+" + re.escape(sig) + r"\s*\(", span):
                continue
            bad.append((method, sig, a + k + 1))

    print("  methods whose signals the frontend waits on: %d" % checked)
    if bad:
        failures.append("WebBridge.cpp has %d early return(s) that never emit" % len(bad))
        for method, sig, lineno in bad:
            print("  [FAIL] %s() line %d: early return without 'emit %s(...)'"
                  % (method, lineno, sig))
        print("         前端只监听信号、不看返回值 —— 漏发会让它白等到超时，"
              "现象是「点了没反应，十几秒后才弹错」")
    else:
        print("  [ OK ] every early return emits its signal")
        print("         （或用 MS_EARLY_ERR 宏）")


def check_embedded_frontend():
    """确认 exe 里内嵌的前端与磁盘上的 ui/index.html 是同一份。

    本工程最容易犯的错之一：改了 `ui/index.html` 却忘了重新编译。前端是通过 qrc
    嵌进 exe 的，不重编译改动**不会生效**，而现象是「我明明改了怎么没用」——
    排查时很容易怀疑到别的地方去（今天就差点被这个绕进去）。

    只比较长度就够了，不必解压：rcc 会把原始长度写进资源头，
    长度不等一定不是同一份内容。
    只在已经构建过的情况下检查，没构建过就跳过。
    """
    print("\n--- embedded frontend freshness ---")
    ui = os.path.join(ROOT, "ui", "index.html")
    rcc = os.path.join(ROOT, "build", ".rcc", "qrc_melodysync_resources.cpp")
    if not os.path.isfile(ui):
        failures.append("ui/index.html missing")
        print("  [FAIL] ui/index.html not found")
        return
    if not os.path.isfile(rcc):
        print("  [skip] 还没构建过（找不到 build/.rcc/…）—— 先跑 tools/build.py")
        return

    with open(ui, "rb") as fh:
        disk_len = len(fh.read())
    with open(rcc, "r", encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    data = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{1,2})\b", src))
    if len(data) < 8:
        failures.append("rcc output looks empty")
        print("  [FAIL] rcc 产物异常（不足 8 字节）")
        return

    # rcc 条目两种格式：压缩时 [0:4]=压缩后长度、[4:8]=原始长度；
    # 未压缩直存时 [0:4] 即原始长度，[4:8] 起就是正文（此时读 [4:8] 会把
    # 正文前四个字节拼成垃圾数，比如 "<!DO" → 1008813135）。两种都兼容。
    len_uncomp = int.from_bytes(data[0:4], "big")
    if len_uncomp == disk_len:
        embedded_len = len_uncomp
    else:
        embedded_len = int.from_bytes(data[4:8], "big")     # 压缩格式：原始长度在第 2 个 u32
    if embedded_len != disk_len:
        failures.append("ui/index.html changed but not rebuilt "
                        "(embedded %d vs disk %d bytes)" % (embedded_len, disk_len))
        print("  [FAIL] 内嵌 %d 字节 ≠ 磁盘 %d 字节 —— 改过前端但没重新编译"
              % (embedded_len, disk_len))
        print("         跑一次 tools/build.py（或 build.bat），改动才会进 exe")
        return
    print("  [ OK ] 内嵌前端与 ui/index.html 一致（%d 字节）" % disk_len)


def _cmake_runtime_output_dir():
    """从 CMakeLists.txt 读出实际的可执行文件输出目录。

    不硬编码 "bin"：这个值以后被改成 bin2/ 之类的时候，检查要跟着变，
    而不是继续拿旧路径去骗人说「没问题」。
    """
    cm = os.path.join(ROOT, "CMakeLists.txt")
    if not os.path.isfile(cm):
        return None
    with open(cm, "r", encoding="utf-8") as fh:
        txt = fh.read()
    m = re.search(r'set\s*\(\s*CMAKE_RUNTIME_OUTPUT_DIRECTORY\s+'
                  r'\$\{CMAKE_BINARY_DIR\}/([\w./-]+)\s*\)', txt)
    return m.group(1) if m else None


def check_launcher_paths():
    """启动器 / 快捷方式里写死的产物路径，必须和 CMakeLists 一致。

    这一条是被真事故逼出来的：曾经桌面快捷方式指向
    build/bin/Release/MelodySync.exe，但这个工程两个配置的产物同名同地
    （CMake 的 per-config 变量和 vcxproj 的 OutDir 都钉在 build/bin/ 下），
    根本没有 Release/ 子目录 —— 双击快捷方式只会得到「找不到文件」。
    路径散落在三个文件里，靠人眼对齐迟早会漂，所以拿机器对。
    """
    print("\n--- launcher / shortcut exe path consistency ---")

    out_sub = _cmake_runtime_output_dir()
    if out_sub is None:
        warnings.append("cannot read CMAKE_RUNTIME_OUTPUT_DIRECTORY from CMakeLists.txt")
        print("  [WARN] CMAKE_RUNTIME_OUTPUT_DIRECTORY not found, skipping")
        return

    want_rel = out_sub.replace("/", os.sep) + os.sep + "MelodySync.exe"
    print("  CMakeLists says the exe lives at: <build>/%s" % want_rel.replace(os.sep, "/"))
    print("  (no Release/Debug sub-directory: both configs share one output dir)")

    def pos_in_build_dir(rel):
        """取「构建目录内的相对位置」，用于把两边的写法对齐后再比。

        两边的写法本来就不一样：
          CMakeLists 给的是构建目录内部的位置  -> bin/MelodySync.exe
          脚本里给的是相对工程根的位置        -> build/bin/MelodySync.exe
        所以统一取**最后两级**（输出子目录 + 文件名）再比较：
          build/bin/MelodySync.exe            -> bin/MelodySync.exe  ✅
          bin/MelodySync.exe                  -> bin/MelodySync.exe  ✅
          build/bin/Release/MelodySync.exe    -> Release/MelodySync.exe  ❌ 正是要抓的 bug

        用「最后两级」而不是「剥掉第一级」，是因为构建目录名由 -B 参数决定，
        CMakeLists 里根本没有；按名字匹配会在有人用 -B out 时误报。
        """
        parts = [p for p in rel.replace("/", os.sep).split(os.sep) if p]
        return os.sep.join(parts[-2:]) if len(parts) >= 2 else rel

    want_pos = pos_in_build_dir(want_rel)

    # 1) launch.py 里的 EXE_PATH
    lp = os.path.join(ROOT, "tools", "launch.py")
    if not os.path.isfile(lp):
        failures.append("tools/launch.py is missing")
        print("  [FAIL] tools/launch.py not found")
    else:
        with open(lp, "r", encoding="utf-8") as fh:
            lt = fh.read()
        # 抓 os.path.join("build", "bin", "MelodySync.exe") 这种字面量
        parts = re.findall(r'os\.path\.join\(\s*ROOT\s*,\s*([^)]*)\)', lt)
        found = None
        for p in parts:
            lits = re.findall(r'"([^"]+)"', p)
            if lits and lits[-1].lower().endswith(".exe"):
                found = os.path.join(*lits)
                break
        if found is None:
            warnings.append("cannot parse EXE_PATH out of tools/launch.py")
            print("  [WARN] could not locate EXE_PATH literal in launch.py")
        elif pos_in_build_dir(found) != want_pos:
            failures.append("launch.py EXE_PATH (%s) != CMake output (%s)"
                            % (found, want_rel))
            print("  [FAIL] launch.py EXE_PATH = %s" % found)
            print("         expected position inside the build dir: %s" % want_pos)
        else:
            print("  [ OK ] launch.py EXE_PATH matches CMake output")

        # 2) 启动器引用的前端预览文件必须存在
        for rel in ("ui/index.html",):
            if os.path.isfile(os.path.join(ROOT, *rel.split("/"))):
                print("  [ OK ] launch.py preview target exists: %s" % rel)
            else:
                failures.append("launch.py points at a missing file: %s" % rel)
                print("  [FAIL] launch.py preview target missing: %s" % rel)

    # 3) make_shortcut.py 的 --target 默认值
    ms = os.path.join(ROOT, "tools", "make_shortcut.py")
    if not os.path.isfile(ms):
        failures.append("tools/make_shortcut.py is missing")
        print("  [FAIL] tools/make_shortcut.py not found")
    else:
        with open(ms, "r", encoding="utf-8") as fh:
            mt = fh.read()
        m = re.search(r'"--target".*?default=os\.path\.join\(([^)]*)\)', mt, re.S)
        if not m:
            warnings.append("cannot parse --target default out of make_shortcut.py")
            print("  [WARN] could not parse --target default")
        else:
            lits = re.findall(r'"([^"]+)"', m.group(1))
            got = os.path.join(*lits)
            if pos_in_build_dir(got) != want_pos:
                failures.append("make_shortcut.py --target default (%s) != CMake output (%s)"
                                % (got, want_rel))
                print("  [FAIL] make_shortcut.py --target default = %s" % got)
                print("         expected position inside the build dir: %s" % want_pos)
            else:
                print("  [ OK ] make_shortcut.py --target default matches CMake output")


def check_ytdlp_sidecars():
    """yt-dlp 的两份副本必须一致；fetch 脚本必须写进仓库的 bin/。

    这是被真事故逼出来的（2026-09-19）：
      CMake 的 POST_BUILD 会执行 `bin\\yt-dlp.exe -> build\\bin\\yt-dlp.exe`。
      手工把修好的 yt-dlp 只放进 build/bin/ ，下一次构建就被 bin/ 里的旧文件
      盖回去 —— 现象是"我明明修好了，跑起来还报 yt-dlp 已损坏"，来回找很久。
    bin/ 才是唯一的手工维护点，build/bin/ 只是部署副本，所以拿机器对它们的哈希。

    另外一件必须守住的事：yt-dlp 是不是**损坏**，只看大小判断不了 ——
    曾经那份坏的有 18,903,582 字节，比官方正常的 17,840,399 还大（中间被塞了
    脏数据），一启动就报 PyInstaller 解包失败。完整校验（SHA-256 + 实跑）交给
    `python tools/fetch_ytdlp.py --check`，这里只守住"两份必须一样"。
    """
    print("\n--- yt-dlp sidecar consistency ---")

    # 1) fetch 脚本的默认落点必须在仓库 bin/，不能是 build/bin/
    fp = os.path.join(ROOT, "tools", "fetch_ytdlp.py")
    if not os.path.isfile(fp):
        warnings.append("tools/fetch_ytdlp.py missing")
        print("  [WARN] tools/fetch_ytdlp.py not found")
    else:
        with open(fp, "r", encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
        m = re.search(r"^\s*dest\s*=\s*args\.dest\s+or\s+([^\n]+)", txt, re.M)
        if not m:
            warnings.append("cannot parse default dest out of tools/fetch_ytdlp.py")
            print("  [WARN] 解析不到 fetch_ytdlp.py 的默认目标")
        else:
            lits = re.findall(r'"([^"]+)"', m.group(1))
            if "build" in lits:
                failures.append("fetch_ytdlp.py writes into build/bin by default; "
                                "it must write the repo bin/ (the deploy copy is overwritten "
                                "by every build)")
                print("  [FAIL] fetch_ytdlp.py 默认写入 build/bin —— 下次构建会被覆盖")
                print('         应该写成 os.path.join(root, "bin", ASSET)')
            elif "bin" in lits:
                print("  [ OK ] fetch_ytdlp.py 默认写入仓库 bin/")
            else:
                warnings.append("fetch_ytdlp.py default dest is not bin/")
                print("  [WARN] fetch_ytdlp.py 默认目标看着不像 bin/")

    # 2) 两份副本的哈希必须一致
    src = os.path.join(ROOT, "bin", "yt-dlp.exe")
    dep = os.path.join(ROOT, "build", "bin", "yt-dlp.exe")
    if not os.path.isfile(src) and not os.path.isfile(dep):
        print("  [skip] 还没有 yt-dlp.exe（跑 tools/fetch_ytdlp.py 获取）")
    elif not os.path.isfile(src):
        failures.append("build/bin/yt-dlp.exe exists but bin/yt-dlp.exe is missing "
                        "(bin/ is the source of truth; the deploy copy will be wiped)")
        print("  [FAIL] 只有 build/bin/yt-dlp.exe，仓库 bin/ 里没有")
        print("         下次构建会以 bin/ 为准，这里必须先放一份")
    elif not os.path.isfile(dep):
        print("  [skip] 还没构建过（找不到 build/bin/yt-dlp.exe）")
    else:
        import hashlib

        def _sha(p):
            h = hashlib.sha256()
            with open(p, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            return h.hexdigest()

        hs, hd = _sha(src), _sha(dep)
        if hs != hd:
            failures.append("bin/yt-dlp.exe and build/bin/yt-dlp.exe differ "
                            "(a rebuild will silently revert one of them)")
            print("  [FAIL] 两份 yt-dlp.exe 不一致")
            print("         bin/       = %s (%d 字节)" % (hs[:16], os.path.getsize(src)))
            print("         build/bin/ = %s (%d 字节)" % (hd[:16], os.path.getsize(dep)))
            print("         跑 tools/fetch_ytdlp.py 会同时更新这两份")
        else:
            print("  [ OK ] 两份 yt-dlp.exe 一致（sha256 %s…，%d 字节）"
                  % (hs[:12], os.path.getsize(src)))


def check_platform_direct_links():
    """平台源（网易云 / QQ）"能拿到可下载直链"这条契约不能悄悄退化。

    背景：这两个源曾经被硬编码成 `previewOnly = true` + 清空 downloadUrl，
    理由是"不伪造登录态"。结果用户在客户端看到的是**一片灰色的「仅试听」**，
    下不了任何东西，而且界面上完全看不出原因。

    改成走 PlatformResolve 之后有三件事必须一直成立，任何一条被改回去，
    症状都是"搜到了但全是仅试听"或"下载全部 403"，且很难联想到这里：
      1) 搜索阶段要真的去换直链；
      2) 下载阶段要**重新**换一次（平台直链有 20 分钟时效，排队后就过期）；
      3) 直链请求必须带 Referer（不带会被 CDN 403，报错看不出原因）。
    """
    print("\n--- platform direct-link contract ---")

    pv = os.path.join(SRC, "search", "sources", "PreviewSource.cpp")
    if not os.path.isfile(pv):
        failures.append("src/search/sources/PreviewSource.cpp missing")
        print("  [FAIL] PreviewSource.cpp 不存在")
    else:
        with open(pv, "r", encoding="utf-8", errors="replace") as fh:
            t = fh.read()
        # 1) 搜索阶段必须换直链
        need = [("PlatformResolve::neteaseBatch", "网易云"),
                ("PlatformResolve::qqBatch", "QQ 音乐")]
        missing = [n for n, _ in need if n not in t]
        if missing:
            failures.append("PreviewSource no longer resolves platform direct links: %s"
                            % ", ".join(missing))
            print("  [FAIL] 搜索阶段没在换平台直链，缺: %s" % ", ".join(missing))
            print("         改回去的症状：结果全是「仅试听」")
        else:
            print("  [ OK ] PreviewSource 会为网易云 / QQ 换官方直链")

        # 2) 不能再出现"把直链清空"的写法
        if re.search(r"downloadUrl\.clear\(\)", t):
            failures.append("PreviewSource clears downloadUrl again "
                            "(platform results will become preview-only)")
            print("  [FAIL] 又出现 downloadUrl.clear() —— 这会让结果全部退回仅试听")
        else:
            print("  [ OK ] 没有清空 downloadUrl 的写法")

    de = os.path.join(SRC, "download", "DownloadEngine.cpp")
    if not os.path.isfile(de):
        failures.append("src/download/DownloadEngine.cpp missing")
        print("  [FAIL] DownloadEngine.cpp 不存在")
        return
    with open(de, "r", encoding="utf-8", errors="replace") as fh:
        d = fh.read()

    # 3) 下载前必须重解析（直链有时效）
    if "PlatformResolve::supports" not in d or "platformId" not in d:
        failures.append("DownloadEngine does not re-resolve platform links before download "
                        "(queue stores expiring URLs -> 403 after a few minutes)")
        print("  [FAIL] DownloadEngine 没有在下载前重新换直链")
        print("         平台直链 20 分钟过期，排队后再下必然 403")
    else:
        print("  [ OK ] DownloadEngine 下载前会重新解析平台直链")

    # 4) 直链请求头（Referer）必须带上
    if "headersFor" not in d:
        failures.append("DownloadEngine does not use PlatformResolve::headersFor "
                        "(platform CDN returns 403 without Referer)")
        print("  [FAIL] 下载直链时没带平台要求的 Referer")
    else:
        print("  [ OK ] 下载平台直链时带上了对应 Referer")


def main():
    print("=" * 72)
    print("source list / encoding consistency check")
    print("root =", ROOT)
    print("=" * 72)

    files = walk_sources()
    print("\nsource files found under src/: %d" % len(files))

    check_encoding(files)
    check_cmake_list(files)
    check_utf8_flag()
    check_win_resources()
    check_includes(files)
    check_ms_qualification(files)
    check_bridge_contract()
    check_frontend_bridge_await()
    check_bridge_early_return()
    check_embedded_frontend()
    check_launcher_paths()
    check_ytdlp_sidecars()
    check_platform_direct_links()

    print("\n" + "=" * 72)
    if failures:
        print("RESULT: FAIL (%d)" % len(failures))
        for f in failures:
            print("  - " + f)
        print("=" * 72)
        return 1
    print("RESULT: ALL CHECKS PASSED")
    if warnings:
        print("warnings:")
        for w in warnings:
            print("  ~ " + w)
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
