"""DSM-only same-origin desktop gateway. Never grants hardware permissions."""
import hashlib
import os
from http.cookies import CookieError, SimpleCookie
import re
import secrets
import threading
import time
from urllib.parse import urlsplit

from portable_runtime import WebHandler, WebServer

PREFIX = "/ugreen-toolbox/"
COOKIE = "ugreen_toolbox_session"
SESSION_SECONDS = 3600


class Sessions:
    def __init__(self):
        self.tokens = {}
        self.lock = threading.Lock()

    def key(self, cookie):
        try:
            jar = SimpleCookie()
            jar.load(cookie)
            token = jar[COOKIE].value
            if re.fullmatch(r"[A-Za-z0-9_-]{43}", token):
                return hashlib.sha256(token.encode()).digest()
        except (CookieError, KeyError):
            pass
        return None

    def prune(self):
        now = time.monotonic()
        self.tokens = {key: expires for key, expires in self.tokens.items() if expires > now}

    def issue(self):
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.prune()
            if len(self.tokens) >= 128:
                self.tokens.pop(next(iter(self.tokens)))
            self.tokens[hashlib.sha256(token.encode()).digest()] = time.monotonic() + SESSION_SECONDS
        return token

    def verify(self, cookie):
        key = self.key(cookie)
        with self.lock:
            self.prune()
            return key is not None and key in self.tokens

    def revoke(self, cookie):
        with self.lock:
            self.tokens.pop(self.key(cookie), None)


class DesktopHandler(WebHandler):
    frame_options = "SAMEORIGIN"
    frame_ancestors = "'self'"

    def health_socket(self):
        # The desktop and log collector remain available if hardware startup fails.
        return os.environ.get("UGREEN_LOG_SOCKET", "/run/ugreen-nas-native/ipc/logs.sock")

    def api_response(self, body):
        if urlsplit(self.path).path == "/api/logs/export":
            if self.command != "POST" or self.path != "/api/logs/export" or body:
                return 400, {"ok": False, "error": "日志导出只接受空 POST 请求"}
            from portable_runtime import call_broker
            return call_broker(self.health_socket(), {"method": "POST", "target": "/logs/export", "body": ""})
        return super().api_response(body)

    def csrf_allowed(self):
        # DSM's registered nginx location overwrites these headers. Never trust
        # a browser-supplied forwarded origin/host or accept a missing Origin.
        expected = self.server.public_origin or "https://" + self.headers.get("Host", "")
        return (self.client_address[0] in {"127.0.0.1", "::1"} and
                self.headers.get("X-UGREEN-DSM-Proxy") == "1" and
                self.headers.get("X-Forwarded-Proto") == "https" and
                self.headers.get("Origin") == expected and
                self.headers.get("X-UGREEN-Request") == "1" and
                self.headers.get("Sec-Fetch-Site") in {None, "same-origin", "none"})

    def is_authenticated(self):
        return (self.server.sessions.verify(self.headers.get("Cookie", "")) or
                bool(self.headers.get("Authorization")) and super().is_authenticated())

    def send_payload(self, status, payload, content_type="application/json; charset=utf-8", authenticate=False, headers=()):
        if status == 401:
            headers = (*headers, ("X-UGREEN-Login", "required"))
        # An iframe must use the explicit login form, not a browser Basic-auth
        # dialog, which browsers may suppress for embedded documents.
        return super().send_payload(status, payload, content_type, False, headers)

    def handle_request(self):
        target = urlsplit(self.path)
        if len(self.path) > 8192 or target.scheme or target.netloc or target.fragment:
            return super().handle_request()
        if target.path in {"/session", "/session/logout"}:
            if self.command != "POST":
                return self.send_payload(405, {"ok": False, "error": "method not allowed"})
            if self.headers.get("Transfer-Encoding") or self.headers.get_all("Content-Length", ["0"]) != ["0"]:
                return self.send_payload(400, {"ok": False, "error": "invalid request framing"})
            if not self.csrf_allowed():
                return self.send_payload(403, {"ok": False, "error": "请通过 DSM HTTPS 同源入口登录"})
            token, age = "", 0
            if target.path == "/session":
                if not self.server.auth.verify(self.headers.get("Authorization", ""), self.client_address[0]):
                    return self.send_payload(401, {"ok": False, "error": "密码错误或尝试过多，请稍后重试"})
                self.server.sessions.revoke(self.headers.get("Cookie", ""))
                token, age = self.server.sessions.issue(), SESSION_SECONDS
            else:
                self.server.sessions.revoke(self.headers.get("Cookie", ""))
            cookie = f"{COOKIE}={token}; Path={PREFIX}; Max-Age={age}; Secure; HttpOnly; SameSite=Strict"
            return self.send_payload(200, {"ok": True}, headers=(("Set-Cookie", cookie),))
        public_files = {"/native-login.js": "text/javascript; charset=utf-8", "/native-login.css": "text/css; charset=utf-8"}
        if self.command == "GET" and (target.path in public_files or target.path in {"/", "/index.html"} and not self.is_authenticated()):
            name = "native-login.html" if target.path in {"/", "/index.html"} else target.path[1:]
            file = self.server.root / "www" / name
            return self.send_payload(200, file.read_bytes(), public_files.get(target.path, "text/html; charset=utf-8"))
        return super().handle_request()


class DesktopWebServer(WebServer):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.sessions = Sessions()
        self.RequestHandlerClass = DesktopHandler
