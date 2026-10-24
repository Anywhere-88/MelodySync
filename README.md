# MelodySync

> Visual Studio 2022 · C++17 · Qt 6.5 WebEngine · CMake 3.21+
> 一个「歌单导入 + AI 全网搜 + 无损下载 + 设备同步 + 本地账户」的 Windows 桌面工具，
> 界面层用 HTML/CSS/JS 写在 `ui/index.html`，通过 QWebChannel 与 C++ 后端通信，
> 最终由 qrc 打包进**单个 exe**。

---

## ⚠️ 合规声明（重要，请先阅读）

1. 本软件**仅限个人学习与研究使用**，不得用于任何商业目的。
2. 本软件**不提供任何绕过 DRM、破解会员限制、伪造登录态**的功能。
   需要会员/付费的曲目，平台会明确拒绝，程序**如实标记并停止**（不重试、不变通）：
   网易云回 `-110`、QQ 音乐回 `104003`，界面上就显示「需要会员」。
3. 「AI 全网搜」仅聚合以下来源：
   - **CC 协议 / CC0 / 公有领域**的合法免费音乐（Jamendo、Free Music Archive、
     Internet Archive）；
   - **各平台官方网页版播放器自己使用的公开接口**返回的音频地址
     （网易云 `song/enhance/player/url`、QQ 音乐 `vkey.GetVkeyServer`），
     以及 **yt-dlp 可解析的页面**（哔哩哔哩 / YouTube / SoundCloud）。
     取不到地址的曲目会带上原因，显示为「仅试听」。
4. 是否有权下载某一首歌曲，**判定责任完全在用户**。用户应自行承担因下载
   受版权保护内容而产生的一切法律责任。
5. 本声明同时出现在程序的三处：`src/main.cpp` 文件头、`README.md`（本文件）、
   以及 UI 的「设置 → 合规声明」区块。

> 第 3 条的边界写直白一点，避免误解：
> 程序**没有**破解加密参数、没有伪造登录态、没有尝试绕过会员校验。
> 它请求的是平台给自家网页播放器用的那个接口 —— 平台愿意给就给，不愿意给就拒绝。
> 用户扫码登录后的 cookie 会被保存（`settings.ini` 的 `platform/neteaseCookie`），
> 用途是让**用户自己的账号**拿到更高码率，不是凭空提升权限。

---

## 搜索源能力对照表

排查「搜到了但下不动」的第一站。**同一个源在不同网络环境下能力差异很大** ——
"某个源能不能用"取决于你的网络，不取决于程序。

> **默认只启用「全网」一个源**（2026-09-23 起，配置 schema v4）。
> 网易云 / QQ 照常注册、设置页可以勾回来，但默认不勾 —— 原因是它们最像原唱的那条
> 常常恰好下不了（会员/无版权），而次优的同名翻唱、合集却能下载、分数也不低，
> 于是用户看到的就是"搜出来是原唱、点下载却是仅试听"。对一个下载器来说，
> **"搜到的都能下"比"元数据更准"重要**，所以搜索与换源统一走全网。
> 勾回平台源即恢复"先平台、再全网"的两层检索。

| 源 | 默认 | 需要凭据 | 国内直连 | 能拿到什么 | 说明 |
|---|---|---|---|---|---|
| 哔哩哔哩 | ✅ 启用 | 不需要 | ✅ | 页面地址（yt-dlp 下载） | **国内唯一稳定可用的 yt-dlp 源** |
| YouTube Music | 随全网 | 不需要 | ❌ | 页面地址（yt-dlp 下载） | YouTube 需可直连的网络 |
| SoundCloud | 随全网 | 不需要 | ⚠️ 不稳定 | 页面地址（yt-dlp 下载） | — |
| 网易云音乐 | ⬜ 可选 | 不需要（登录可提升） | ✅ | 官方音频直链（可到 320K） | 会员曲目返回 `-110`，只能标注"仅试听" |
| QQ 音乐 | ⬜ 可选 | 不需要 | ✅ | 官方音频直链（多为 128K） | 付费曲目返回 `104003`，只能标注"仅试听" |
| Jamendo | ❌ 已下线 | **需要** client_id | ✅ | 音频直链（部分曲目开放下载） | 见 §八，实现文件保留未删 |
| Free Music Archive | ❌ 已下线 | 不需要 | ❌ | — | 走 archive.org，国内常超时 |
| Internet Archive | ❌ 已下线 | 不需要 | ❌ | — | 同上 |

> **✂ 2026-09-23 之前**还并存过 ytmusic / soundcloud / bilibili 三个独立 yt-dlp 源，
> 它们已被合并成一个「全网」源：站点顺序由代码写死、每个站点有独立预算，
> 不可达的站点还有 10 分钟冷却期 —— 否则被墙的 YouTube 会抢着占住那把 yt-dlp
> 串行闸门 30~44 秒，把哔哩哔哩活活饿死到总超时。

> **Pixabay 已下线**：官方 API 只有 `/api/`（图片）与 `/api/videos/`（视频）
> 两个端点，**没有音乐/音效检索端点**。原来请求的 `pixabay.com/api/music/`
> 必然 404 —— 这个源从第一天起就没返回过任何结果。详见
> `src/search/sources/PixabaySource.h`。

### 一条命令查清"到底哪个环节坏了"

```
build\bin\MelodySync.exe --probe "雨爱 杨丞琳" --probe-download 2
```

它做和界面完全一样的事（同一套聚合器 / 直链解析 / 下载引擎），只是不开窗口：
逐个源报可用性与原因 → 列出每条结果「可下载 / 仅试听 + 原因」→ 真的下载前 N 条并报字节数。
输出同时写进日志：`%APPDATA%\MelodySync\MelodySync\logs\melodysync.log`。


---

## 一、目录结构

```
MelodySync/
├── CMakeLists.txt                  # 权威构建定义（推荐用这个）
├── vcpkg.json                      # vcpkg manifest 依赖清单
├── build.bat                       # ★ 一键构建（配置 / 编译 / 运行 / MSBuild 五种动作）
├── check.bat                       # ★ 提交前静态自检（编码 / XML / 源码清单）
├── MelodySync.sln                  # VS 2022 解决方案（手写，供 IDE 打开）
├── MelodySync/
│   ├── MelodySync.vcxproj          # VS 工程
│   └── MelodySync.vcxproj.filters
├── resources/
│   ├── resources.qrc               # 把 ui/index.html 打进 exe（VS 构建走这个）
│   ├── MelodySync.rc               # Win32 资源脚本：把下面的 .ico 链进 PE（见 §13）
│   └── icons/
│       ├── melodysync.ico          # 应用图标，16~256 共 10 帧（由脚本生成，勿手改）
│       ├── preview_sizes.png       # 各尺寸拼版预览（浅底 / 深底各一行）
│       ├── melodysync_256.png      # 实际显示尺寸的预览
│       └── melodysync_1024.png     # 设计稿大图
├── installer/
│   └── MelodySync.iss              # Inno Setup 6 打包脚本
├── bin/                            # 放置外部可执行文件（见 §6）
│   ├── 放置说明.txt                 #   下载地址与放置规则
│   ├── yt-dlp.exe                  #   ← 需自行下载
│   └── ffmpeg.exe                  #   ← 需自行下载
├── tools/
│   ├── check_sources.py            # 源码清单 / 编码 / include / 桥接契约自检
│   ├── check_xml.py                # vcxproj / filters / qrc 的 XML 与路径自检
│   ├── selftest.py                 # ★ 桥接自检：真启动程序，在页面里跑一遍每个 MS.xxx()
│   ├── probe_regression.py         # ★ 换源回归抽查：守"要么下对、要么放弃"（见 §11.2）
│   ├── launch.py                   # ★ 智能启动器：编译了就启 exe，没编译就开浏览器预览
│   ├── make_icon.py                # ★ 画应用图标（纯 PIL 手绘，见 §13）
│   ├── make_shortcut.py            # ★ 建桌面快捷方式（见 §13）
│   └── qr-verify/                  # 自研 QR 编码器的验证套件（见 tools/qr-verify/README.md）
│       ├── README.md               #   验什么、怎么跑、历史踩过的坑
│       ├── qrlite.js               #   与 ui/index.html 内嵌版本同源
│       ├── make_verify_copy.py     #   生成带「强制掩码」开关的测试副本
│       ├── compare2.py             #   ① 逐模块比对 Python qrcode 参考实现
│       ├── penalty_ref.py          #   ② Python 独立实现 ISO 罚分做交叉验证
│       ├── mask_audit.py           #   ③ 审计掩码优选是否取到 ISO 最小值
│       ├── decode_ab2.py           #   ④ 用 OpenCV 解码器把码扫回来
│       ├── run.js / runall.js      #   单次 / 一次跑完 8 个掩码
│       ├── pen_same.js             #   同一矩阵上比对 JS/Python 罚分
│       └── final_check.js          #   正式版与测试副本的一致性
├── ui/
│   └── index.html                  # 前端全部内容（含离线 QR 二维码编码器）
└── src/
    ├── main.cpp                    # 入口 + 日志 + 合规声明
    ├── MainWindow.h / .cpp         # QWebEngineView 宿主 + qwebchannel.js 注入
    ├── core/
    │   ├── Types.h / .cpp          # 全工程数据字典 + 工具函数
    │   ├── HttpClient.h / .cpp     # libcurl 封装（断点续传 / 进度 / Set-Cookie）
    │   └── Async.h / .cpp          # 线程池 / 主线程回投 / 并行 / 限流
    ├── data/
    │   └── Database.h / .cpp       # SQLite（一线程一连接 + WAL + 统计聚合）
    ├── settings/
    │   └── Settings.h / .cpp       # QSettings(IniFormat) 封装
    ├── auth/
    │   ├── UserManager.h / .cpp    # 账户 / 迭代加盐哈希 / 会话恢复
    │   └── CloudSync.h / .cpp      # 可选云端同步（失败全部降级为本地）
    ├── ai/
    │   ├── AiClient.h / .cpp       # OpenAI 兼容 / Claude 双协议
    │   └── IntentParser.h / .cpp   # ★ 意图解析 + AI 匹配度评分 + 去重
    ├── search/
    │   ├── SearchSource.h          # 搜索源统一接口
    │   ├── SearchAggregator.h/.cpp # 8 源并行聚合 + 逐源进度 + 三级排序
    │   └── sources/
    │       ├── JamendoSource.*     # CC 授权
    │       ├── FmaSource.*         # CC 授权（走 archive.org）
    │       ├── ArchiveSource.*     # 公有领域
    │       ├── PixabaySource.*     # ⚠ 未注册（官方无音乐 API，见文件头说明）
    │       └── PreviewSource.*     # 平台源：网易云/QQ/哔哩哔哩/YouTube/SoundCloud
    ├── playlist/
    │   ├── NeteaseApi.*            # 网易云歌单 + 扫码登录
    │   ├── QQMusicApi.*            # QQ 音乐歌单
    │   ├── PlatformResolve.*       # ★ 平台音频直链解析（网易云 / QQ）
    │   └── PlaylistParser.*        # 平台识别 + 分派 + 入库
    ├── download/
    │   ├── YtDlpWrapper.*          # yt-dlp 进程封装 + 健康检查 + 进度正则解析
    │   └── DownloadEngine.*        # ★ 任务池 / 固定槽位 / 优先无损 / 重试
    ├── audio/
    │   ├── TagWriter.*             # TagLib 写元数据 + 内嵌封面
    │   └── FFmpegTranscoder.*      # ★ 格式适配与实时转码
    ├── device/
    │   ├── DeviceManager.*         # WM_DEVICECHANGE 热插拔 + 去抖
    │   ├── UsbSync.*               # ★ 增量同步算法
    │   └── MtpSync.*               # libmtp（不可用时编译期降级）
    └── bridge/
        └── WebBridge.h / .cpp      # ★ JS ↔ C++ 唯一通道（异步契约）
```

★ = 关键算法集中处 / 关键工具，文件头有详细中文推导注释。

---

## 二、环境准备

| 组件 | 版本 | 说明 |
|---|---|---|
| Visual Studio 2022 | 17.8+ | 勾选「使用 C++ 的桌面开发」+ Windows 10/11 SDK |
| CMake | 3.21+ | VS 自带也可，或独立安装 |
| Qt | **6.5 或更高** | 必须包含 **WebEngineWidgets**、**WebChannel**、Network、Sql |
| vcpkg | 最新 | 用于自动拉取 TagLib / FFmpeg / curl / sqlite3 / nlohmann-json |

> **这四个里只有 Qt 必须单独装。** MSVC / CMake / Ninja / vcpkg 通常随 Visual Studio
> 一起就有了 —— VS 安装目录下的 `Common7\IDE\CommonExtensions\Microsoft\CMake\`
> 里带着 cmake 和 ninja，`VC\vcpkg\` 里带着 vcpkg.exe。
> 缺 Qt 时 CMake 会在 `find_package(Qt6 6.5 REQUIRED ...)` 这一步直接停下，
> 报错信息里会点名缺哪个组件。用 `python tools\launch.py --dry-run` 也能先看出来。

### 2.1 安装 Qt（在线安装器）

运行 Qt 在线安装器，在组件树中至少勾选：

```
Qt 6.5.x  →  MSVC 2019 64-bit
        └─ Additional Libraries
             ├─ Qt WebEngine          ← 必须（本工程的核心界面方案）
             └─ Qt WebChannel         ← 必须
             ├─ Qt Network
             └─ Qt Sql
```

记住安装路径，例如 `C:\Qt\6.5.3\msvc2019_64`。

**不想注册 Qt 账号**：官方在线安装器要登录 Qt 账号，也可以用开源的
[`aqtinstall`](https://pypi.org/project/aqtinstall/) 从公开镜像直接拉，无需账号。
**本机就是这么装的**，下面这条命令已实测通过：

```bat
pip install aqtinstall
aqt install-qt windows desktop 6.5.3 win64_msvc2019_64 -O D:\Qt ^
    -m qtwebengine qtwebchannel
```

几个实测要点：

- **只需要装两个模块**。工程 `find_package` 要的六个组件里，
  `Core / Widgets / Network / Sql` 都随 base 包一起下来了，
  只有 `WebEngineWidgets` 和 `WebChannel` 是独立模块。
- 装完 1.9 GB，4 分半钟（走 `download.qt.io`，会自动重定向到国内镜像
  `mirrors.sau.edu.cn`，速度尚可）。
- 验证：`D:\Qt\6.5.3\msvc2019_64\bin\qmake.exe -v` 应输出
  `Using Qt version 6.5.3 in D:/Qt/6.5.3/msvc2019_64/lib`。
- 安装目录可以随意放，但 **`build.bat` 顶部的候选列表要能命中它**
  （默认已包含 `D:\Qt\6.5.3\msvc2019_64`；装到别处就把路径加进那个列表，
  或者设 `QTDIR` 环境变量）。

装好后把它写进环境变量 `QTDIR`，`tools\launch.py` 就能自动找到 Qt 的运行时目录
（它也会自动扫描 `<盘>:/Qt/<版本>/msvc*_64/bin` 这种标准布局，所以不设也能找到）。

### 2.2 准备 vcpkg

```bat
git clone https://github.com/microsoft/vcpkg.git D:\OI\AI\AIWork\Music\vcpkg
D:\OI\AI\AIWork\Music\vcpkg\bootstrap-vcpkg.bat

:: 把 vcpkg 注册到用户级，CMake 才能自动找到
D:\OI\AI\AIWork\Music\vcpkg\vcpkg integrate install
```

**如果 `github.com` 连不上**（企业代理、沙箱环境常见）：

`git clone` 走的是 `github.com:443`，而 GitHub 的文件下载走的是另外几个域名
（`codeload.github.com` / `objects.githubusercontent.com` / `raw.githubusercontent.com`）。
实测过「主站超时、但这几个下载域完全正常」的情况 —— 此时可以手工搭一份 vcpkg：

```bat
:: 1) 从 codeload 取一份快照（主站不通时这个域名往往仍可用）
::    <SHA> 用 https://api.github.com/repos/microsoft/vcpkg/branches/master 查
curl -L -o vcpkg.tar.gz ^
  https://codeload.github.com/microsoft/vcpkg/tar.gz/<SHA>
tar -xzf vcpkg.tar.gz -C D:\OI\AI\AIWork\Music\vcpkg --strip-components=1

:: 2) 从 vcpkg-tool 的 release 直接取官方预编译 exe（版本号要和快照对得上）
curl -L -o D:\OI\AI\AIWork\Music\vcpkg\vcpkg.exe ^
  https://github.com/microsoft/vcpkg-tool/releases/download/<TOOL_TAG>/vcpkg.exe

:: 3) 标记根目录，否则 vcpkg 不认 VCPKG_ROOT
type nul > D:\OI\AI\AIWork\Music\vcpkg\.vcpkg-root

:: 4) 把它变成 git 仓库 —— 这一步不能省，原因见 §2.3
cd /d D:\OI\AI\AIWork\Music\vcpkg
git init && git add -f versions ports && git commit -m "vcpkg snapshot"
```

### 2.3 固定依赖基线（这一步最容易踩坑）

vcpkg 的版本控制机制比想象的深，有两个必知点：

**① `builtin-baseline` 必须是「本机 vcpkg 仓库里真实存在的 commit」。**
vcpkg 解析任何 port 之前都会执行：

```
git show <baseline>:versions/baseline.json
```

填一个本机不存在的哈希（比如直接抄别人的），会立刻报
`failed to git show versions/baseline.json`。换机器后要重新生成：

```bat
cd D:\path\to\MelodySync
D:\OI\AI\AIWork\Music\vcpkg\vcpkg x-update-baseline --add-initial-baseline
```

**② `ports/` 目录必须也提交进 git，只提交 `versions/` 是不够的。**
除了读 baseline.json，vcpkg 还会对每个 port 执行：

```
git read-tree <versions JSON 里记录的 git-tree>
```

也就是说它要从 git 对象库里**取出 port 目录本身**。只加 `versions/` 的话，
会报一堆 `fatal: failed to unpack tree object <sha>`。

好消息是：**git 的 tree 哈希只取决于内容**。只要 ports 内容与官方仓库一致，
自己 `git init` 出来的仓库也能生成出与官方**完全相同**的 tree 哈希，
`read-tree` 一样成功。可以这样自检：

```bat
cd /d D:\OI\AI\AIWork\Music\vcpkg
git rev-parse HEAD:ports/taglib
:: 应等于 versions/t-/taglib.json 里 2.3.1 那条的 git-tree
```

**③ 顺手把 `ports/` 之类挡进 `.gitignore`。**
本仓库里有 1 万多个文件，vcpkg 每次启动都要做一次仓库状态检查。
不挡的话 `git status` 要跑好几分钟，`vcpkg install` 看起来就像卡死。
（挡掉之后 git 只跟踪 `versions/`，状态检查 0.7 秒完成。已用 `git add -f`
提交过的文件不受 `.gitignore` 影响。）

### 2.4 本工程依赖的三个「版本漂移」陷阱

这些是本机实际编译时踩到的，**照抄老文档的写法会直接构建失败**：

| 依赖 | 现象 | 处理 |
|---|---|---|
| `libmtp` | vcpkg 的 port 声明 `supports: (!windows \| mingw) & !uwp & !android`，在 `x64-windows` 上直接被拒 | 已从 `vcpkg.json` 移除。工程会自动降级为 `MS_HAVE_LIBMTP=0`（USB 同步照常，MTP 仅识别设备） |
| `ffmpeg` | ① feature 名漂移：旧版的 `avutil` / `encoding` / `decoding` / `flac` / `aac` 在 ffmpeg 7.x 之后**已全部不存在**，写了会报 "no such feature" 并中断配置；② 更要命的是这个 port 在 Windows 上要拉 **MSYS2** 并用它的 `build.sh` 构建，而在受限环境里那个脚本会**直接卡死**（实测 bash 进程 CPU 只跑了 0.1 秒，却挂在那里 11 分钟不动） | **改用预编译包**，见 §2.7 |
| `openssl` | 单独声明 `openssl` 会让它**从源码编译**（最耗时的一项，20～40 分钟） | **删掉即可**。curl 的 `ssl` feature 在 Windows 上默认就是 SChannel（`platform: windows & !uwp` → `sspi`），只有非 Windows 才回落 OpenSSL。TLS 能力不受影响，还顺便不再需要 `cacert.pem` |

### 2.5 编译端口时找不到 `rc.exe`？

症状（`vcpkg install` 阶段）：

```
-- Check for working C compiler: .../cl.exe - broken
RC Pass 1: command "rc /fo ...manifest.res ..." failed
no such file or directory
--mt=CMAKE_MT-NOTFOUND
```

根因：**vcpkg 用 Ninja 编译端口时不会加载 vcvars 环境**。它的 toolchain
（`scripts/toolchains/windows.cmake`）只设置 `CMAKE_RC_FLAGS`，不指定
`rc.exe` / `mt.exe` 的位置；而 CMake 生成 Ninja 工程时是靠 PATH 去
`find_program(... NAMES rc)` 的。注意报错里的 `--rc=rc` 是**裸文件名**，
说明 PATH 里没有它。

`rc.exe` / `mt.exe` 属于 **Windows SDK**（不在 VS 安装目录里），
默认路径形如：

```
C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\rc.exe
```

两条解法，任选：

```bat
:: 解法 A：先打开「Developer Command Prompt for VS」，再执行 vcpkg
::         （最正统，vcvars64.bat 会把 SDK 路径铺进 PATH）

:: 解法 B：用本工程附带的环境补齐脚本（不需要 cmd 环境，纯 Python）
python tools\run_vcpkg.py install --triplet x64-windows
```

`tools\run_vcpkg.py` 会自己找出最新的一套 MSVC 工具集与 Windows SDK，
补齐 `INCLUDE` / `LIB` / `PATH` / `WindowsSdkDir` / `WindowsSDKVersion`，
再用 `VCPKG_KEEP_ENV_VARS` 放行给 portfile 的子进程 —— 等价于替你跑了一遍
`vcvars64.bat`。**本机就是用这个脚本装完依赖的。**

> 如果 vcpkg 报 `PROGRAM BLOCKED BY SECURITY POLICY ... reg.exe`，
> 说明环境禁止 `reg.exe` 启动，vcpkg / CMake 通过注册表定位 Windows SDK
> 的路就断了 —— 这种情况下必须用解法 B。

### 2.6 工具链版本约束（三条硬性限制，抄错任何一条都构建不过）

这是本机实测出来的组合。**这三条不是偏好，是硬约束**：

| 组件 | 必须用 | 用了别的会怎样 |
|---|---|---|
| **MSVC 工具集** | **14.44**（14.42 / 14.34 亦可） | 用 VS 18 Insiders 自带的 **14.51**：每个包含 QtCore 头的 `.cpp` 都会报 `qvarlengtharray.h(379): error C3861: "stdext": 找不到标识符`。Qt 6.5.3 通过 `QT_MAKE_CHECKED_ARRAY_ITERATOR` 宏用了 MSVC 私有扩展 `stdext::checked_array_iterator`，而 14.51 已把它从 `<iterator>` 里删除（只在 `yvals_core.h` 留了一句 `STL4043 **was** ...` 的注释） |
| **CMake** | **3.30.x** | 用 4.x 时在 `find_package(Qt6 ... COMPONENTS WebEngineWidgets)` 处报 `Failed to find required Qt component`。Qt 6.5.3 的 CMake 包是 3.x 时代的产物 |
| **生成器** | **Ninja** | 用 `-G "Visual Studio 18 2026"` 报 `No CMAKE_CXX_COMPILER could be found`（Insiders 的实例探测有问题）。Ninja 需要脚本自己补齐 vcvars 环境 |

> **改了工具集之后，只删 `build/CMakeCache.txt` 是不够的** ——
> `build/CMakeFiles/` 里缓存着编译器的绝对路径，必须把整个 `build/` 删掉重配。
> 否则日志里的 cl.exe 路径还是旧的，会白折腾半天。

这三条约束已经全部固化在 `tools/build.py` 里，正常使用不需要手工传参。

### 2.7 FFmpeg 用预编译包（不走 vcpkg 编译）

原因见 §2.4：vcpkg 的 ffmpeg port 在 Windows 上要靠 **MSYS2** 的 `build.sh`
构建，而受限环境里那个脚本会直接卡死。

**做法**：取 BtbN 的 shared 构建（一个包同时含头文件、导入库、DLL 和 exe）：

```
https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip
```

解压到 `D:\OI\AI\AIWork\Music\deps\`，结构如下：

```
D:\OI\AI\AIWork\Music\deps\ffmpeg-n9.0-latest-win64-gpl-shared-9.0\
├── include\    libavcodec / libavformat / libavutil / libswresample 的头文件
├── lib\        avcodec.lib / avformat.lib / avutil.lib / swresample.lib（导入库）
└── bin\        avcodec-63.dll ... 以及 ffmpeg.exe / ffprobe.exe
```

`CMakeLists.txt` 里本来就有手工探测的回退路径（`find_package(FFMPEG QUIET)`
失败后走 `find_path` / `find_library`），只要把这个目录放进 `CMAKE_PREFIX_PATH`
就能命中，**工程代码零改动**。`tools/build.py` 的 `FFMPEG_DIR` 默认就指向它，
可用环境变量 `MS_FFMPEG_DIR` 覆盖。

因为链接的是 shared 版，运行时要把 DLL 放到 exe 同级，`ffmpeg.exe` 则作为
sidecar 供设备转码调用：

```bat
copy D:\OI\AI\AIWork\Music\deps\ffmpeg-n9.0-latest-win64-gpl-shared-9.0\bin\*.dll build\bin\
copy D:\OI\AI\AIWork\Music\deps\ffmpeg-n9.0-latest-win64-gpl-shared-9.0\bin\ffmpeg.exe bin\
```

> **顺带一个 Qt 侧的同类坑**：`Qt WebEngine` 依赖 **`Qt6Positioning`**，
> 而 aqtinstall 装 `base + qtwebengine` 时**不会**自动带上它。缺了会报：
>
> ```
> Failed to find required Qt component "WebEngineWidgets".
> Expected Config file at "...Qt6WebEngineWidgetsConfig.cmake" exists
> ```
>
> 这句提示极度误导 —— 那个 Config 文件明明存在。真正缺的是它的**传递依赖**，
> 只有用 `--debug-find-pkg=Qt6WebEngineWidgets` 才看得出来：
>
> ```
> Could NOT find Qt6Positioning (missing: Qt6Positioning_DIR)
> QtPublicDependencyHelpers.cmake:111 (find_dependency)
> ```
>
> 补装即可：
>
> ```bat
> aqt install-qt windows desktop 6.5.3 win64_msvc2019_64 -O D:\Qt -m qtpositioning
> ```

---

## 三、构建

### 3.1 命令行构建（推荐）

**方式一：用 `build.bat`（Windows 下最顺手）**

```bat
build.bat               :: 配置 + 编译 Release → build\bin\MelodySync.exe
build.bat debug         :: 配置 + 编译 Debug   → build-debug\bin\MelodySync.exe
build.bat reconfigure   :: 清掉构建目录后重新配置（改了 CMakeLists.txt / vcpkg.json 后）
build.bat run           :: 编译完直接以 --dev 模式启动
build.bat clean         :: 删掉两个构建目录
build.bat msbuild       :: 改走手写的 MelodySync.sln（备用路径，需 VS 的 Qt 扩展，未在本机验证）
```

**`build.bat` 本身只做转发** —— 它找到 Python 后把参数交给 `tools\build.py`，
真正的构建逻辑在那边。原因见 §2.6：本机有三条硬性工具链约束，用 bat 描述
会很难维护（这一版之前就有一份完整的 cmake 命令行，但它和实际能跑通的链路
已经脱节了，两套实现并存只会越走越偏）。

**方式二：直接调 `tools\build.py`（CI / 脚本里也这么用）**

```bat
python tools\build.py              :: 配置 + 编译 Release
python tools\build.py --debug      :: 配置 + 编译 Debug（独立目录，不会覆盖 Release）
python tools\build.py --configure  :: 只配置，不编译
python tools\build.py --clean      :: 删掉构建目录后从头来（换了工具链必须这么做）
python tools\build.py --run        :: 编译完以 --dev 模式启动
```

它做的事：找出 **MSVC 14.44** 与 Windows SDK → 补齐 `INCLUDE` / `LIB` /
`PATH` / `WindowsSDKVersion` → 用 **Ninja** 生成器 + **CMake 3.30** 配置
（含 `VCPKG_INSTALLED_DIR`）→ `cmake --build`。

可覆盖的路径（环境变量）：`QTDIR`、`VCPKG_ROOT`、`MS_FFMPEG_DIR`、`MS_CMAKE`、
`MS_VC_TOOLS_VER`。

**方式三：手工跑等价命令**（仅供理解流程，**不推荐照抄网上的通用写法**）

```bat
:: 前提：自行设好 INCLUDE / LIB / PATH（或从 Developer Command Prompt 启动），
::       否则 CMake 连编译器都找不到（见 §2.5 / §2.6）
cmake -B build -S . -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_TOOLCHAIN_FILE=D:\OI\AI\AIWork\Music/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH="D:/Qt/6.5.3/msvc2019_64;D:\OI\AI\AIWork\Music/deps/ffmpeg-n9.0-latest-win64-gpl-shared-9.0" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_MANIFEST_INSTALL=OFF ^
  -DVCPKG_INSTALLED_DIR=%CD%\vcpkg_installed

cmake --build build --parallel
```

> 最后那个 `VCPKG_INSTALLED_DIR` 很容易漏：一旦 `VCPKG_MANIFEST_INSTALL=OFF`，
> vcpkg.cmake 就不再认为这是 manifest 工程，会去 `${VCPKG_ROOT}/installed`
> 找依赖（那里是空的），然后报 `Could not find ... provided by "TagLib"`。

产物位置：

```
Release:  build\bin\MelodySync.exe
Debug:    build-debug\bin\MelodySync.exe
```

> **注意 Release 目录下没有 `Release\` 子目录**。Ninja 是单配置生成器，配置名
> 体现在**构建目录名**上（`build` vs `build-debug`），不会再往输出目录里拼一层。
> 两个配置用独立目录是为了避免互相覆盖：挤在同一个目录时，后编的那个会把
> 前一个的 exe 冲掉。

> **提示**：首次构建 vcpkg 要从源码编译 11 个依赖包，本机实测约 **25 分钟**
> （大部分时间花在下载上 —— 这个环境的代理会把大文件在整数 MB 处截断，
> 得靠 HTTP Range 续传补齐）。之后就是增量编译了，改几个文件只要十几秒。

### 3.2 调试运行

```bat
:: 带 --dev 参数 → 直接从磁盘加载 ui/index.html，改前端不用重新编译
build\bin\MelodySync.exe --dev
```

不带 `--dev` 时，界面从 `qrc:/ui/index.html` 读取（即 exe 内嵌的那份）。

### 3.3 打开 VS 2022 IDE

**方式 A（推荐）**：直接用 VS 打开文件夹

```
文件 → 打开 → 文件夹 → 选择 MelodySync 目录
```

VS 会自动识别 `CMakeLists.txt` 并生成 CMake 目标，无需手工配置工程项目。

**方式 B**：打开 `MelodySync.sln`
需要先安装 **Qt Visual Studio Tools** 扩展，并在
`扩展 → Qt VS Tools → Qt Versions` 里添加你的 Qt 安装路径，
否则 `QtMsBuild` 相关的 props/targets 找不到会报错。

### 3.4 提交前自检（强烈建议养成习惯）

```bat
check.bat           :: 跑 check_sources.py + check_xml.py
check.bat full      :: 再带上 QR 编码器验证
```

或者直接单独跑：

```bat
python tools\check_sources.py
python tools\check_xml.py
```

> **`.bat` 里不要出现任何非 ASCII 字符（2026-09-23 修）**
>
> `cmd.exe` 解析含多字节字符的 `.bat` 时会错位。实测：一个 6 行的 `.bat`，
> 只有一行中文 `REM` 注释，在 **LF 行尾**下就会把那条注释当命令执行；
> 而 UTF-8 + `chcp 65001` 的完整版本会让批处理读取器**丢失文件位置并反复重执行**——
> 10 秒刷出约 900 KB 的「不是内部或外部命令」，无子进程、CPU 占满。
> `check.bat` 与 `build.bat` 原来都是这种写法，两个都是坏的
> （`build.bat` 甚至会去跑 `AT` 命令、打 2 MB 帮助文本，然后**根本到不了 `build.py`**）。
>
> 所以 `check.bat` 和 `build.bat` 现在**只有英文**，且必须保持 **CRLF 行尾**。
> 中文输出不会丢 —— 它由下面的 python 脚本（以及 `tools\build.py`）自己打印，
> `chcp 65001` 保证控制台能显示。只有这两个 bat 自己的那几行是英文。
>
> **改动已实测**：`check.bat` 3 秒跑完三门禁、exit 0；
> `build.bat` 默认分支真正走到 `build.py` 并输出 `BUILD OK`、exit 0，
> 未知动作分支正确报错并返回 1。

这两个脚本覆盖的都是「**编译期不一定报错、运行期才炸**」的问题：

| 脚本 | 检查项 | 不检查会怎样 |
|---|---|---|
| `check_sources.py` | 源文件是否 UTF-8 无 BOM | 某天有人用记事本另存，中文全乱码 |
| | `CMakeLists.txt` 清单与 `src/` 实盘是否一致 | 加文件忘了登记 → 链接期 LNK2019；删文件没删清单 → 配置期直接失败 |
| | `/utf-8` 是否还在 | 被手滑删掉 → 界面中文变问号 |
| | 112 条 `#include "..."` 是否都能落地 | 改目录名后忘记改 include |
| | 非 ms 命名空间的 .cpp 是否漏了 `ms::` | 报一堆看不懂的 C2653 |
| | `Q_INVOKABLE` 是否都有定义、信号转发签名是否一致 | LNK2019 / connect 报错难读 |
| `check_xml.py` | `.vcxproj` / `.filters` / `.qrc` 的 XML 良构性 | 注释里出现 `--`（例如把 `cmake --build` 抄进注释）→ VS 直接打不开工程 |
| | 56 条 `Include="..\src\..."` 是否真实存在 | 源码在 Solution Explorer 里全是感叹号 |
| | `<Filter Include="src\core">` 是否被误写成路径 | 文件在解决方案里"消失"了 |
| | `src/` 下有没有漏登记的源文件 | 同上 |
| | `MelodySync.rc` 是否纯 ASCII、引用的 `.ico` 在不在、两个构建入口是否都登记了它 | 图标链不上时 `rc.exe` 只报文件名不报它相对哪个目录找；`.rc` 里写中文 → 被按 CP936 误解析 |

另外还有几条是**"把踩过的坑写成检查"**，它们守的不是"能不能编译"，而是
"某个功能会不会悄悄退化"：

| 检查 | 抓什么 |
|---|---|
| 前端 `_waitSignal` 是否**三段式**（先注册 → 再调用 → 后 await） | 漏 `await` 会立刻炸；`await` 前置会**静默死锁**（等满 40/120 秒超时） |
| 桥接方法提前 `return` 是否发了对应信号 | 前端"只看信号不看返回值"，会白等到超时 |
| exe 内嵌的前端与磁盘 `ui/index.html` 是否同长度 | 改了前端忘重新编译（前端经 qrc 嵌进 exe） |
| `bin/yt-dlp.exe` 与 `build/bin/yt-dlp.exe` **哈希是否一致** | 只改了副本 → 下次构建被 `bin/` 的旧文件静默盖回去 |
| `tools/fetch_ytdlp.py` 默认落点是否在 `bin/` | 同上，从工具侧堵住 |
| 平台源是否仍在换官方直链、下载前是否重解析、直链是否带 Referer | 网易云/QQ 会退回「仅试听」或下载全部 403 |

> 这两套检查不是摆设 —— 开发过程中它们实际抓到过：
> `.vcxproj` 里 56 条 `src\...` 少写了 `..\`、`.filters` 的 Filter 组名被误加前缀、
> XML 注释里的 `--` 导致工程无法加载、`main.cpp` 引用 `ms` 里的类却漏了限定符、
> 新增 `PlatformResolve.*` 只改了 CMakeLists 没登记 vcxproj。

### 3.5 新增守门规则时：必须做注入式反向验证

```
python tools\_verify_new_checks.py
```

它逐条给守门规则**造一个真 bug**（改默认落点、清空直链、`append` 一个字节到
yt-dlp 副本…），确认检查真的报错，然后还原并断言恢复全绿。

为什么非做不可：本工程被一条"永远通过的检查"坑过 —— 第一版"早退发信号"检查
在注入 bug 之后仍然报 0 处，因为判定窗口取宽了，把相邻的正确代码算了进去。
**一个永远通过的检查不只是没用，它比没有更糟**：会给人虚假的安全感。

> 判定窗口要取**结构边界**（例如"本行到下一个同类标记之间"），不能取固定行数 ——
> 否则会撞上隔壁代码，把"漏检"误判成"通过"。

---

---

## 四、VS 2022 配置步骤（方式 B 详细版）

1. **安装扩展**
   `扩展 → 管理扩展 → 联机` 搜索并安装 **Qt Visual Studio Tools**，重启 VS。

2. **配置 Qt 版本**
   `扩展 → Qt VS Tools → Qt Versions` → `Add New Qt Version`
   - Name：`6.5.3_msvc2019_64`
   - Path：`C:\Qt\6.5.3\msvc2019_64`
   点 OK 后设为默认。

3. **让工程引用 Qt**
   右键 `MelodySync` 项目 → `Qt Project Settings` →
   勾选 `Qt Installation` 为上一步的名字，Modules 里勾选
   `Core / Widgets / Network / Sql / WebEngineWidgets / WebChannel`。

4. **配置 vcpkg 集成**
   - 若用过 `vcpkg integrate install`，VS 会自动为所有项目注入
     vcpkg 的 include/lib 路径，一般无需手工配置。
   - 若想用 manifest 模式，右键项目 → `属性` → `vcpkg` →
     设 `Use Vcpkg Manifest = Yes`，`Triplet = x64-windows`。

5. **确认 C++ 语言标准**
   项目属性 → `C/C++ → 语言` → `C++ 语言标准 = ISO C++17 标准 (/std:c++17)`
   （`.vcxproj` 里已设好，无需改动）。

6. **确认字符集为 Unicode**
   项目属性 → `常规` → `字符集 = 使用 Unicode 字符集`。

7. **生成**
   工具栏选 `Release` + `x64` → `生成 → 生成解决方案`（Ctrl+Shift+B）。
   输出：`build\bin\MelodySync.exe`（`CopyRuntimeSidecars` 目标会自动
   把 `bin\yt-dlp.exe`、`bin\ffmpeg.exe` 拷到 exe 同级）。
   —— `MelodySync.vcxproj` 里 `OutDir` 写死为 `$(SolutionDir)build\bin\`，
   没有配置名子目录，和 CMake 的输出路径保持一致，方便两种方式混用。

> **重要**：本工程所有 `.cpp/.h` 均为 **UTF-8（无 BOM）** 编码，
> 因此编译选项里必须带 **`/utf-8`**（CMakeLists 与 .vcxproj 都已加上）。
> 如果你手工新建文件并另存为 GBK，请相应去掉 `/utf-8`，
> 否则会出现中文乱码或 `C2001: 常量中有换行符` 报错。
>
> 用 `python tools\check_sources.py` 可以一次性检查「有没有人把某个文件存成了
> 带 BOM / GBK」，以及「有没有人加了新源文件忘了同步 CMakeLists.txt」。

---

## 五、把 `ui/index.html` 通过 qrc 打包进 exe

这是本项目「单个 exe」的关键。有两条等价路径：

### 5.1 CMake（推荐，自动）

`CMakeLists.txt` 中：

```cmake
qt_add_resources(MelodySync "melodysync_resources"
    PREFIX "/"
    FILES
        ui/index.html
)
```

`qt_add_resources` 会在编译期调用 `rcc`，把文件编译成一个 `.cpp`
并自动加入目标。资源路径取决于 `PREFIX`：

| PREFIX | FILES | 运行时路径 |
|---|---|---|
| `/` | `ui/index.html` | `qrc:/ui/index.html` |
| `/ui` | `index.html` | `qrc:/ui/index.html` |

**改完前端后必须重新执行构建**，否则 exe 里还是旧的那份。

### 5.2 VS 手写工程（`resources/resources.qrc`）

`resources.qrc` 内容是：

```xml
<RCC>
    <qresource prefix="/">
        <file alias="ui/index.html">../ui/index.html</file>
    </qresource>
</RCC>
```

要让 VS 走这条路径，需两步：

1. 项目属性 → `Qt Rcc` → `Qrc 资源` 里确认 `resources.qrc` 已列出；
2. 确保 `resources.qrc` 与 `ui/index.html` 的相对路径正确
   （`../ui/index.html` 是相对 `resources.qrc` 所在目录算的）。

也可以在 VS 里选 `资源文件 → 添加 → 现有项` 把 `resources.qrc` 加进项目，
Qt VS Tools 会自动为它挂上 `QtRcc` 构建步骤。

### 5.3 代码里怎么加载

```cpp
// src/MainWindow.cpp
void MainWindow::loadFrontend(bool preferLocalFile)
{
    // --dev：直读磁盘，方便改 UI 后立刻生效
    if (preferLocalFile) {
        m_view->load(QUrl::fromLocalFile(
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("ui/index.html"))));
        return;
    }
    // 正式：从 qrc 读（文件已内嵌进 exe）
    m_view->load(QUrl(QStringLiteral("qrc:/ui/index.html")));
}
```

### 5.4 别忘了 `qwebchannel.js`

`qwebchannel.js` **不能**用 `<script src="qrc:/...">` 引入 —— QWebEngine 会
拦截 `qrc:` scheme 的子资源请求。正确做法是编译期把它读成字符串，
再用 `QWebEngineScript` 在 `DocumentCreation` 阶段注入：

```cpp
QFile f(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
f.open(QIODevice::ReadOnly);
QWebEngineScript script;
script.setSourceCode(QString::fromUtf8(f.readAll()));
script.setInjectionPoint(QWebEngineScript::DocumentCreation);
script.setWorldId(QWebEngineScript::MainWorld);
m_view->page()->scripts().insert(script);
```

---

## 六、运行时的外部可执行文件（`bin/`）

程序依赖两个外部 exe。它们**不进 qrc**（体积大且需要独立执行权限）。

| 文件 | 用途 | 下载地址 |
|---|---|---|
| `yt-dlp.exe` | 无直链时的下载回退、YouTube/SoundCloud 试听检索 | https://github.com/yt-dlp/yt-dlp/releases |
| `ffmpeg.exe` | 设备格式适配时的实时转码（FLAC→MP3/AAC） | https://www.gyan.dev/ffmpeg/builds/ |

**源码树里**放在 `MelodySync\bin\`（这个目录就是给它们准备的，见 `bin\放置说明.txt`）。

> ### ⚠️ `bin\` 才是唯一的源，`build\bin\` 只是它的副本
>
> 两个构建系统（CMake POST_BUILD / MSBuild CopyRuntimeSidecars）都会执行
> `bin\yt-dlp.exe → build\bin\yt-dlp.exe` 的复制。
>
> 所以**只替换 `build\bin\` 里的那一份是没用的** —— 下次构建就会被 `bin\` 里的旧文件
> 盖回去。踩过的真实场景：手工把 yt-dlp 换成了新版本、跑起来正常，重新构建之后
> 又开始报「yt-dlp.exe 已损坏」，来回找了半天，原因就是这里。
>
> 正确做法（用现成的脚本，它一次更新两份）：
>
> ```bat
> python tools\fetch_ytdlp.py            REM 下载 → 校验 SHA-256 → 实跑 --version → 同步两份
> python tools\fetch_ytdlp.py --check    REM 只体检，不下载
> ```
>
> 为什么需要校验哈希：**只看文件大小不够**。曾经那份损坏的 yt-dlp.exe 有
> 18,903,582 字节，比官方的 17,840,399 还**大**（中间被塞了脏数据），
> 单看大小完全看不出来。而损坏的 PyInstaller 单文件包一启动就报：
>
> ```
> [PYI-xxxx:ERROR] Failed to extract Cryptodome\Cipher\_ARC4.pyd:
>                 decompression resulted in return code -3!
> ```
>
> 程序里 `YtDlpWrapper::healthy()` 会识别这个报错并给出可执行的中文提示，
> 而不是把它当成"搜索了但没结果"。

**运行时**由 `YtDlpWrapper::executablePath()` 按固定顺序查找，找到第一个可用的就用：

```
1. 用户在设置页手工指定的路径（QSettings: tools/ytDlpPath）
2. <exe 同级>\bin\yt-dlp.exe          ← Inno 安装目录布局
3. <exe 同级>\yt-dlp.exe              ← CMake POST_BUILD / MSBuild CopyRuntimeSidecars 用这个
4. <exe 同级>\..\bin\yt-dlp.exe       ← 构建目录布局（exe 在 build\bin，源码在 ..\bin）
5. %USERPROFILE%\.melodysync\bin\yt-dlp.exe
6. 系统 PATH 里的 yt-dlp（用户自己装过的情况）
```

`ffmpeg.exe` 走同一套查找逻辑。

想跳过自动查找、固定用某一个副本时，两个办法：

```ini
; settings.ini
[tools]
ytDlpPath=D:/portable/yt-dlp.exe
```

```bat
:: 或设环境变量（便携版启动脚本里用，优先级高于 ini）
set MELODYSYNC_YTDLP_PATH=D:\portable\yt-dlp.exe
```

> 清空 `ytDlpPath` 即恢复自动查找。

> 也就是说：**两个构建系统都会把 `bin\*.exe` 拷到 exe 同级目录**，
> 而安装包把它们放进 `{app}\bin\`。两种布局都被覆盖，不需要用户手动配。
>
> 另外，若要让 libcurl 正常校验证书，可在 exe 同级放一份 `cacert.pem`
> （程序会优先用它，找不到时回落系统证书库）。

---

## 七、打包成安装程序

### 7.1 先补齐 Qt 运行时

安装包脚本 `installer\MelodySync.iss` 的两个路径宏是：

```ini
#define MyBuildDir   "..\build\bin"        ; 编译产物（exe + 拷过来的 sidecar）
#define MyDistDir    "..\dist\MelodySync"  ; windeployqt 铺好的分发包
```

所以要把 exe **单独放到 `dist\MelodySync\`**（不是 `dist\`），
让 windeployqt 把 Qt 运行库铺在同一个目录里：

```bat
mkdir dist\MelodySync
copy build\bin\MelodySync.exe dist\MelodySync\

C:\Qt\6.5.3\msvc2019_64\bin\windeployqt.exe ^
  --release --no-translations --no-opengl-sw ^
  --no-system-d3d-compiler ^
  dist\MelodySync\MelodySync.exe
```

`windeployqt` 会自动把 `Qt6Core.dll`、`Qt6WebEngineCore.dll`、
`QtWebEngineProcess.exe`、`resources\`、`translations\` 等全部拷到
`dist\MelodySync\`。**Qt WebEngine 的运行时文件很多（约 150～200 MB），这是正常的。**

> `yt-dlp.exe` / `ffmpeg.exe` / `cacert.pem` **不需要**手工拷进 `dist\`：
> `.iss` 会直接从 `build\bin\` 取它们，装到 `{app}\bin\`（见 §6 的查找顺序）。

如果你想做的是**免安装绿色版**（而不是安装包），那就自己拼：

```bat
copy build\bin\MelodySync.exe  dist\MelodySync\
:: windeployqt 同上
mkdir dist\MelodySync\bin
copy bin\yt-dlp.exe  dist\MelodySync\bin\
copy bin\ffmpeg.exe  dist\MelodySync\bin\
copy bin\cacert.pem  dist\MelodySync\          :: 可选
:: 整个 dist\MelodySync 目录打包成 zip 即可
```

可选：把 VC 运行库也一并带上（若目标机器可能没装 VS）：

```bat
copy "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT\*.dll" dist\MelodySync\
```

### 7.2 用 Inno Setup 生成安装包

1. 安装 [Inno Setup 6](https://jrsoftware.org/isdl.php)；
2. 用 Inno 打开 `installer\MelodySync.iss`；
3. 确认 `MyBuildDir` / `MyDistDir` 两个宏与你的实际输出目录一致；
4. `Build → Compile`（Ctrl+F9），或命令行 `ISCC.exe installer\MelodySync.iss`。

产物：`dist\MelodySync-1.1.0-Setup.exe`

脚本要点（已在 `.iss` 里配好）：

```ini
Compression=lzma2/max          ; 最大压缩，WebEngine 体积大，这项很重要
SolidCompression=yes
PrivilegesRequired=lowest      ; 只装到当前用户，免 UAC
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
```

欢迎页嵌入了合规声明，用户安装前必须看到。

---

## 八、需要申请 Key 的搜索源（可选）

程序**没有 Key 也能正常运行** —— 缺 Key 的源会自动跳过，并且**在设置页直接标出**
「未配置 Jamendo client_id（设置 → 搜索源）」，不会报错、不会阻塞其它源。
想启用更多源时按下面配置：

| 源 | 环境变量 | 申请地址 |
|---|---|---|
| Jamendo | `MELODYSYNC_JAMENDO_CLIENT_ID` | https://developer.jamendo.com/v3.0 |

> Pixabay 已下线，对应的 `pixabayKey` 配置项不再生效（见前面的源能力对照表）。

也可以写进设置文件（`QSettings::IniFormat`）：

```ini
[search]
jamendoClientId=xxxxxxxx

[tools]
ytDlpPath=D:/tools/yt-dlp.exe

[platform]
; 扫码登录成功后由程序自动写入，一般不需要手改
neteaseCookie=...
```

**配置文件的确切位置不用猜** —— 程序启动时会把路径打进日志：

```
[MelodySync] 配置文件: "C:/Users/<你>/AppData/Local/MelodySync/MelodySync/settings.ini"
```

（在「设置」页改任何一项后落盘，也可以直接编辑这个 ini 再重启程序。）

环境变量优先级高于设置文件。

**AI 意图解析**（可选）：设置页填 `AI 服务商` + `API Key`，
支持 OpenAI / Claude / 智谱 GLM。**不填则使用内置的规则引擎**
（本地正则 + 9 条风格规则 + 7 条版本规则），完全离线也能工作。
密钥只写入本机 `QSettings`，程序从不回传密钥明文（只回「是否已配置」标志）。

---

## 九、关键算法速查

| 算法 | 位置 | 要点 |
|---|---|---|
| **密码哈希** | `src/auth/UserManager.h` 文件头 | `SHA256(salt ‖ hash)` 迭代 12000 轮 + 16 字节系统随机盐；抗彩虹表 / 抗相同摘要 / 抬高 GPU 暴力成本 |
| **AI 匹配度评分** | `src/ai/IntentParser.h` 文件头 | 歌名相似度 ×60 + 歌手一致性 ×25 + 时长相似度 ×10 + 源可信度 ×5；Dice 二元组系数；版本修正（Live/Remix ×0.85、翻唱 ×0.6） |
| **智能去重** | `src/ai/IntentParser.cpp` | 归一化键（去 `feat.`、去装饰后缀）→ 保留 `qualityRank` 最高者，其余塞进 `altSources` |
| **增量同步** | `src/device/UsbSync.h` 文件头 | 三级判定：文件名存在 → 大小相同 → mtime 差 ≤ **2 秒**（FAT32 时间戳精度只有 2 秒，不容差会导致每次全量重拷） |
| **格式适配** | `src/audio/FFmpegTranscoder.cpp` | 设备支持源格式 → 直传；否则转 MP3 320kbps/44100Hz/2ch；Apple 设备→AAC 256；编码器失败自动降级 |
| **异步契约** | `src/bridge/WebBridge.h` 文件头 | 耗时操作立即返回 `{ok:true,pending:true}`，结果通过信号回推，避免 QWebChannel 在 UI 线程同步阻塞 |
| **假无损直存** | `src/download/DownloadEngine.cpp` | 无损源有直链时落盘不做二次编码，保住 FLAC 原始比特流 |
| **断点续传** | `src/core/HttpClient.cpp` | `CURLOPT_RESUME_FROM_LARGE` + `LOW_SPEED_LIMIT 1B/60s`（防挂在无限重定向） |
| **离线二维码** | `ui/index.html` 第一个 `<script>` 块 | 自研 QR 编码器：byte 模式、版本 1–40、ECC L/M/Q/H、RS 纠错分块交错、ISO 18004 掩码优选。**离线可用**，不依赖任何 CDN。验证见 `tools/qr-verify/` |

---

## 十、自研 QR 编码器为什么需要验证

网易云扫码登录要显示二维码，但这是本地桌面应用 —— 界面通过 qrc 打进 exe，
运行时不会（也不应该）去联网拉 `qrcode.js`，所以编码器必须自己实现。

二维码有几个环节「看起来能扫」但实际上不合规，肉眼完全看不出来：

- 格式信息位的 BCH(15,5) 编码与 XOR 掩码
- 纠错码字的分块、补零（短块在前）与交错顺序
- 时序图形 / 定位图形 / 校正图形的绘制顺序与相互覆盖关系
- 掩码罚分（`N1=3` 连续同色、`N2=3` 2×2 同色块、`N3=40` 伪定位图形、`N4=10` 深色占比）

因此 `tools/qr-verify/` 用 Python 的 `qrcode` 库作为**逐模块比对基准**，
配合 OpenCV 解码器做端到端回扫。当前结论：

| 检查 | 结果 |
|---|---|
| 同一掩码下与参考实现逐模块比对 | **864 / 864 组完全一致** |
| JS 罚分函数 vs Python 独立 ISO 实现 | **逐值相等，掩码优选全部命中最小值**（141 / 141） |
| 渲染成位图后用 OpenCV 解码 | 自研码的失败集合 ⊆ 参考库的失败集合，**真 bug 数 0** |
| 真实扫码登录载荷（72–100 字符 URL） | **12 / 12 全部可解码** |

跑法见 `tools/qr-verify/README.md`。

---

## 十一、常见问题

**Q: 下载全都失败，而且 AI 搜出来的结果「只能试听」**
先跑一次探针，它会把每个源的真实状态摊开：

```bat
build\bin\MelodySync.exe --probe "雨爱 杨丞琳" --probe-download 2
```

这条问题的成因排查（都是实测过的真实成因，不是猜测）：

| 现象 | 真实原因 | 处理 |
|---|---|---|
| 每个任务都在 1 秒内失败，日志刷满「第 1 次重试 / 第 2 次重试」 | `yt-dlp.exe` 被下载截断。它一启动就报 `Failed to extract Cryptodome\...\_ARC4.pyd`，所有走 yt-dlp 的路径（哔哩哔哩 / YouTube / SoundCloud 检索 + 无直链下载）一起废掉 | `python tools\fetch_ytdlp.py`（会校验 SHA-256 并实跑 `--version`）。**注意改 `bin\` 那一份**，见 §六 的警告 |
| 结果列表**每一条**都是灰的「仅试听」 | 8 个源里 5 个拿不到东西（Jamendo 缺 Key、Pixabay 端点本就不存在、FMA/Archive 走 archive.org 国内超时、yt-dlp 坏了），只剩网易云 + QQ —— 而它们**原本被硬编码成仅供试听** | 已修：网易云/QQ 现在会换官方直链；Pixabay 已下线；新增哔哩哔哩源 |
| 队列里失败的任务显示成「等待中」，看不到任何原因 | 前端 `renderQueue()` 没有 `failed` 分支，状态掉进默认值 | 已修：失败显示红字 + 原因，并支持单条重试 |
| 从**歌单**点「全部下载」，全部失败 | 前端只传了歌名/歌手，没传平台与平台内曲目 id，后端只能退回"用歌名去 YouTube 搜一首" | 已修：「全部下载」现在带上 `src` + `pid`，下载前现取平台直链 |
| 单曲点下载，过一会儿才失败（403） | 平台直链是**带时效**的（网易云 1200 秒）。排队十几分钟后再下，地址已过期 | 已修：队列里只存平台曲目 id，真正下载的那一刻才换地址 |
| 提示「需要网易云会员」「需要 QQ 音乐会员或购买单曲」 | 平台拒绝了，这是**正常且预期**的结果 | 无法绕过，也不应绕过。换一个源（哔哩哔哩 / SoundCloud / 免费 CC 源） |

**Q: 搜索要等 8 秒才出结果**
`FMA` 与 `Internet Archive` 两个源都走 archive.org，国内多数网络不可达，
每次都要等连接超时（8 秒）才回落。所有源是并行的，所以总耗时 ≈ 最慢的那个源。
停用这两个源后搜索会在 1～2 秒内返回：**设置 → 搜索源** 取消勾选即可。
（在有代理的环境里它们是可以用的，所以没有默认关掉。）

**Q: 双击桌面快捷方式，提示「找不到文件」/「Windows 找不到 build\bin\MelodySync.exe」**
工程还没编译过，那个 exe 根本不存在。两条路：

1. 让快捷方式变「怎么都能用」——换成启动器模式，没编译时它会打开浏览器预览：
   ```bat
   python tools\make_shortcut.py --via-launcher
   ```
2. 真的编译它：见 §2 装 Qt（**这是唯一硬性前置条件**）→ §3 构建。
   本机可能已经装好 MSVC / CMake / Ninja（VS Insiders 自带），但**没装 Qt SDK**，
   `find_package(Qt6 6.5 REQUIRED ...)` 会直接失败。

先跑 `python tools\launch.py --dry-run` 可以把「缺什么」一次性列出来。

**Q: AI 搜索 / 歌单解析「全部失败」，而且填了 API Key 也没用**
这是一类**症状在界面上、病根在桥接契约**的问题，先看日志再动手：

```bat
:: 1) 看有没有这句 —— 它是「前端忘了 await」的铁证
findstr /C:"[object Promise]" "%APPDATA%\MelodySync\MelodySync\logs\melodysync.log"

:: 2) 看 Key 到底存进去没有（有 provider=glm 却没有 apiKey 就是没存）
type "%LOCALAPPDATA%\MelodySync\MelodySync\settings.ini"

:: 3) 直接问程序：桥接到底通不通
python tools\selftest.py
```

三种典型病因，各自的现象都不一样：

| 现象 | 病因 | 处理 |
|---|---|---|
| 日志里反复出现 `Unexpected token 'o', "[object Promise]" is not valid JSON`，搜索/解析/队列/曲库**全都**失败 | 前端「等信号」的封装**漏了 `await`**，`JSON.parse(Promise)` 抛错 | 改成**三段式**（见下），别只在前面补个 `await` |
| 点按钮后**一直在转圈**，没有任何报错；日志里后端**确实收到了请求**，但比点击晚几十秒 | `await` 加到了 `_waitSignal` 前面 —— 死锁：信号要等后面那行 `bridge.xxx()` 才可能发出，而那行永远执行不到，只能等满超时。解析要 40 秒、AI 搜索要 120 秒 | 同上，改成三段式 |
| `settings.ini` 里只有 `provider=` 没有 `apiKey=` | Key 的保存顺序写反了 | 见 `_pendingAiKey` 的实现，**先存值再清空** |
| Key 存上了但仍然报 401/404 | 选了服务商但接口地址/模型名没跟着变（例如选 GLM 却把请求发去了 api.openai.com） | 设置页「模型名」留空即按服务商自动推导；下方那行「将请求 …」会显示实际地址 |
| 想确认 Key 到底行不行 | 地址、Key、模型名三样错任何一个，表现都是"静静回落成规则解析"，看不出来 | 设置页点 **「测试连接」**，它发一次真实请求并把服务端原话带回来（如「令牌已过期或验证不正确」） |

**前端「等信号」必须写成三段式，顺序不能动**：

```js
const p = _waitSignal('settingsLoaded', 8000);   // 1) 先注册监听
bridge.getSettings();                            // 2) 再发起调用
const r = await p;                               // 3) 最后等信号
return r ? JSON.parse(r) : null;
```

> 信号是第 2 步触发的。把 `await` 提前到第 1 步（`await _waitSignal(...)`）就会
> **死锁**：函数卡在等一个永远等不到的信号上，第 2 步根本执行不到。
> 这是本项目踩过最贵的一个坑 —— 它不报错，只表现为"点了没反应"，
> 而日志里请求确实存在，只是晚了一个超时周期，极易误判成网络慢。
> `check.bat` 已加守门规则（两种错法都能报出来），改完前端必跑。

**排查时最省时间的一条**：设置页 AI 区域底下那行提示会实时显示
`将请求 <完整地址> · 模型 <模型名>`，一眼就能区分上面三种情况。

> 改完 `ui/index.html`（前端）**必须重新编译** —— 它是通过 qrc 嵌进 exe 的。
> 只想快速试前端效果可以 `build\bin\MelodySync.exe --dev`，那样直接读磁盘上的
> `ui/index.html`，省掉一次编译。

**Q: 构建报 `builtin-baseline` 无效**
执行 `vcpkg x-update-baseline --add-initial-baseline` 更新基线。

**Q: 找不到 `Qt6WebEngineWidgets`**
Qt 在线安装器里没勾 WebEngine。补装 `Qt WebEngine` 组件后重新配置 CMake
（先删掉 `build/CMakeCache.txt`）。

**Q: 界面白屏**
1. 带 `--dev` 跑，看控制台是否有 JS 报错（JS 的 `console.log` 会转发到 Qt 日志）；
2. 检查 `%APPDATA%\MelodySync\MelodySync\logs\melodysync.log`（见 §12）；
3. 确认 `qwebchannel.js` 是用 `QWebEngineScript` 注入的（见 §5.4）。

**Q: 中文显示成乱码**
源码被存成了 GBK，但编译带了 `/utf-8`（或反过来）。
本工程统一 **UTF-8 无 BOM + `/utf-8`**，请保持这个组合。

**Q: MTP 设备识别不到**
libmtp 未找到时会**编译期降级**为「仅识别设备」模式（不会崩）。
在 vcpkg 里安装 `libmtp:x64-windows` 后重新配置即可启用完整功能。

**Q: 下载一直"等待中"**
检查 `bin\yt-dlp.exe` 是否存在。纯免费源（有直链）不依赖 yt-dlp，
但试听类源与搜索匹配需要它。

**Q: 每台设备同步都全量重拷**
先确认设备的文件系统。FAT32 时间戳精度 2 秒，程序已加容差；
若设备是 exFAT/NTFS 仍全量，请查看日志里 `[UsbSync]` 的判定明细。

### 11.1 桥接自检：`python tools\selftest.py`

前端的桥接契约是「调方法 + 等信号」，这类问题（信号名对不上、返回值忘了 `await`、
超时回落成 mock）**静态检查抓不到，只有真正在页面里执行一次才暴露** —— 而且往往是
用户点一下界面才炸。这个脚本把「人工点一遍」自动化了：

```bat
python tools\selftest.py                      :: 全部探测项
python tools\selftest.py --no-search --no-playlist   :: 只测基础桥接
python tools\selftest.py --query "周杰伦 晴天" --playlist "<歌单链接>"
```

它启动 `MelodySync.exe --selftest`，程序会在页面就绪后注入一段自检脚本，把每个
`MS.xxx()` 的**真实返回类型**写进日志，脚本再整理打印。输出长这样：

```
  bridge.native   = true
  settings        = provider=glm model=glm-4-flash baseUrl=https://open.bigmodel.cn/api/paas/v4 keySet=true
  sources         = 8
  queue           = 0
  parsePlaylist   = OK 6 首 / 华语流行精选
  aiSearch        = OK 21 条 / engine=llm:智谱 GLM-4
```

> **两个必须知道的限制**
>
> 1. `--selftest` 会**显示窗口并拒绝外部关闭请求**。因为 Chromium 会把不可见页面的
>    JS 挂起（隐藏运行时连同步脚本的回调都不触发），而某些无人值守会话又会在窗口
>    出现后立刻发 WM_CLOSE。两条都堵死，只能「显示但不许关」。
> 2. 自检依赖 QWebEngine 真的执行页面 JS。**在没有交互式桌面会话的环境里跑不出来**
>    （现象是「步骤1 — 探测页面运行环境」之后就没有下文了）。这时改用
>    `--dev` 模式在本机跑，或者直接看日志里的 JS 报错。
>
> **2026-09-27 修订**：早期版本「步骤 2 永远拿不到结果」曾被误判为上面第 2 条的
> 环境限制，真因是 **Qt 6.5 的 `runJavaScript` 不等 Promise** —— async 自检脚本的
> 回调下一拍就带着空串返回，而脚本本身还在后台继续跑（搜索日志在 9 秒后才出现），
> 「结束」标记因此永远不出现。现已改为：脚本把结果落 `window.__msSelftest` 全局变量
> （附 `.catch()` 兜底），C++ 每秒轮询该变量（预算 150s），拿到后再输出实测表和
> 「结束」标记。实测 24s 跑完全部探测项、0 可疑项。

### 11.2 换源回归抽查：`python tools\probe_regression.py`

「换源下载下错版本」是一个反复出现、却长期没有自动验证覆盖的问题类 ——
`--probe-fallback` 只能挑「搜索结果里恰好没有直链」的条目，而**歌单任务的字段
直接来自平台曲库、根本不经过搜索结果**，那条最容易出错的路径从来没被测过。
这个脚本把它固化成可重复的抽查：

```bat
python tools\probe_regression.py                :: 跑全部用例
python tools\probe_regression.py -v             :: 附带每个候选的分数明细
python tools\probe_regression.py --only sugar   :: 只跑名字含 sugar 的用例
```

它守的是一条**不依赖具体分数**的硬契约：

> 换源要么下到**时长对得上**的版本（偏差 ≤ 20%），要么**明确放弃**；绝不静默下错的。

内置用例取自真实歌单（字段来自 `users.db`，artist 是 `Cartoon / Jéja / Coleman Trapp`
这样的多歌手串），失败时会直接打印「**下错版本**：实际 295.4s / 应为 235s，偏差 25.7%」
这种带数字的判定。输出长这样：

```
[用例] why-we-lose        Cartoon / Jéja / Coleman Trapp — Why We Lose（目标 197s）
[  OK  ] 下载：实际 213.1s / 应为 197s，偏差 8.2%
       选中：Cartoon - Why We Lose (feat. Coleman Trapp) [NCS Release]
[用例] sugar              Maroon 5 — Sugar（目标 235s）
[  OK  ] 放弃：最高候选 38 分，低于门槛
```

> **三点必须知道的**
>
> 1. **它每跑一个用例前会把 `%TEMP%\melodysync-probe` 整体归档到 `_archive\<时间戳>\`。**
>    这不是洁癖：`DownloadEngine` 里有「目标文件已存在且 >64KB 就当完成」的早退，
>    不清产物的话第二次跑测到的全是上一次的旧文件，日志上看着像「换源秒成」。
> 2. **不要加进 `check.bat`。** 它依赖哔哩哔哩的当期搜索结果，分数会随站点内容漂移 ——
>    这是抽查工具，不是门禁。
> 3. 手动跑探针（`MelodySync.exe --probe ... --probe-song "歌手|歌名|时长秒"`）时，
>    若**在用户设的下载目录里已经有同名文件**（例如他报"下错版本"时手上就是旧文件），
>    新逻辑不会执行、界面却照样报成功。**验证前先把那几个旧文件删掉。**

---

## 十二、日志与数据文件位置

三个文件都挂在 `QStandardPaths` 下，具体目录由「组织名 / 应用名」（都是
`MelodySync`）决定，所以路径里会出现**两层同名目录**，这不是 bug：

```
%APPDATA%\MelodySync\MelodySync\logs\melodysync.log   运行日志（含 JS console 转发）
%LOCALAPPDATA%\MelodySync\MelodySync\settings.ini      设置（IniFormat，可手改）
%APPDATA%\MelodySync\MelodySync\users.db               账户 / 音乐库 / 歌单 / 下载历史（SQLite）
%APPDATA%\MelodySync\MelodySync\users.db-wal           WAL 日志（程序运行时存在）
```

> 不想记路径就在启动日志里看 —— `main.cpp` 会把三个位置都打出来：
> ```
> [MelodySync] 配置文件: ".../settings.ini"
> [MelodySync] 下载目录: ".../Music/MelodySync"
> ```

数据库使用 WAL 模式，一线程一连接（连接名带 threadId），
多线程下不会互相踩踏；连接还设了 `busy_timeout`，避免瞬时锁冲突直接报错。
日志 handler 对 `QtWarningMsg` 以下级别做了限流，避免 UI 线程被刷爆。

想彻底重置程序：关掉程序，删掉上面三个文件（连同 `-wal` / `-shm`）即可。
**注意这不会删除已经下载到磁盘的音乐文件** —— 音乐库表只记录索引。

---

## 十三、应用图标与桌面快捷方式

### 13.1 图标是画出来的，不是找的

`resources/icons/melodysync.ico` 由 `tools/make_icon.py` 生成，不依赖任何图标素材
（只用 **Pillow** 做栅格化和缩放，需要 `pip install pillow`，没有别的图形库依赖）：

```bat
python tools\make_icon.py
```

要改配色、大小、形状，动脚本顶部的常量即可：

| 元素 | 做法 |
|---|---|
| 圆角方底 | 深灰 `#2B313B` → `#0A0C11` 的对角线性渐变，圆角半径约占边长 23% |
| 右下辉光 | 径向衰减的冷青色 `#16E0C4`，峰值不透明度 13% |
| 音符 | 薄荷青 `#6EF5DC` → 紫蓝 `#7C5CFF`，高度占画布 66.5% |
| 符号构成 | 旋转椭圆符头 + 胶囊符干 + 二次贝塞尔采样出的渐细旗子 |

三个刻意的实现选择（换套参数重画时别把它们改掉）：

1. **在 2048 上画完再缩到 16~256**。PIL 的 `polygon` / `line` 没有抗锯齿，
   直接在 256 上画边缘全是台阶；放大 8 倍再缩等于自带超采样。
2. **形状用解析几何构造，不用 `rotate()`**。旋转椭圆按参数方程直接算点集，
   旗子沿贝塞尔采样后按法线左右偏移成渐细的闭合多边形 —— 位置能精确到亚像素，
   也不会被 `rotate()` 的取整带偏。
3. **最后一步才切 alpha**。先把整张画布铺满不透明底色，再 `putalpha` 圆角遮罩；
   如果一开始就把画布做成透明的，缩放时透明区的黑色会渗进边缘，
   圆角处会出现一圈脏黑边。

### 13.2 每次生成都会校验 ICO 容器

`make_icon.py` 写完 `.ico` 后会按字节**独立**解析一遍（不走 PIL 的读取路径）核对：
ICONDIR 头、每个条目的偏移是否首尾相接、位深是否 32、DIB 的 `biHeight` 是否为
真实高度的两倍（ICO 把 XOR 位图与 AND 掩码记为「两层」，高度翻倍）、
内嵌尺寸是否与预期一致。

因为手写容器格式最容易错在偏移上，而这类错误 PIL 往往能容忍、Windows 却直接读不出图标。

```
ico written : ...\resources\icons\melodysync.ico (174437 bytes)
container   : [ OK ] 10 frames, offsets contiguous, all 32bpp
sizes       : [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
```

想肉眼看效果就打开 `resources/icons/preview_sizes.png` —— 各尺寸用最近邻放大后
并排贴出来，还分了浅底 / 深底两行，可以确认图标在两种桌面背景上都看得清。

### 13.3 exe 的图标是怎么链上去的

`resources/MelodySync.rc` 全文只有一句 `1 ICON "icons/melodysync.ico"`，
它会在两个构建入口分别登记，缺一个就有一边构建出来的 exe 没图标：

- CMake 路线：`CMakeLists.txt` 里的 `MS_WIN_RESOURCES` 变量
- VS 路线：`.vcxproj` 里的 `<ResourceCompile Include="..\resources\MelodySync.rc" />`

> **`MelodySync.rc` 必须保持纯 ASCII。**
> `rc.exe` 按系统 ANSI 代码页读 .rc 脚本（中文 Windows = CP936），
> 除非文件开头有 `#pragma code_page(65001)`。所以**全工程唯一一个刻意不写中文注释的
> 文件就是它** —— 其他文件都是 UTF-8 + `/utf-8`，但资源编译器是另一套东西。
> 原因也写在文件头注释里，`check_sources.py` 会盯着这条。

`rc.exe` 解析资源文件路径时以 .rc 所在目录为基准，
所以 `"icons/melodysync.ico"` 指的是 `resources/icons/melodysync.ico`。

### 13.4 桌面快捷方式

**推荐：启动器模式**

```bat
python tools\make_shortcut.py --via-launcher
```

快捷方式指向 `tools\launch.py`（用 `pythonw` 跑，不弹控制台），由启动器自己决定：

| 情况 | 行为 |
|---|---|
| `build\bin\MelodySync.exe` 存在 | 直接拉起真程序（并把 Qt 的 `bin` 前置到子进程 `PATH`） |
| 不存在 | 弹原生对话框说明「还没编译」，然后在浏览器打开 `ui\index.html` 预览 |

这样**编译前后都不用重新创建快捷方式**。如果直接把快捷方式指向 exe，在还没编译
时双击只会得到 Windows 的「找不到文件」——看不出原因，也看不到任何东西。

**直接指向 exe（不推荐，除非已经打好包）**

```bat
python tools\make_shortcut.py
python tools\make_shortcut.py --name "MelodySync (便携)" --target dist\MelodySync\MelodySync.exe
python tools\make_shortcut.py --dir dist         :: 不建在桌面，建到别处
```

默认目标路径是 `build\bin\MelodySync.exe` —— 这个工程**没有** Release/Debug 子目录，
两个配置的产物同名同地（`CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>` 和 vcxproj 的
`OutDir` 都钉在同一层），所以别写成 `build\bin\Release\...`。
`check_sources.py` 会拿 CMakeLists 里的 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 去核对
`launch.py` 和 `make_shortcut.py` 里的路径，防止哪天又漂掉。

脚本会写入目标 / 参数 / 工作目录 / 图标 / 备注，然后**回读校验**一遍再报 OK。

为什么不用网上最常见的 `WScript.Shell`：PowerShell 的 `New-Object -ComObject`、
`cmd` 调 `cscript` 这类方式在受限环境里可能被安全策略拦掉。所以这里用 `ctypes`
直接对接 Explorer 内部用的同一套接口 —— `IShellLinkW` + `IPersistFile::Save`。
走这条路生成的 `.lnk` 与右键「新建快捷方式」完全等价（shell 自己序列化，
带完整 IDList），约 1.7 KB。

`ctypes` 没有 COM 内建支持，vtable 得自己数，脚本里每个槽位都标了对应的
C++ 方法名（`GetArguments`/`SetArguments` 是 10/11）。改的时候对着 IDL 数，
别凭记忆 —— 索引错位不会报错，会直接崩。

### 13.5 启动器（`tools/launch.py`）为什么存在

它解决的是两类「难懂的错误」：

1. **还没编译** → 原本是 Windows 的「找不到文件」，没有任何上下文；
2. **编译了但缺 Qt 运行时** → 原本是「找不到 Qt6WebEngineCore.dll」。
   开发期通常没跑过 `windeployqt`，这个错几乎一定会遇到。

现在前者变成「还没编译 + 现在给你看浏览器预览」，后者被启动器直接消掉了：
它会去找 Qt 的 `bin` 目录（顺序是 `qt_bin.txt` → `QTDIR` → `C:\Qt\<版本>\msvc*_64\bin`
→ exe 同级目录），找到就前置到子进程的 `PATH`，不必反复跑 `windeployqt`。

单独调用：

```bat
python  tools\launch.py             :: 按逻辑走（有 exe 就启，没有就预览）
python  tools\launch.py --preview   :: 强制浏览器预览
python  tools\launch.py --dry-run   :: 只报告会做什么，不真的启动
python  tools\launch.py -- --dev    :: -- 之后的参数原样传给 MelodySync.exe
```

`--dry-run` 在排查「双击没反应」时最有用，它会一次性打印工程根、exe 路径、
前端文件、图标、找到的 Qt 运行时和即将执行的动作。

> **提示**：前端 `ui/index.html` 是自包含单文件（含自研离线二维码编码器），
> 无 C++ 后端时会灌演示数据，所以浏览器预览能完整渲染界面 —— 改前端时不必编译，
> 直接刷浏览器就行。

### 13.6 图标换了但桌面不刷新

Windows 有图标缓存，按顺序试：

```bat
ie4uinit.exe -show                              :: 温和，通常就够
:: 还不行就删缓存再重启 explorer
del /f /q %LOCALAPPDATA%\IconCache.db
```

最省事的办法：把桌面那个快捷方式改个名，让它换个缓存键。

---

## 十四、许可

本项目仅供学习研究。使用者需自行遵守所在地区法律法规及
各音乐平台的服务条款。
