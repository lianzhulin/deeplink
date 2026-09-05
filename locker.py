#!/usr/bin/env python3
"""
电子寄存柜原型 - 单文件 Python 实现
====================================
特性:
  * 零依赖, 仅用标准库 (http.server + threading + json)
  * 凭证为三位数字 001-999 (可通过 CAPACITY 扩容)
  * 寄存后返回凭证, 取回需输入正确凭证
  * 默认 TTL=300 秒 (5 分钟), 过期自动销毁
  * 后期可扩展: 支持更多数据类型 / 持久化存储 / 分布式扩容

启动: python3 locker.py [端口号]   (默认 8000)
"""

import sys
import json
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# ---------- 可配置项 ----------
CAPACITY = 999                       # 凭证容量, 改大可扩容 (如 9999 四位)
TOKEN_LEN = len(str(CAPACITY))       # 凭证长度自动跟随 CAPACITY
TTL_SECONDS = 5 * 60                 # 寄存有效期, 秒
CLEANUP_INTERVAL = 10                # 过期清理线程间隔, 秒
# ------------------------------

# 柜子: {token(str): {"data": str, "expires_at": float}}
lockers = {}
lockers_lock = threading.Lock()


def cleanup_loop():
    """后台线程: 定期清理过期寄存"""
    while True:
        now = time.time()
        with lockers_lock:
            expired = [t for t, v in lockers.items() if v["expires_at"] <= now]
            for t in expired:
                del lockers[t]
        time.sleep(CLEANUP_INTERVAL)


def allocate_token():
    """从 001..CAPACITY 找一个空位, 格式化返回. 全部占满返回 None."""
    for i in range(1, CAPACITY + 1):
        token = str(i).zfill(TOKEN_LEN)
        if token not in lockers:
            return token
    return None


# ---------- 页面 (纯 HTML, 内嵌 CSS/JS, 无外部资源) ----------
# 注意: 下面两个页面使用 f-string, JS 里的 `${xxx}` 必须写成 `${{xxx}}`

PAGE_STYLE = """
<style>
  * { box-sizing: border-box; }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
         max-width: 520px; margin: 60px auto; padding: 0 16px; color: #222; }
  h1 { font-size: 22px; text-align: center; margin-bottom: 24px; }
  .tabs { display: flex; justify-content: center; gap: 8px; margin-bottom: 28px; }
  .tab { padding: 6px 16px; border: 1px solid #ccc; border-radius: 6px;
         text-decoration: none; color: #555; font-size: 14px; }
  .tab.active { background: #2563eb; color: #fff; border-color: #2563eb; }
  textarea, input[type=text] { width: 100%; padding: 10px; border: 1px solid #ccc;
       border-radius: 6px; font-size: 14px; font-family: inherit; }
  textarea { min-height: 110px; resize: vertical; }
  button { width: 100%; padding: 12px; background: #2563eb; color: #fff;
       border: none; border-radius: 6px; font-size: 15px; cursor: pointer; margin-top: 12px; }
  button:hover { background: #1d4ed8; }
  button:disabled { background: #93c5fd; cursor: not-allowed; }
  button.copy { width: auto; padding: 6px 14px; background: #16a34a; font-size: 13px; margin-top: 8px; }
  .result { margin-top: 20px; padding: 16px; border-radius: 8px;
       background: #f1f5f9; font-size: 14px; word-break: break-all; white-space: pre-wrap; }
  .token-big { font-size: 48px; font-weight: bold; letter-spacing: 12px;
       text-align: center; color: #dc2626; margin: 8px 0; }
  .hint { color: #666; font-size: 13px; text-align: center; margin-top: 6px; }
  .error { color: #dc2626; }
  .success { color: #16a34a; }
</style>
"""


def _page_base(active_tab: str, body: str) -> str:
    return (
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>电子寄存柜</title>" + PAGE_STYLE + "</head><body>"
        '<h1>电子寄存柜</h1>'
        '<div class="tabs">'
        f'<a class="tab {"active" if active_tab=="store" else ""}" href="/store">寄存</a>'
        f'<a class="tab {"active" if active_tab=="retrieve" else ""}" href="/retrieve">取回</a>'
        "</div>" + body + "</body></html>"
    )


STORE_BODY = f"""
<form id="storeForm">
  <textarea id="data" placeholder="输入要寄存的内容, 如网络链接 https://..."></textarea>
  <button type="submit">存入</button>
</form>
<div id="storeResult"></div>
<p class="hint">有效期 {TTL_SECONDS // 60} 分钟 · 容量 {CAPACITY} 个柜子</p>
<script>
document.getElementById('storeForm').addEventListener('submit', async e => {{
  e.preventDefault();
  const data = document.getElementById('data').value.trim();
  if (!data) {{ alert('请输入内容'); return; }}
  const res = await fetch('/api/store', {{
    method: 'POST', headers: {{'Content-Type':'application/json'}},
    body: JSON.stringify({{data}})
  }});
  const j = await res.json();
  const box = document.getElementById('storeResult');
  if (j.ok) {{
    box.innerHTML = '<div class="result success">'
      + '已存入, 请记住凭证号:'
      + '<div class="token-big">' + j.token + '</div>'
      + '有效期至 ' + j.expire_time + '</div>';
  }} else {{
    box.innerHTML = '<div class="result error">' + j.msg + '</div>';
  }}
}});
</script>
"""
PAGE_STORE = _page_base("store", STORE_BODY)


RETRIEVE_BODY = f"""
<form id="retrieveForm">
  <input type="text" id="token" placeholder="输入凭证号 (三位数字)" maxlength="{TOKEN_LEN}">
  <button type="submit">取回</button>
</form>
<div id="retrieveResult"></div>
<script>
document.getElementById('retrieveForm').addEventListener('submit', async e => {{
  e.preventDefault();
  const token = document.getElementById('token').value.trim();
  if (!/^\\d+$/.test(token)) {{ alert('请输入数字凭证'); return; }}
  const res = await fetch('/api/get?token=' + encodeURIComponent(token));
  const j = await res.json();
  const box = document.getElementById('retrieveResult');
  if (j.ok) {{
    box.innerHTML = '<div class="result success">内容如下 '
      + '(点击右侧按钮复制):<textarea id="out" readonly>'
      + j.data + '</textarea>'
      + '<button type="button" class="copy" '
      + 'onclick="navigator.clipboard.writeText(document.getElementById(\\'out\\').value)">'
      + '一键复制</button></div>';
  }} else {{
    box.innerHTML = '<div class="result error">' + j.msg + '</div>';
  }}
}});
</script>
"""
PAGE_RETRIEVE = _page_base("retrieve", RETRIEVE_BODY)


# ---------- HTTP Handler ----------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[{self.log_date_time_string()}] {fmt % args}")

    def _send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html: str):
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/store"):
            self._send_html(PAGE_STORE)
        elif path == "/retrieve":
            self._send_html(PAGE_RETRIEVE)
        elif path == "/api/get":
            token = parse_qs(urlparse(self.path).query).get("token", [""])[0]
            self._handle_get(token)
        else:
            self._send_json({"ok": False, "msg": "not found"}, 404)

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/store":
            self._handle_store()
        else:
            self._send_json({"ok": False, "msg": "not found"}, 404)

    def _handle_store(self):
        length = int(self.headers.get("Content-Length", 0))
        try:
            payload = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._send_json({"ok": False, "msg": "请求格式错误"}, 400)
            return

        data = (payload.get("data") or "").strip()
        if not data:
            self._send_json({"ok": False, "msg": "寄存内容不能为空"}, 400)
            return
        if len(data) > 8192:
            self._send_json({"ok": False, "msg": "内容过长, 当前上限 8KB"}, 400)
            return

        with lockers_lock:
            token = allocate_token()
            if token is None:
                self._send_json({"ok": False, "msg": f"柜子已满 (上限 {CAPACITY}), 请稍后再试"}, 503)
                return
            expires_at = time.time() + TTL_SECONDS
            lockers[token] = {"data": data, "expires_at": expires_at}

        expire_str = time.strftime("%H:%M:%S", time.localtime(expires_at))
        self._send_json({"ok": True, "token": token, "expire_time": expire_str})

    def _handle_get(self, token: str):
        # 只保留数字字符, 补齐长度, 截到上限
        token = "".join(c for c in token if c.isdigit()).zfill(TOKEN_LEN)[:TOKEN_LEN]
        if not token.isdigit():
            self._send_json({"ok": False, "msg": "凭证格式错误"}, 400)
            return

        now = time.time()
        with lockers_lock:
            item = lockers.get(token)
            if item is None or item["expires_at"] <= now:
                if item:
                    del lockers[token]  # 懒清理
                self._send_json({"ok": False, "msg": "凭证无效或已过期"}, 404)
                return
            # 取回即销毁: 一次性凭证
            data = item["data"]
            del lockers[token]

        self._send_json({"ok": True, "data": data})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000

    t = threading.Thread(target=cleanup_loop, daemon=True)
    t.start()

    server = HTTPServer(("0.0.0.0", port), Handler)
    print(f"电子寄存柜已启动  http://0.0.0.0:{port}")
    print(f"  容量: {CAPACITY}  凭证长度: {TOKEN_LEN}  有效期: {TTL_SECONDS}s")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
        server.server_close()


if __name__ == "__main__":
    main()
