# ============================================================================
#  mock_server.py — ms-account 云函数的本地 mock（客户端联调用）
#
#  用途：云端还没部署/不想耗免费额度时，验证客户端传输层与探针链路。
#  合同与 tools/cloudbase/ms-account.js 完全一致：
#    POST /  {action: register|login|heartbeat|pushPlaylist|pullPlaylist|ping}
#    → {code:0,...} / {code:非0, msg}
#
#  用法：
#    python tools/cloudbase/mock_server.py [端口]     # 默认 8765
#    MelodySync.exe --probe-cloud http://127.0.0.1:8765
# ============================================================================
import hashlib
import json
import os
import secrets
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 内存存储（进程生命周期内有效）：{username: {...}}
USERS = {}
PLAYLISTS = {}   # {owner: items_str}
TOKEN_DAYS = 30
SALT_BYTES = 16


def scrypt_hash(pwd_hash: str, salt: str) -> str:
    # 与云函数一致：前端弱哈希再叠 scrypt（N=2^14，见 ms-account.js 注释；
    # maxmem 放宽到 64MB，躲开 OpenSSL 默认 32MB 上限）
    return hashlib.scrypt(pwd_hash.encode(), salt=salt.encode(), n=2**14, r=8, p=1,
                          dklen=32, maxmem=64 * 1024 * 1024).hex()


def issue_token():
    return secrets.token_hex(24)


def make_handler():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):   # 静默默认访问日志，探针输出更清爽
            pass

        def do_POST(self):
            try:
                length = int(self.headers.get("Content-Length", 0))
                p = json.loads(self.rfile.read(length) or b"{}")
            except Exception:
                self._send({"code": 1, "msg": "请求体不是合法 JSON"})
                return

            action = p.get("action", "")
            try:
                res = getattr(self, "a_" + action, None)
                res = res(p) if res else {"code": 1, "msg": "未知 action: " + action}
            except Exception as e:   # noqa: BLE001
                res = {"code": 1, "msg": f"mock 内部错误: {e}"}
            print(f"[mock] {action} username={p.get('username','')} -> {res}")
            self._send(res)

        def _send(self, obj: dict):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # ---- action 实现 ----
        def a_ping(self, p):
            return {"code": 0, "pong": True, "time": int(time.time() * 1000)}

        def a_register(self, p):
            u = str(p.get("username", "")).strip()
            pw = str(p.get("passwordHash", ""))
            if not (2 <= len(u) <= 20):
                return {"code": 1, "msg": "用户名需 2-20 字符"}
            if len(pw) < 6:
                return {"code": 1, "msg": "密码过短"}
            if u in USERS:
                return {"code": 1, "msg": "用户名已被使用"}
            salt = secrets.token_hex(SALT_BYTES)
            token = issue_token()
            USERS[u] = {
                "salt": salt, "pass": scrypt_hash(pw, salt),
                "avatar": p.get("avatar", ""), "deviceId": p.get("deviceId", ""),
                "osVersion": p.get("osVersion", ""), "appVersion": p.get("appVersion", ""),
                "createdAt": int(time.time() * 1000), "lastActiveAt": int(time.time() * 1000),
                "token": token, "tokenExpiresAt": int(time.time() * 1000) + TOKEN_DAYS * 86400000,
            }
            return {"code": 0, "username": u, "avatar": p.get("avatar", ""), "token": token}

        def a_login(self, p):
            u = str(p.get("username", "")).strip()
            rec = USERS.get(u)
            if not rec:
                return {"code": 404, "msg": "云端无此账户"}
            if scrypt_hash(str(p.get("passwordHash", "")), rec["salt"]) != rec["pass"]:
                return {"code": 401, "msg": "密码错误"}
            token = issue_token()
            rec["token"] = token
            rec["tokenExpiresAt"] = int(time.time() * 1000) + TOKEN_DAYS * 86400000
            rec["lastActiveAt"] = int(time.time() * 1000)
            return {"code": 0, "username": u, "avatar": rec["avatar"], "token": token}

        def _auth(self, p):
            u = str(p.get("username", "")).strip()
            rec = USERS.get(u)
            if not rec or rec.get("token") != p.get("token"):
                return None
            if rec["tokenExpiresAt"] < int(time.time() * 1000):
                return None
            rec.setdefault("username", u)   # 云函数文档里存了 username，mock 补齐同构
            return rec

        def a_heartbeat(self, p):
            rec = self._auth(p)
            if not rec:
                return {"code": 401, "msg": "会话失效，请重新登录"}
            rec["lastActiveAt"] = int(time.time() * 1000)
            rec["tokenExpiresAt"] = int(time.time() * 1000) + TOKEN_DAYS * 86400000
            return {"code": 0}

        def a_pushPlaylist(self, p):
            rec = self._auth(p)
            if not rec:
                return {"code": 401, "msg": "会话失效，请重新登录"}
            items = str(p.get("items", ""))
            if len(items) > 512 * 1024:
                return {"code": 1, "msg": "歌单过大（>512KB）"}
            PLAYLISTS[rec["username"]] = items
            return {"code": 0}

        def a_pullPlaylist(self, p):
            rec = self._auth(p)
            if not rec:
                return {"code": 401, "msg": "会话失效，请重新登录"}
            return {"code": 0, "items": PLAYLISTS.get(rec["username"], "")}

    return Handler


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler())
    print(f"[mock] ms-account mock 就绪: http://127.0.0.1:{port}  (Ctrl+C 退出)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[mock] 退出")
