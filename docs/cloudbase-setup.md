# CloudBase 账号后端部署指南（10 分钟）

LeanCloud 已公告 2027-01-12 彻底关停，账号系统改用腾讯云 CloudBase（官方指定迁移目标，国内直连 0.15s，免费体验版够用）。

## 一、开通环境（一次性）

1. 注册腾讯云账号：<https://cloud.tencent.com>（微信扫码即可，需要实名认证）
2. 打开 CloudBase 控制台：<https://tcb.cloud.tencent.com>
3. **创建环境** → 选「免费体验版」（¥0/月，3000 资源点/月，约 15 万次调用）→ 环境名随意（如 `melodysync`）
   - ⚠ 免费环境**每 6 个月要手动续期一次**（到期前 1 个月控制台会有提醒按钮）

## 二、部署云函数

1. 环境里 → **云函数** → **新建云函数**
   - 函数名：`ms-account`
   - 运行时：`Nodejs 18.15`（或 16，都可以）
   - 创建方式：**空白函数**（Hello World 模板）
2. 打开本仓库 `tools/cloudbase/ms-account.js`，**全选复制**，粘贴覆盖在线编辑器里的 `index.js`
3. **保存并安装依赖**（在线编辑器会自动装 `wx-server-sdk`；若手动：依赖名填 `wx-server-sdk` 最新版）
4. 部署完成

## 三、开启 HTTP 访问

1. 云函数 `ms-account` → **函数配置/云接入（HTTP 访问服务）** → 新建云接入路径
   - 路径：`/ms-account`
   - 鉴权：**免鉴权**（我们的安全在函数内部：scrypt 加盐 + token 机制）
2. 记下 **公网访问地址**，形如：
   `https://<环境ID>-<随机>.service.tcloudbase.com/ms-account`

## 四、验证

**推荐：客户端探针一键全验**（注册→心跳→歌单推拉→错误密码拒绝，全链路）：

```bash
build\bin\MelodySync.exe --probe-cloud "<你的HTTP访问地址>"
# 期望末行：=== CloudProbe：全部通过 ===（返回码 0）
```

或先手动 curl 验证连通性：

```bash
curl -X POST "<你的HTTP访问地址>" -H "Content-Type: application/json" \
     -d "{\"action\":\"ping\"}"
# 期望返回：{"code":0,"pong":true,"time":17...}
```

## 五、接入客户端（二选一）

- **本机调试**：编辑 `%LOCALAPPDATA%\MelodySync\MelodySync\settings.ini`，加一行：
  ```ini
  [cloud]
  apiBase=<你的HTTP访问地址>
  ```
- **发布构建**：创建 `cmake/secrets.cmake`（已被 .gitignore，不进仓库）：
  ```cmake
  set(MS_CLOUD_API_BASE "<你的HTTP访问地址>")
  ```
  然后正常 `python tools\build.py`，地址即打进 exe。

## 六、开发者怎么看用户

- **用户列表**：CloudBase 控制台 → 数据库 → `ms_users` 集合
  （注册时间 `createdAt`、最近活跃 `lastActiveAt`、设备/系统/版本号都在）
- **在线状态**：`lastActiveAt` 半小时内 = 在线（客户端每 10 分钟心跳一次）
- **审计日志**：`ms_logs` 集合（每次动作一条）
- **歌单云存档**：`ms_playlists` 集合（`owner` = 用户名，`items` = 歌单 JSON）

## 安全说明

- 客户端只传前端弱哈希（明文不出进程）；服务端叠加随机盐 scrypt 存库
- 登录/注册发 32 字节随机 token（30 天有效，心跳自动续期）；心跳/歌单操作必须带 token
- 免鉴权暴露的只是入口，攻击面是函数级限流（CloudBase 自带 QPS 限制）；额度被盗刷最坏情况是耗尽免费资源点，不影响本地功能（客户端全链路降级）
