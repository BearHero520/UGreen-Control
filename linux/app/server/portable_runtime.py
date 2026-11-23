#!/usr/bin/env python3
"""Unprivileged HTTP frontend and local-only hardware application adapter.

Hardware operations remain in api.cgi -> bundled LED CLI / upstream ugreenctl.
This module never opens hardware devices or implements register access.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import configparser
import hashlib
import hmac
import http.server
import json
import logging
import mimetypes
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import socketserver
import stat
import struct
import subprocess
import threading
import time
from urllib.parse import urlsplit

APP_ROOT = Path(__file__).resolve().parents[1]
MAX_BODY = 65536
MAX_REPLY = 8 * 1024 * 1024
LOG = logging.getLogger("ugreen-runtime")
READ_ROUTES = {
    "/status", "/hardware/status", "/bios/status", "/bios/telemetry",
    "/bios/fan-curve", "/mapping", "/settings", "/logs",
    "/about/info", "/about/readme", "/update/check", "/platform/status",
}
LED_ROUTES = {
    "/settings", "/mode", "/remap", "/power26/apply", "/led/set",
    "/led/off", "/daemon/start", "/daemon/stop",
}
BIOS_ROUTES = {"/bios/confirmation", "/bios/fan", "/bios/fan-curve", "/bios/startup", "/bios/wol"}
LOCAL_ROUTES = {"/logs/config", "/logs/clear", "/logs/client", "/logs/export"}
UNSUPPORTED_ROUTES = {"/driver/install", "/driver/unload", "/bios/power-schedule", "/bios/fan/mode", "/hardware/diagnostics"}
LOCK_NAMES = (
    "llled-hardware.lock", "ugreenctl-led-mcu.lock", "ugreenctl-it8613.lock",
    "ugreenctl-fand.lock", "ugreenctl-wol.lock", "ugreenctl-rtc.lock",
)


def password_record(password: str) -> dict:
    if not 12 <= len(password) <= 256 or ":" in password:
        raise ValueError("密码须为 12–256 个字符，且不能包含冒号")
    salt = secrets.token_bytes(16)
    digest = hashlib.scrypt(password.encode(), salt=salt, n=16384, r=8, p=1)
    return {"username": "admin", "salt": salt.hex(), "password_hash": digest.hex()}


class Authentication:
    def __init__(self, path: Path):
        if path.is_symlink():
            raise ValueError("认证文件不能为符号链接")
        meta = path.stat()
        if not stat.S_ISREG(meta.st_mode) or (os.name == "posix" and meta.st_mode & 0o007):
            raise ValueError("auth.json 必须为普通文件，且不能允许其他用户访问")
        record = json.loads(path.read_text(encoding="utf-8"))
        self.username = record["username"]
        self.salt = bytes.fromhex(record["salt"])
        self.digest = bytes.fromhex(record["password_hash"])
        if self.username != "admin" or len(self.salt) != 16 or len(self.digest) != 64:
            raise ValueError("invalid authentication file")
        self.cached = None
        self.failures = {}
        self.lock = threading.Lock()

    def verify(self, header: str, address: str) -> bool:
        with self.lock:
            now = time.monotonic()
            self.failures = {k: v for k, v in self.failures.items() if v[1] > now}
            if self.failures.get(address, (0, 0))[0] >= 20:
                return False
            try:
                scheme, payload = header.split(" ", 1)
                if scheme.lower() != "basic" or len(payload) > 2048:
                    raise ValueError("invalid credentials")
                raw = base64.b64decode(payload, validate=True)
                username, password = raw.decode("utf-8").split(":", 1)
                if len(password) > 256:
                    raise ValueError("invalid credentials")
                cache_key = hashlib.sha256(raw).digest()
                if self.cached is not None and hmac.compare_digest(cache_key, self.cached):
                    return True
                calculated = hashlib.scrypt(password.encode(), salt=self.salt, n=16384, r=8, p=1)
                if hmac.compare_digest(username.encode(), self.username.encode()) and hmac.compare_digest(calculated, self.digest):
                    self.cached = cache_key
                    self.failures.pop(address, None)
                    return True
            except (ValueError, UnicodeError, binascii.Error):
                pass
            if len(self.failures) >= 2048:
                self.failures.pop(next(iter(self.failures)))
            attempts, expiry = self.failures.get(address, (0, now + 300))
            self.failures[address] = (attempts + 1, expiry)
            return False


def result(status: int, **body) -> tuple[int, dict]:
    return status, body


def validate_settings(body: str, defaults: Path) -> None:
    """Keep generic LED settings from bypassing protected BIOS APIs or shell syntax."""
    schema = configparser.ConfigParser()
    schema.read(defaults, encoding="utf-8")
    for line in body.splitlines():
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([a-z_]+)\.([a-z_]+)=([A-Za-z0-9 _.,+-]{0,128})", line)
        if not match:
            raise ValueError("配置行格式无效")
        section, key, value = match.groups()
        if section in {"bios", "fan_curve", "power_schedule"} or not schema.has_option(section, key):
            raise ValueError("该配置项不能通过通用灯光接口修改")
        default = schema.get(section, key)
        if section == "hardware" and value != {"backend": "cli", "profile": "auto", "write_protocol": "auto"}[key]:
            raise ValueError("本平台必须使用内置 CLI 与真实 DMI 自动检测")
        if default in {"true", "false"} and value not in {"true", "false"}:
            raise ValueError("开关值无效")
        if default.isdigit() and (not value.isdigit() or not 0 <= int(value) <= 86400):
            raise ValueError("数值无效")
        if (section.endswith("brightness") or key == "brightness") and int(value) > 255:
            raise ValueError("亮度必须在 0–255 之间")
        choices = {("mode", "global"): {"off", "on", "smart"},
                   ("power26", "color"): {"white", "red"},
                   ("power26", "effect"): {"steady", "fast", "slow", "breath", "network"},
                   ("behavior", "disk_map_mode"): {"auto", "position", "disk"},
                   ("logging", "level"): {"debug", "info", "warn", "error"}}
        if (section, key) in choices and value not in choices[section, key]:
            raise ValueError("配置值无效")
        if key.endswith("color") or section.endswith("colors"):
            if value != "off" and not (re.fullmatch(r"\d{1,3} \d{1,3} \d{1,3}", value) and all(int(n) <= 255 for n in value.split())):
                raise ValueError("颜色值无效")


class Application:
    def __init__(self, root: Path = APP_ROOT, environ=None):
        self.root = root.resolve()
        self.environ = dict(os.environ if environ is None else environ)
        self.led_enabled = self.environ.get("UGREEN_ALLOW_LED_CONTROL") == "true"
        self.bios_enabled = self.environ.get("UGREEN_ALLOW_BIOS_CONTROL") == "true"
        self.platform = self.environ.get("UGREEN_PLATFORM", "linux")
        self.distribution = self.environ.get("UGREEN_DISTRIBUTION", "container")
        self.version = json.loads((root / "app-info.json").read_text(encoding="utf-8"))["version"]

    def dispatch(self, request: dict) -> tuple[int, dict]:
        method, target, body = request.get("method"), request.get("target", ""), request.get("body", "")
        if not all(isinstance(v, str) for v in (method, target, body)):
            return result(400, ok=False, error="invalid request")
        if len(target) > 8192 or len(body.encode()) > MAX_BODY or re.search(r"[\x00-\x1f\x7f]", target):
            return result(400, ok=False, error="invalid request size or target")
        parsed = urlsplit(target)
        if parsed.scheme or parsed.netloc or parsed.fragment or not re.fullmatch(r"/[a-z0-9/-]+", parsed.path):
            return result(400, ok=False, error="invalid API path")
        path = parsed.path
        if path in UNSUPPORTED_ROUTES:
            return result(501, ok=False, error="此容器版本尚未适配该功能；定时开关机请使用 NAS 系统设置")
        if path not in READ_ROUTES | LED_ROUTES | BIOS_ROUTES | LOCAL_ROUTES:
            return result(404, ok=False, error="unknown API")
        if method == "GET":
            if path not in READ_ROUTES:
                return result(405, ok=False, error="method not allowed")
        elif method == "POST":
            if path not in LED_ROUTES | BIOS_ROUTES | LOCAL_ROUTES:
                return result(405, ok=False, error="method not allowed")
            if path in LED_ROUTES and not self.led_enabled:
                return result(403, ok=False, error="部署配置未授权灯光控制")
            if path in BIOS_ROUTES and not self.bios_enabled:
                return result(403, ok=False, error="部署配置未授权 BIOS/风扇控制")
            if path == "/settings":
                try:
                    validate_settings(body, self.root / "server/default_settings.conf")
                except ValueError as exc:
                    return result(400, ok=False, error=str(exc))
        else:
            return result(405, ok=False, error="method not allowed")
        if method == "POST" and path == "/bios/fan-curve":
            return result(501, ok=False, error="容器温控曲线尚未完成进程退出及风扇接管验证，暂不开放启停")
        if path == "/logs/export":
            if method != "POST" or parsed.query or body:
                return result(400, ok=False, error="日志导出只接受空 POST 请求")
            from log_export import export_logs
            return 200, export_logs(self.environ.get("VAR_DIR", "/var/lib/ugreen-nas"), include_journal=False)
        if path == "/platform/status":
            return result(200, ok=True, platform=self.platform, experimental=True,
                          led_control=self.led_enabled, bios_control=self.bios_enabled,
                          power_schedule=False, fan_curve=False, hardware_validation="not-performed")
        if path == "/about/info":
            return result(200, ok=True, display_name="UGREEN工具箱", version=self.version,
                          platform=self.platform, distribution=self.distribution, maintainer="BearHero520", license="AGPL-3.0-only",
                          repository_url="https://github.com/BearHero520/LLLED_FPK", qq_group="1108837172")
        if path == "/about/readme":
            readme = self.root / "README.md"
            if not readme.is_file():
                readme = self.root.parent / "README.md"
            return result(200, ok=True, content=readme.read_text(encoding="utf-8"),
                          source_url="https://github.com/BearHero520/LLLED_FPK/tree/synology-truenas/linux",
                          reachable=True, cached=True)
        if path == "/update/check":
            message = "群晖 SPK 请从套件中心手动安装新版" if self.distribution == "spk" else "容器版本请使用本平台部署文件手动更新镜像"
            return result(200, ok=True, reachable=False, supported=False, current_version=self.version, message=message)
        # Do not pass browser headers, credentials, environment or executable paths.
        return self.run_cgi(method, parsed.path, parsed.query, body)

    def cgi_environment(self) -> dict:
        env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "HOME": "/tmp"}
        for key in ("VAR_DIR", "UGREEN_RUNTIME_DIR", "SYSTEM_INFO_PROC_ROOT", "SYSTEM_INFO_HOSTNAME_FILE",
                    "SYSTEM_INFO_OS_RELEASE", "NET_SYS_CLASS_DIR", "UGREEN_PLATFORM", "UGREEN_NETWORK_MODE", "UGREEN_DISK_MODE"):
            if key in self.environ:
                env[key] = self.environ[key]
        env["APP_ROOT"] = str(self.root)
        env["SERVER_DIR"] = str(self.root / "server")
        env["UGREEN_INTERNAL_CGI"] = "1"
        return env

    def run_cgi(self, method: str, path: str, query: str, body: str) -> tuple[int, dict]:
        payload = body.encode()
        env = self.cgi_environment()
        env.update(REQUEST_METHOD=method, PATH_INFO=path, QUERY_STRING=query, CONTENT_LENGTH=str(len(payload)))
        child = subprocess.Popen(["/bin/bash", str(self.root / "ui/api.cgi")], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 start_new_session=True)
        try:
            output, errors = child.communicate(payload, timeout=60)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.communicate()
            return result(504, ok=False, error="hardware API timeout")
        if child.returncode or len(output) > MAX_REPLY:
            LOG.error("CGI failed: path=%s exit=%s", path, child.returncode)
            return result(502, ok=False, error="application adapter failed; check hardware logs")
        try:
            headers, raw = output.replace(b"\r\n", b"\n").split(b"\n\n", 1)
            status_line = next(line for line in headers.decode().splitlines() if line.startswith("Status: "))
            status = int(status_line.split()[1])
            data = json.loads(raw)
            if not isinstance(data, dict) or not 200 <= status <= 599:
                raise ValueError("invalid CGI response")
            if path == "/bios/status":
                data["power_schedule"] = {"available": False, "enabled": False, "error": "请使用 NAS 系统定时开关机"}
            return status, data
        except (ValueError, StopIteration, UnicodeError):
            LOG.error("Invalid CGI response: path=%s", path)
            return result(502, ok=False, error="invalid application adapter response")


def call_broker(path: str, request: dict) -> tuple[int, dict]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(70)
        connection.connect(path)
        connection.sendall(json.dumps(request, ensure_ascii=False).encode() + b"\n")
        with connection.makefile("rb") as reader:
            raw = reader.readline(MAX_REPLY + 1)
        if len(raw) > MAX_REPLY or not raw.endswith(b"\n"):
            raise ValueError("invalid broker response")
        response = json.loads(raw)
        return response["status"], response["body"]


class BrokerHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(10)
        credentials = self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
        _, uid, _ = struct.unpack("3i", credentials)
        if uid not in {0, self.server.web_uid}:
            return
        raw = self.rfile.readline(4 * MAX_BODY + 1)
        try:
            if not raw.endswith(b"\n") or len(raw) > 4 * MAX_BODY:
                raise ValueError("request too large")
            request = json.loads(raw)
            if not isinstance(request, dict):
                raise ValueError("invalid request")
            if request == {"method": "GET", "target": "/healthz"}:
                status, body = result(200, ok=True)
            else:
                status, body = self.server.application.dispatch(request)
        except (ValueError, TypeError, OSError):
            LOG.exception("Broker request failed")
            status, body = result(400, ok=False, error="invalid broker request")
        self.wfile.write(json.dumps({"status": status, "body": body}, ensure_ascii=False).encode() + b"\n")


class WebServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address, *, auth, broker_path, root, public_origin=""):
        self.auth, self.broker_path, self.root, self.public_origin = auth, broker_path, root, public_origin
        self.slots = threading.BoundedSemaphore(24)
        super().__init__(address, WebHandler)

    def process_request(self, request, address):
        self.slots.acquire()
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class WebHandler(http.server.BaseHTTPRequestHandler):
    server_version = "UGREEN"
    frame_options = "DENY"
    frame_ancestors = "'none'"
    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, format, *args):
        # Request targets can contain BIOS parameters; never log credentials or query strings.
        LOG.info("HTTP method=%s", self.command)

    def send_payload(self, status, payload, content_type="application/json; charset=utf-8", authenticate=False, headers=()):
        raw = json.dumps(payload, ensure_ascii=False).encode() if isinstance(payload, dict) else payload
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", self.frame_options)
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self' data:; connect-src 'self'; frame-ancestors " + self.frame_ancestors + "; object-src 'none'; base-uri 'self'; form-action 'self'")
        if authenticate:
            self.send_header("WWW-Authenticate", 'Basic realm="UGREEN NAS", charset="UTF-8"')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        self.handle_request()

    def do_POST(self):
        self.handle_request()

    def is_authenticated(self):
        return self.server.auth.verify(self.headers.get("Authorization", ""), self.client_address[0])

    def csrf_allowed(self):
        origin = self.headers.get("Origin")
        expected = self.server.public_origin or "http://" + self.headers.get("Host", "")
        return (self.headers.get("X-UGREEN-Request") == "1" and
                self.headers.get("Sec-Fetch-Site") != "cross-site" and (not origin or origin == expected))

    def health_socket(self):
        return self.server.broker_path

    def api_response(self, body):
        return call_broker(self.server.broker_path, {
            "method": self.command, "target": self.path[4:], "body": body,
        })

    def handle_request(self):
        if len(self.path) > 8192:
            return self.send_payload(414, {"ok": False, "error": "URI too long"})
        parsed = urlsplit(self.path)
        if parsed.scheme or parsed.netloc or parsed.fragment:
            return self.send_payload(400, {"ok": False, "error": "invalid request target"})
        if self.command == "GET" and self.path == "/healthz":
            try:
                status, body = call_broker(self.health_socket(), {"method": "GET", "target": "/healthz"})
                healthy = status == 200 and body.get("ok") is True
            except (OSError, ValueError):
                healthy = False
            return self.send_payload(200 if healthy else 503, {"ok": healthy})
        if not self.is_authenticated():
            return self.send_payload(401, {"ok": False, "error": "需要登录"}, authenticate=True)
        if self.command == "POST" and not self.csrf_allowed():
            return self.send_payload(403, {"ok": False, "error": "cross-origin request rejected"})
        if parsed.path.startswith("/api/"):
            if self.headers.get("Transfer-Encoding") or len(self.headers.get_all("Content-Length", [])) > 1:
                return self.send_payload(400, {"ok": False, "error": "invalid request framing"})
            length = self.headers.get("Content-Length", "0")
            if not re.fullmatch(r"[0-9]{1,6}", length) or int(length) > MAX_BODY:
                return self.send_payload(413, {"ok": False, "error": "request body too large"})
            try:
                raw = self.rfile.read(int(length))
                if len(raw) != int(length):
                    raise ValueError("incomplete body")
                body = raw.decode("utf-8")
                status, response = self.api_response(body)
            except (OSError, ValueError):
                return self.send_payload(503, {"ok": False, "error": "服务未就绪，请检查服务日志和目录权限"})
            return self.send_payload(status, response)
        if self.command != "GET":
            return self.send_payload(405, {"ok": False, "error": "method not allowed"})
        # No URL decoding, directory listing, CGI execution or source file serving.
        if not re.fullmatch(r"/(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]*", parsed.path):
            return self.send_payload(404, {"ok": False, "error": "not found"})
        root = (self.server.root / "www").resolve()
        target = (root / ("index.html" if parsed.path == "/" else parsed.path.lstrip("/"))).resolve()
        # DSM 7.2.2 supplies Python 3.8 (Path.is_relative_to was added in 3.9).
        if root not in target.parents or not target.is_file() or target.suffix not in {".html", ".js", ".css", ".png", ".svg", ".ico", ".woff", ".woff2", ".ttf"}:
            return self.send_payload(404, {"ok": False, "error": "not found"})
        self.send_payload(200, target.read_bytes(), mimetypes.guess_type(target)[0] or "application/octet-stream")


def verify_payload(root: Path) -> None:
    receipt = json.loads((root / "server/lib/ugreenctl/build-info.json").read_text(encoding="utf-8"))
    for name, expected in receipt["files"].items():
        path = root / ("server/lib/ugreenctl" if name.startswith("models/") else "server/bin") / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"hardware payload checksum mismatch: {name}")


def notify_systemd_ready():
    address = os.environ.get("NOTIFY_SOCKET")
    if address:
        if address.startswith("@"):
            address = "\0" + address[1:]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notification:
            notification.connect(address)
            notification.sendall(b"READY=1")


def run_broker(path: Path):
    import fcntl  # Linux only; keep module importable for Windows fixture tests.
    if os.geteuid() != 0:
        raise ValueError("hardware broker must run as container root")
    verify_payload(APP_ROOT)
    uid = int(os.environ.get("UGREEN_WEB_UID", "568"))
    gid = int(os.environ.get("UGREEN_WEB_GID", "568"))
    if uid <= 0 or gid <= 0:
        raise ValueError("web UID/GID must be nonzero")
    # Prepared by the deployment tool; shared with other containers and upstream.
    for name in LOCK_NAMES:
        fd = os.open("/run/" + name, os.O_RDWR | os.O_NOFOLLOW)
        try:
            meta = os.fstat(fd)
            if not stat.S_ISREG(meta.st_mode) or meta.st_uid != 0 or meta.st_nlink != 1 or meta.st_mode & 0o077:
                raise ValueError("invalid shared lock: " + name)
        finally:
            os.close(fd)
    guard = os.open("/run/llled-hardware.lock", os.O_RDWR | os.O_NOFOLLOW)
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.umask(0o077)
    path.parent.mkdir(parents=True, exist_ok=True)
    os.chown(path.parent, 0, gid)
    path.parent.chmod(0o750)
    if path.exists():
        if not stat.S_ISSOCK(path.lstat().st_mode):
            raise ValueError("broker socket path is not a socket")
        path.unlink()
    data = Path(os.environ.get("VAR_DIR", "/var/lib/ugreen-nas"))
    data.mkdir(parents=True, exist_ok=True)
    data.chmod(0o700)
    runtime = Path(os.environ.get("UGREEN_RUNTIME_DIR", "/run/ugreen-nas"))
    runtime.mkdir(parents=True, exist_ok=True)
    application = Application()
    with socketserver.UnixStreamServer(str(path), BrokerHandler) as server:
        server.application, server.web_uid = application, uid
        os.chown(path, 0, gid)
        path.chmod(0o660)
        # Finish an in-flight upstream hardware transaction before shutting down.
        def stop(signum, frame):
            threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        try:
            notify_systemd_ready()
            server.serve_forever(poll_interval=0.5)
        finally:
            # Stop only our container's LED process. No BIOS restore/write on startup/shutdown.
            subprocess.run(["/bin/bash", str(APP_ROOT / "server/led_daemon.sh"), "stop"],
                           env=application.cgi_environment(), timeout=10, check=False)
            path.unlink(missing_ok=True)
            os.close(guard)


def main(web_server_class=WebServer):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=["web", "broker", "verify", "healthcheck"])
    parser.add_argument("--socket", default=os.environ.get("UGREEN_BROKER_SOCKET", "/ipc/hardware.sock"))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if args.role == "verify":
        verify_payload(APP_ROOT)
    elif args.role == "healthcheck":
        status, _ = call_broker(args.socket, {"method": "GET", "target": "/healthz"})
        raise SystemExit(0 if status == 200 else 1)
    elif args.role == "broker":
        run_broker(Path(args.socket))
    else:
        if os.name == "posix" and os.geteuid() == 0:
            raise ValueError("Web 服务必须以普通用户运行")
        auth = Authentication(Path(os.environ.get("UGREEN_AUTH_FILE", "/auth/auth.json")))
        with web_server_class((os.environ.get("UGREEN_BIND", "127.0.0.1"), int(os.environ.get("PORT", "5088"))),
                       auth=auth, broker_path=args.socket, root=APP_ROOT,
                       public_origin=os.environ.get("UGREEN_PUBLIC_ORIGIN", "")) as server:
            server.serve_forever()


if __name__ == "__main__":
    main()
