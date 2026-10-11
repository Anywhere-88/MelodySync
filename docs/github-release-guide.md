# GitHub 发布指南（MelodySync 1.0.0）

> 仓库已在本地方案就绪：`D:\OI\AI\AIWork\Music\MelodySync`，首次提交 `cbcc718`，
> 共 113 个源文件。你只需要按下面 4 步走，每步都是复制粘贴级别。

## 前置说明（必读）

1. **你之前发我的 `cookie:d=...` 不是 GitHub 凭据** —— 那是某个网站 15 分钟过期的
   会话 cookie。GitHub 认两种东西：**Personal Access Token**（`ghp_` 或
   `github_pat_` 开头）或 **SSH 密钥**。
   ⚠ 既然这个 cookie 已经贴出来过，建议去来源网站把会话登出/改密一下。
2. **网络通道实测结论**（2026-10-11）：
   - `https://github.com` / `api.github.com` —— **直连不通**（curl 000）；
   - **`ssh.github.com:443` —— 通**（能走到公钥验证那步）。
   所以代码 push 走 SSH over 443（下面第 3 步），浏览器网页操作如果你有代理就正常开。

## 第 1 步：创建空仓库（网页）

浏览器开 https://github.com/new （需要你有代理或能访问的网络环境）：

- Repository name: `MelodySync`
- 选 **Private**（先私有，博客发出时再改 Public）
- **不要**勾选 README / .gitignore / LICENSE（本地已备好）
- 点 Create repository

## 第 2 步：生成 Personal Access Token（网页）

浏览器开 https://github.com/settings/tokens/new ：

- Note: `melodysync-push`
- Expiration: 90 天
- 勾选 `repo`（整个 repo 大类）
- 点 Generate token，**复制 `ghp_...`（只显示一次）**

## 第 3 步：推送代码（本机，走 SSH 443）

```bat
:: 1) 生成 SSH 密钥（一路回车）
ssh-keygen -t ed25519 -C "melodysync" -f %USERPROFILE%\.ssh\id_ed25519

:: 2) 打印公钥，整行复制
type %USERPROFILE%\.ssh\id_ed25519.pub
```

浏览器开 https://github.com/settings/keys → New SSH key → 粘贴 → Add。

然后在 `C:\Users\yfw20\.ssh\` 新建 `config` 文件（无扩展名），写入：

```
Host github.com
    HostName ssh.github.com
    Port 443
    User git
    IdentityFile ~/.ssh/id_ed25519
```

最后推送：

```bat
cd /d D:\OI\AI\AIWork\Music\MelodySync
git remote add origin git@github.com:你的用户名/MelodySync.git
git push -u origin main
```

> 不想配 SSH 的话，HTTPS + token 也行（git push 时把 token 当密码），
> 但 HTTPS 直连 github.com 当前不通，SSH 443 是已验证可用的通道。

## 第 4 步：上传 Release 附件（网页，最省事）

两个包都在桌面：

| 包 | 内容 | 大小 |
|---|---|---|
| `MelodySync-1.0.0-win64-20260927.zip` | 完整版：源码 + 可运行产物（Qt/ffmpeg/yt-dlp 全带） | 213MB |
| 安装包 `MelodySync-1.0.0-Setup.exe` | **还没编译**——需要装 [Inno Setup 6](https://jrsoftware.org/isdl.php) 后运行：`ISCC.exe installer\MelodySync.iss`（产物在 `dist\`） | 预计 ~80MB |

GitHub 网页 → Releases → Draft a new release → Tag `v1.0.0` → 标题
`MelodySync 1.0.0` → 拖入 zip → Publish。

> ⚠ git 仓库里**不放**这两个包（GitHub 单文件上限 100MB，213MB 的 zip 推不上去），
> Release 附件上限 2GB，走 Release 才对。

## 博客

初稿在 `docs\blog-2026-10-11.md`。GitHub 仓库链接建好后，把博客「下载地址」
一节的占位替换成真实 Release 链接即可发布。

## 实战记录（2026-10-11，全流程已跑通）

本仓库实际就是按 PAT + HTTPS 推上去的（没走 SSH），三处坑记录如下：

1. **fine-grained PAT 权限**：`github_pat_` 开头的细粒度 token 要能推送，必须
   **Repository access 覆盖本仓库 + Permissions → Contents = Read and write**。
   权限没生效时 push 报 403，可用 API 响应头 `x-accepted-github-permissions`
   查看 token 实际权限。改完权限记得点页面最底部的 **Update token**。
2. **Watt Toolkit（Steam++）加速劫持 TLS**：开着加速时 git 拿到的是 SteamTools
   本地 MITM 证书，schannel 后端报 `CRYPT_E_NO_REVOCATION_CHECK`。解法（本仓库
   已在 `.git/config` 固化）：
   ```bat
   git config http.sslBackend openssl
   git config http.sslVerify false
   ```
   仅对本仓库生效（MITM 是本机自己的加速器，风险可接受；介意就关加速再推）。
3. **Release 附件走 API**：建 Release（`POST /releases`，tag 用已推送的 `v1.0.0`）
   → `POST uploads.github.com/.../assets?name=<文件名>` 二进制上传 213MB zip，
   上传后核对返回的 `size` 与本地一致。

线上结果：<https://github.com/Anywhere-88/MelodySync>
Release：<https://github.com/Anywhere-88/MelodySync/releases/tag/v1.0.0>
