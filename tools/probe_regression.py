# -*- coding: utf-8 -*-
"""
搜索 / 换源回归探针（手动抽查，**不要进 check.bat**）。

一个脚本管两类契约 —— 它们是用户报过的同一类问题的两个面：

  A) 换源型（`CASES`）：「下错版本」
     换源要么下到**时长对得上**的版本，要么**明确放弃**；绝不静默下错的。

  B) 搜索型（`SEARCH_CASES`）：「搜不到原唱」
     平台原唱下不了时（会员曲），必须**真的去全网**找能下载的版本，
     并且结果里必须有能下的好结果。

为什么需要它：
  "换源下载下错版本"是一个反复出现、却长期没有自动验证覆盖的问题类。
  它骗过的不只是用户 —— 2026-09-23 复盘时发现，`--probe-fallback` 只能挑
  "搜索结果里恰好没有直链"的条目，而**歌单任务的字段直接来自平台曲库、
  根本不经过搜索结果**，所以那条最容易出错的路径从来没被测过。
  （同期还踩到一个验证陷阱：`DownloadEngine` 里"目标文件已存在且 >64KB
  就当完成"的早退会把上一次的旧产物认领成本次成功，日志上看着像"换源秒成"。
  本脚本因此**每跑一个换源用例前都把产物目录清空到一个归档子目录**。）

A) 换源型的断言只有三条（不依赖具体分数）：
  1. 若发生了下载 → 实际时长与目标偏差 ≤ 容差（默认 20%），否则判「下错版本」；
  2. 若发生了下载 → 它选中的候选相关度必须 ≥ 60（不能低于自己的门槛还硬下）；
  3. 若明确放弃 → 最高候选相关度必须 < 60（否则是"有分却不选"）。
  另外 `must_find=True` 的用例额外要求"最高候选 ≥ 60" ——
  这一条正是 2026-09-23 那次回归的探针：改坏 `similarity()` 后，正确候选
  只剩 2~13 分、全被放弃，只有 must_find 能把它抓出来。

B) 搜索型的断言见 `SEARCH_CASES` 上方注释。

用法：
    python tools\\probe_regression.py                      # 跑全部用例
    python tools\\probe_regression.py --type search        # 只跑搜索型
    python tools\\probe_regression.py --type fallback      # 只跑换源型
    python tools\\probe_regression.py --only sugar         # 用例名含 sugar 的
    python tools\\probe_regression.py -v                   # 打印候选/结果明细
退出码 0 = 全部通过。

前提：build\\bin\\MelodySync.exe 已构建；联网；yt-dlp 可用。
注意：依赖第三方站点（哔哩哔哩）的当期内容，分数会漂移 ——
      这是抽查工具，不是门禁，**别加进 check.bat**。
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "MelodySync.exe")
LOGDIR = os.path.join(os.environ.get("APPDATA", ""), "MelodySync", "MelodySync", "logs")
LOG = os.path.join(LOGDIR, "melodysync.log")
PROBE_DIR = os.path.join(tempfile.gettempdir(), "melodysync-probe")

TOLERANCE = 0.20          # 时长容差
MIN_SCORE = 60            # 换源候选采纳门槛（与 DownloadEngine 保持一致）

# ---------------------------------------------------------------------------
# 用例：字段取自真实歌单（users.db），artist 是多歌手串 —— 这正是当初出错的地方。
# duration 是歌单里的原曲时长，用于校验收下来的到底是不是那一版。
# ---------------------------------------------------------------------------
CASES = [
    {
        "name": "why-we-lose",
        "artist": "Cartoon / Jéja / Coleman Trapp",
        "title": "Why We Lose",
        "duration": 197,
        "must_find": True,
        "why": "曾搜成『挖掘机儿童玩具车/20 秒印度卡通』；多歌手串 3 段",
        "regression": "歌手不出现在视频标题里（视频站标题写 Cartoon、平台写三串）",
    },
    {
        "name": "checklist",
        "artist": "MAX / Chromeo",
        "title": "Checklist",
        "duration": 135,
        "must_find": True,
        "why": "歌名包在书名号里，曾被 similarity() 连内容删掉 → 正确版本只剩 2 分",
        "regression": "书名号/引号只删符号保留内容；feat 后缀摊平",
    },
    {
        "name": "sugar",
        "artist": "Maroon 5",
        "title": "Sugar",
        "duration": 235,
        "must_find": False,
        "why": "曾被『甜蜜神曲』合集（295s）以 72 分骗过 → 下错版本；现在应拒下",
        "regression": "标题党包装词惩罚 + 包含奖励按覆盖率给分",
    },
    # 为什么没有"干净的对照组"用例：
    #   曾想用 `Glass Animals — The Other Side Of Paradise`（它走第一层搜索时 98 分）
    #   当对照组，实测**必然失败且不是回归** —— 那句 98 分来自网易云（tier 0），
    #   而本脚本强制走换源（全网/B站），B站只有
    #     「Glass Animals《The Other Side Of Paradise》百万豪装录音棚大声听」
    #   这类器材试听视频，`录音棚` 命中 kBadContentPattern（nameSim ×0.35），
    #   实测 nameSim 0.756 → 0.265 → 名称9 + 歌名闸门 0.25 → 相关度 12 分。
    #   **这是刻意设计的行为**（"视频主体是这首歌在这套设备上听起来怎么样"），
    #   不是评分塌陷。所以"对照组"的职责改由上面两个 must_find 用例承担 ——
    #   它们同样能抓住"全都拒绝"式的假绿，而且不依赖站点当期内容。
]

# ---------------------------------------------------------------------------
# 搜索型用例：验证"一次搜索能不能给出可下载的原唱"。
#
# 上面的换源用例测的是「下错版本」，这一类测的是「搜不到原唱」—— 2026-09-23
# 用户报的《だから僕は音楽を辞めた》正是后者：
#   · 网易云上ヨルシカ原唱是会员曲（接口回 code=-110），QQ 上同样是会员曲
#     （result=104003），两个平台都拿不到全曲；
#   · 而免费的同名翻唱（碎花 / まふまふ / 幸祜…）能下载、歌名还完全一致；
#   · **只给歌名**时 intent.artist 为空，评分对翻唱不设防（原唱一致度取中性
#     0.75、歌手闸门不触发）→ 翻唱拿 78 分 → 第一层判「已足够」→ **跳过全网**；
#   · 结果用户看到的只有"原唱仅试听 + 一堆翻唱"，他的原话是
#     "搜出来原唱因为是网易云所以下不了，其他结果也没有原唱"。
#   同一个查询加上歌手（"ヨルシカ だから僕は音楽を辞めた"）就会走全网并拿到
#   B 站官方 MV（88 分、可下载）—— 差别只在歌手字段，用户却感觉是搜索坏了。
#
# **2026-09-23 晚的后续**：用户对这件事的最终裁决是"直接不要什么网易云qq了，
# 直接全网找"。于是默认配置改成**只启用全网**（Settings schema v4），
# 上面这套"平台原唱下不了"的情形在默认配置下不再出现 —— 但用例照留：
# ① 它的断言在"用户手动勾回平台源"时仍然有效；② 下面新增的断言 3 守的正是
# 新默认本身。
#
# 断言（都不依赖具体分数）：
#   1. 日志里必须出现「[Web] 全网源合计」—— 即真的惊动了全网层。
#      这一条守的正是「跳过全网」这个 bug：平台原唱下不了时就必须去全网。
#   2. 结果清单里必须存在一条「可下载 且 匹配度 ≥ MIN_SCORE」的条目。
#      对应"其他结果也没有原唱"：修好后必须有能下的好结果。
#   3. **仅全网模式下不允许出现任何"仅试听"**（见 check_web_only_no_preview）。
#      这是"搜到的都能下"这条新契约的守门人，也是用户最在意的点。
# ---------------------------------------------------------------------------
SEARCH_CASES = [
    {
        "name": "yorushika-bare-name",
        "query": "だから僕は音楽を辞めた",      # 刻意只给歌名，不给歌手
        "why": "曾只有平台源结果、原唱是会员曲下不了，第一层还误判『已足够』跳过全网；"
               "现在默认仅全网，应直接给出可下载的官方 MV",
        "regression": "① 必须真的跑全网；② 必须有好结果；③ 仅全网模式下不得有『仅试听』",
    },
]

# 结果清单行的格式（见 main.cpp 的 --probe 输出）：
#   [probe]   [1] web | <标题 — 歌手> | 128K | 00:00 | ? MB | 匹配88 | 703.8万播放 | 可下载
#
# ⚠ 必须带 re.MULTILINE。末尾那个 `$` 没有它就只能匹配**整段字符串的最末尾**，
#   于是 finditer 只能命中"最后一行"；而探针在结果清单后面还要打印两行提示
#   （「加 --probe-download N …」），结果就是**一条都匹配不上** → rows 为空 →
#   判定报「可下载的最高只有 None 分」。这是个纯粹的假红，
#   2026-09-23 实测踩到过：换源用例全过、搜索用例却 FAIL 得莫名其妙。
RESULT_LINE = re.compile(
    r"\[probe\]\s+\[(\d+)\]\s+(\S+)\s+\|(.+?)\|\s*(\S+)\s*\|\s*(\S+)\s*\|\s*(.+?)\s*\|"
    r"\s*匹配(\d+)\s*\|(.*)$", re.MULTILINE)


def find_ffprobe():
    # FFmpeg 预编译包已随工具链迁入工作空间上级目录（deps\ffmpeg-*\bin）
    pats = [
        os.path.join(os.path.dirname(ROOT), "deps", "ffmpeg-*", "bin", "ffprobe.exe"),
        os.path.join("D:\\", "deps", "ffmpeg-*", "bin", "ffprobe.exe"),  # 旧位置兜底
        os.path.join(ROOT, "build", "bin", "ffprobe.exe"),
    ]
    for p in pats:
        hit = glob.glob(p)
        if hit:
            return sorted(hit)[-1]
    return shutil.which("ffprobe")


def log_size():
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def read_log_delta(offset):
    """读日志增量，并切到本次探针自己那一段。

    日志是多实例共写的（用户可能同时开着 GUI），必须按本次起点切段，
    否则会把别人的下载记录当成自己的结果。
    """
    if not os.path.exists(LOG):
        return ""
    with open(LOG, "rb") as f:
        f.seek(offset)
        raw = f.read()
    text = raw.decode("utf-8", errors="replace")
    marker = "[probe] === 启用的搜索源"
    if marker in text:
        text = text.rsplit(marker, 1)[-1]
    return text


def archive_probe_dir():
    """把产物目录清空到归档子目录。

    这是本脚本存在的一半理由：目标文件还在原地时，DownloadEngine 那条
    "已存在且 >64KB 就当完成"的早退会直接认领旧产物，测出来的是上一次的结果。
    """
    if not os.path.isdir(PROBE_DIR):
        return
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dest = os.path.join(PROBE_DIR, "_archive", stamp)
    os.makedirs(dest, exist_ok=True)
    moved = 0
    for entry in os.listdir(PROBE_DIR):
        if entry == "_archive":
            continue
        try:
            shutil.move(os.path.join(PROBE_DIR, entry), os.path.join(dest, entry))
            moved += 1
        except OSError:
            pass
    return moved


def ffprobe_duration(path):
    if not ffprobe:
        return None
    try:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=1", path],
            capture_output=True, text=True, timeout=60)
        return float(out.stdout.strip())
    except Exception:
        return None


def parse(seg):
    r = {}
    cands = re.findall(
        r'\[Download\] 换源候选 (\d+) : "(.*?)" \| "(.*?)" \| 相关度 (\d+) \| "(.*?)"',
        seg)
    r["candidates"] = [{"rank": int(a), "title": b, "artist": c,
                        "score": int(d), "detail": e} for a, b, c, d, e in cands]
    r["top_score"] = max((c["score"] for c in r["candidates"]), default=None)

    m = re.search(r"\[Download\] 换源下载第 (\d+) 候选", seg)
    r["picked_rank"] = int(m.group(1)) if m else None
    m = re.search(r'\[Download\] 换源完成: "(.*?)"', seg)
    r["picked_title"] = m.group(1) if m else None
    r["aborted"] = "已放弃以免下错版本" in seg

    m = re.search(r"\[probe\]\s+✓\s+.*?\s+—\s+(.+)（(\d+) 字节）", seg)
    r["file"] = m.group(1).strip() if m else None
    r["bytes"] = int(m.group(2)) if m else None

    m = re.search(r"\[probe\]\s+✗\s+.*?\s+—\s+(.+)", seg)
    r["error"] = m.group(1).strip() if m else None

    r["stale_skip"] = "目标文件已存在，跳过下载" in seg
    return r


def run_case(case, verbose):
    query = "%s %s" % (case["title"], case["artist"].split("/")[0].strip())
    spec = "%s|%s|%d" % (case["artist"], case["title"], case["duration"])
    archive_probe_dir()
    off = log_size()
    try:
        subprocess.run([EXE, "--probe", query, "--probe-song", spec],
                       cwd=os.path.dirname(EXE),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=240)
    except subprocess.TimeoutExpired:
        return {"verdict": "FAIL", "reason": "探针超时（240s）"}

    seg = read_log_delta(off)
    if not seg.strip():
        return {"verdict": "FAIL", "reason": "日志里没有本次探针的记录（切段失败？）"}

    r = parse(seg)
    r["seg"] = seg

    if r["stale_skip"]:
        return dict(r, verdict="FAIL",
                    reason="命中『目标文件已存在，跳过下载』—— 归档没生效，测到的是旧产物")

    target = case["duration"]
    top = r["top_score"]

    if r["file"]:
        if r["file"] and not os.path.exists(r["file"]):
            return dict(r, verdict="FAIL", reason="日志说成功但文件不存在：%s" % r["file"])
        actual = ffprobe_duration(r["file"])
        if actual is None:
            return dict(r, verdict="FAIL", reason="ffprobe 读不出时长：%s" % r["file"])
        dev = abs(actual - target) / float(target)
        r["actual"] = actual
        r["dev"] = dev
        if top is not None and top < MIN_SCORE:
            return dict(r, verdict="FAIL",
                        reason="最高候选只有 %d 分（<%d）却仍然下载了" % (top, MIN_SCORE))
        if dev > TOLERANCE:
            return dict(r, verdict="FAIL",
                        reason="**下错版本**：实际 %.1fs / 应为 %ds，偏差 %.1f%%（容差 %.0f%%）"
                               % (actual, target, dev * 100, TOLERANCE * 100))
        return dict(r, verdict="PASS", kind="下载",
                    reason="实际 %.1fs / 应为 %ds，偏差 %.1f%%" % (actual, target, dev * 100))

    if r["aborted"]:
        if top is None:
            return dict(r, verdict="FAIL", reason="放弃了，但日志里没有任何候选记录")
        if top >= MIN_SCORE:
            return dict(r, verdict="FAIL",
                        reason="放弃，但最高候选有 %d 分（≥%d）—— 有分却不选" % (top, MIN_SCORE))
        if case["must_find"]:
            return dict(r, verdict="FAIL",
                        reason="评分塌了：最高候选只有 %d 分（本用例应当能找到 ≥%d 分的版本）"
                               % (top, MIN_SCORE))
        return dict(r, verdict="PASS", kind="放弃", reason="最高候选 %d 分，低于门槛" % top)

    return dict(r, verdict="FAIL",
                reason="既没下载也没报『已放弃』（探针中途挂了？）%s"
                       % (("错误：" + r["error"]) if r.get("error") else ""))


def parse_search(seg):
    rows = []
    for m in RESULT_LINE.finditer(seg):
        tail = m.group(8)
        rows.append({
            "rank": int(m.group(1)),
            "source": m.group(2),
            "title": m.group(3).strip(),
            "score": int(m.group(7)),
            # "可下载" 与 "仅试听：需要网易云会员…" 是互斥的两个尾标
            "downloadable": ("仅试听" not in tail) and ("可下载" in tail),
        })
    return {
        "rows": rows,
        "web_ran": "[Web] 全网源合计" in seg,
        "tier1_line": next((l.strip() for l in seg.splitlines() if "第一层" in l), ""),
        # 「仅全网模式」＝ 配置里没勾平台源（2026-09-23 起的默认）。
        # 这种模式下必须**一条"仅试听"都没有** —— 见 run_search_case 断言 3。
        "web_only": "仅全网模式" in seg,
    }


def run_search_case(case, verbose):
    off = log_size()
    try:
        subprocess.run([EXE, "--probe", case["query"]],
                       cwd=os.path.dirname(EXE),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=240)
    except subprocess.TimeoutExpired:
        return {"verdict": "FAIL", "reason": "探针超时（240s）"}

    seg = read_log_delta(off)
    if not seg.strip():
        return {"verdict": "FAIL", "reason": "日志里没有本次探针的记录（切段失败？）"}

    r = parse_search(seg)
    r["seg"] = seg
    r["candidates"] = [{"rank": x["rank"], "score": x["score"],
                        "title": x["title"], "detail": x["source"]}
                       for x in r["rows"]]
    r["top_score"] = max((x["score"] for x in r["rows"]), default=None)

    # 断言 0：结果清单必须能解析出来。解析不到 ≠ 结果不好 ——
    # 前者是本工具自己的问题（正则/输出格式变了），后者才是产品回归。
    # 不分开报的话，两者都会显示成"可下载的最高只有 None 分"，极难分诊。
    if not r["rows"]:
        return dict(r, verdict="FAIL",
                    reason="**没解析到任何结果行**（不是产品问题，是本脚本的正则对不上"
                           "探针输出格式了）。请对照 main.cpp 里 --probe 的清单格式"
                           "检查 RESULT_LINE。本次日志片段 %d 字节。" % len(seg))

    # 断言 1：必须真的去全网找过
    if not r["web_ran"]:
        return dict(r, verdict="FAIL",
                    reason="**没跑到全网**：第一层判了『已足够』就跳过第二层 —— "
                           "这正是本用例守的 bug。判定行：%s" % (r["tier1_line"] or "(未找到)"))

    # 断言 2：结果里必须有能下的好结果
    best = [x for x in r["rows"] if x["downloadable"] and x["score"] >= MIN_SCORE]
    if not best:
        top_dl = max((x["score"] for x in r["rows"] if x["downloadable"]), default=None)
        return dict(r, verdict="FAIL",
                    reason="跑到全网了，但没有任何『可下载 且 ≥%d 分』的条目"
                           "（可下载的最高只有 %s 分）—— 就是用户说的『其他结果也没有原唱』"
                           % (MIN_SCORE, top_dl))

    top = max(best, key=lambda x: x["score"])
    return dict(r, verdict="PASS", kind="搜索",
                reason="可下载且 ≥%d 分的有 %d 条；最佳 %d 分 [%s] %s"
                       % (MIN_SCORE, len(best), top["score"], top["source"],
                          top["title"][:52]))


def check_web_only_no_preview(r):
    """断言 3：默认配置（仅全网）下不允许出现任何『仅试听』条目。

    这是用户 2026-09-23 的明确要求："你直接不要什么网易云qq了，直接全网找…
    就是搜索或者是换源都是全网"。既然平台源默认关闭，搜索结果里就不该再有
    "搜出来是原唱、点下载却是仅试听"这种东西 —— 有它就是回归，不管分数多高。

    只在日志出现「仅全网模式」时才检查：用户手动勾回平台源后，"仅试听"是
    平台的如实回应，不能算失败（那时用户自己选择了这个权衡）。
    """
    if not r.get("web_only"):
        return None
    preview = [x for x in r["rows"] if not x["downloadable"]]
    if preview:
        return dict(r, verdict="FAIL",
                    reason="仅全网模式下仍出现 %d 条『仅试听』（第 %d 条：%s）—— "
                           "搜到的都应该是可下载的页面地址"
                           % (len(preview), preview[0]["rank"], preview[0]["title"][:48]))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑用例名包含该子串的用例")
    ap.add_argument("--type", choices=["all", "fallback", "search"], default="all",
                    help="fallback=换源下错版本（默认全部）；search=搜索结果里有没有可下载的原唱")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    global ffprobe
    ffprobe = find_ffprobe()

    if not os.path.exists(EXE):
        print("找不到 %s，先构建：python tools\\build.py" % EXE)
        return 2
    if not ffprobe:
        print("找不到 ffprobe（期望 工作空间\\deps\\ffmpeg-*\\bin\\ffprobe.exe 或 build\\bin）")
        return 2

    key = args.only.lower()
    fb = [c for c in CASES if key in c["name"].lower()] if args.type in ("all", "fallback") else []
    sc = [c for c in SEARCH_CASES if key in c["name"].lower()] if args.type in ("all", "search") else []
    if not fb and not sc:
        print("没有匹配的用例：%s" % args.only)
        return 2

    print("=" * 78)
    print("搜索 / 换源回归探针   exe=%s" % time.strftime("%Y-%m-%d %H:%M",
                                                     time.localtime(os.path.getmtime(EXE))))
    print("契约：换源要么下到时长对得上的版本（偏差 ≤ %.0f%%）、要么明确放弃，绝不下错；"
          % (TOLERANCE * 100))
    print("      平台原唱下不了时，必须去全网找能下载的版本。")
    print("=" * 78)

    failures = []
    total = 0

    for c in sc:
        total += 1
        print("\n[搜索用例] %-18s 「%s」" % (c["name"], c["query"]))
        print("       背景：%s" % c["why"])
        r = run_search_case(c, args.verbose)
        bad = check_web_only_no_preview(r) if r["verdict"] == "PASS" else None
        if bad:
            r = bad
        mark = "  OK  " if r["verdict"] == "PASS" else " FAIL "
        print("[%s] %s：%s" % (mark, r.get("kind", r["verdict"]), r["reason"]))
        if args.verbose and r.get("candidates"):
            for cd in r["candidates"][:10]:
                print("       #%-3d %3d 分  %s  [%s]" % (cd["rank"], cd["score"],
                                                         cd["title"][:58], cd["detail"]))
        if r["verdict"] != "PASS":
            failures.append((c["name"], r["reason"]))

    for c in fb:
        total += 1
        print("\n[换源用例] %-18s %s — %s（目标 %ds）"
              % (c["name"], c["artist"], c["title"], c["duration"]))
        print("       背景：%s" % c["why"])
        r = run_case(c, args.verbose)
        mark = "  OK  " if r["verdict"] == "PASS" else " FAIL "
        print("[%s] %s：%s" % (mark, r.get("kind", r["verdict"]), r["reason"]))
        if r.get("picked_title"):
            print("       选中：%s" % r["picked_title"])
        if args.verbose and r.get("candidates"):
            for cd in r["candidates"]:
                print("       候选%d %3d 分  %s  [%s]" % (cd["rank"], cd["score"],
                                                        cd["title"][:60], cd["detail"]))
        if r["verdict"] != "PASS":
            failures.append((c["name"], r["reason"]))

    print("\n" + "=" * 78)
    if failures:
        print("RESULT: %d/%d 用例失败" % (len(failures), total))
        for n, why in failures:
            print("  ✗ %-18s %s" % (n, why))
    else:
        print("RESULT: ALL PASSED（%d 个用例：没有下错、原唱也搜得到）" % total)
    print("=" * 78)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
