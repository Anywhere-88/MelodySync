// ============================================================================
//  ms-account.js — MelodySync 账号云函数（部署到腾讯云 CloudBase）
//
//  功能：注册 / 登录 / 心跳(在线状态) / 歌单云同步。无服务器方案的核心端点。
//  数据库集合（首次调用自动建）：
//    ms_users     {username, salt, pass, avatar, deviceId, osVersion,
//                  appVersion, createdAt, lastActiveAt, token, tokenExpiresAt}
//    ms_playlists {owner, items, deviceId, updatedAt}
//    ms_logs      {action, username, ts}   ← 开发者审计日志（可选看）
//
//  安全设计：
//    - 客户端只传「前端弱哈希」pwdHash，绝不传明文；服务端再叠加
//      随机盐 scrypt 落库（与桌面端 UserManager 的本地哈希相互独立）
//    - 登录/注册成功发 32 位随机 token（30 天有效，心跳自动续期）
//    - 心跳/歌单操作必须带有效 token
//
//  部署步骤见仓库 docs/cloudbase-setup.md。部署后把 HTTP 访问地址填进
//  settings.ini 的 cloud/apiBase（或发布机构建时写进 cmake/secrets.cmake）。
// ============================================================================

const cloud = require('wx-server-sdk');
const crypto = require('crypto');

cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV });
const db = cloud.database();
const _ = db.command;

const USERS = 'ms_users';
const PLAYLISTS = 'ms_playlists';
const LOGS = 'ms_logs';
const TOKEN_DAYS = 30;

// ---------- 工具 ----------
function scryptHash(pwdHash, salt) {
  // pwdHash 本身已是前端弱哈希；scrypt 再叠加 → 服务端存储层安全。
  // N=2^14（128KB 内存 * 16384 = 16MB）—— Node 默认 maxmem 32MB 内安全；
  // N=2^15 会撞 33.5MB > 32MB 上限（实测 "memory limit exceeded"）。
  return crypto.scryptSync(pwdHash, salt, 32, { N: 16384, r: 8, p: 1 }).toString('hex');
}
function newToken() { return crypto.randomBytes(24).toString('hex'); }
function now() { return Date.now(); }
function ok(extra) { return Object.assign({ code: 0 }, extra || {}); }
function fail(msg, code) { return { code: code === undefined ? 1 : code, msg: msg }; }

function ensureCollection(name) {
  return db.createCollection(name).catch(() => { /* 已存在 */ });
}

// ---------- action 处理 ----------
async function handleRegister(p) {
  const username = String(p.username || '').trim();
  const pwdHash = String(p.passwordHash || '');
  if (username.length < 2 || username.length > 20) return fail('用户名需 2-20 字符');
  if (pwdHash.length < 6) return fail('密码过短');

  const exists = await db.collection(USERS).where({ username }).count();
  if (exists.total > 0) return fail('用户名已被使用');

  const salt = crypto.randomBytes(16).toString('hex');
  const token = newToken();
  const doc = {
    username,
    salt,
    pass: scryptHash(pwdHash, salt),
    avatar: String(p.avatar || ''),
    deviceId: String(p.deviceId || ''),
    osVersion: String(p.osVersion || ''),
    appVersion: String(p.appVersion || ''),
    createdAt: now(),
    lastActiveAt: now(),
    token,
    tokenExpiresAt: now() + TOKEN_DAYS * 86400000,
  };
  await db.collection(USERS).add({ data: doc });
  return ok({ username, avatar: doc.avatar, token });
}

async function handleLogin(p) {
  const username = String(p.username || '').trim();
  const pwdHash = String(p.passwordHash || '');
  const got = await db.collection(USERS).where({ username }).limit(1).get();
  if (!got.data.length) return fail('云端无此账户', 404);
  const u = got.data[0];
  if (scryptHash(pwdHash, u.salt) !== u.pass) return fail('密码错误', 401);

  const token = newToken();
  await db.collection(USERS).doc(u._id).update({
    data: { token, tokenExpiresAt: now() + TOKEN_DAYS * 86400000, lastActiveAt: now() },
  });
  return ok({ username: u.username, avatar: u.avatar || '', token });
}

async function authByToken(p) {
  const username = String(p.username || '').trim();
  const token = String(p.token || '');
  const got = await db.collection(USERS).where({ username, token }).limit(1).get();
  if (!got.data.length) return null;
  const u = got.data[0];
  if (u.tokenExpiresAt < now()) return null;
  return u;
}

async function handleHeartbeat(p) {
  const u = await authByToken(p);
  if (!u) return fail('会话失效，请重新登录', 401);
  await db.collection(USERS).doc(u._id).update({
    data: { lastActiveAt: now(), tokenExpiresAt: now() + TOKEN_DAYS * 86400000 },
  });
  return ok();
}

async function handlePushPlaylist(p) {
  const u = await authByToken(p);
  if (!u) return fail('会话失效，请重新登录', 401);
  const items = String(p.items || '');
  if (items.length > 512 * 1024) return fail('歌单过大（>512KB）');
  const got = await db.collection(PLAYLISTS).where({ owner: u.username }).limit(1).get();
  const data = { items, deviceId: String(p.deviceId || ''), updatedAt: now() };
  if (got.data.length) {
    await db.collection(PLAYLISTS).doc(got.data[0]._id).update({ data });
  } else {
    data.owner = u.username;
    await db.collection(PLAYLISTS).add({ data });
  }
  return ok();
}

async function handlePullPlaylist(p) {
  const u = await authByToken(p);
  if (!u) return fail('会话失效，请重新登录', 401);
  const got = await db.collection(PLAYLISTS).where({ owner: u.username }).limit(1).get();
  return ok({ items: got.data.length ? got.data[0].items : '' });
}

// ---------- 入口 ----------
// HTTP 访问服务（云接入）会把请求体放在 event.body（字符串）；
// 直接云函数调用时 event 本身就是对象。两种都兼容。
exports.main = async (event) => {
  let p = event;
  if (typeof event.body === 'string') {
    try { p = JSON.parse(event.body); } catch (e) { return fail('请求体不是合法 JSON'); }
  } else if (event.body && typeof event.body === 'object') {
    p = event.body;
  }
  const action = String(p.action || '');

  // 首次访问自动建集合（失败不影响主流程）
  await Promise.all([ensureCollection(USERS), ensureCollection(PLAYLISTS)]);

  let res;
  switch (action) {
    case 'register':     res = await handleRegister(p); break;
    case 'login':        res = await handleLogin(p); break;
    case 'heartbeat':    res = await handleHeartbeat(p); break;
    case 'pushPlaylist': res = await handlePushPlaylist(p); break;
    case 'pullPlaylist': res = await handlePullPlaylist(p); break;
    case 'ping':         res = ok({ pong: true, time: now() }); break;
    default:             res = fail('未知 action: ' + action);
  }

  // 审计日志（fire-and-forget，失败不影响响应）
  db.collection(LOGS).add({
    data: { action, username: String(p.username || ''), ts: now() },
  }).catch(() => {});

  return res;
};
