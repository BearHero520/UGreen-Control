"""Bounded, read-only, redacted application log exports. No hardware access."""
import base64
from datetime import datetime, timezone
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import socketserver
import stat
import struct
import subprocess
import threading
import time
import unicodedata
from urllib.parse import unquote
import zipfile

FILE_LIMIT = 256 * 1024
JOURNAL_LIMIT = 1024 * 1024
UNITS = ("pkg-UGreenToolbox-hardware.service", "pkg-UGreenToolbox-logs.service",
         "pkg-UGreenToolbox-web.service", "pkgctl-UGreenToolbox.service")
LOG_NAMES = ("app.log",) + tuple("app.log." + str(i) for i in range(1, 11))
SECRET = re.compile(r"password|passwd|passphrase|token|secret|authorization|cookie|credential|"
                    r"api[_-]?key|access[_-]?key|private[_-]?key|session|csrf|synosid|"
                    r"\b(?:bearer|basic)\s+\S+|密码|口令|令牌|密钥|凭据|"
                    r"(?:^|[\W_])(?:sid|auth|salt|confirm|pwd)(?:[\W_]|$)", re.I)
IDENTITY = re.compile(r"(?:\bhost(?:[ _-]?name)?|\buser(?:[ _-]?name)?|serial(?:[ _-]?(?:number|no))?|"
                      r"machine[ _-]?id|disk[ _-]?id|wwn|\bs/n\b|\bsn\b|主机名|用户名|序列号)\s*[\"']?\s*[:=：]", re.I)
RECORD = re.compile(r"^\s*\[?\d{4}[-/]\d{2}[-/]\d{2}[T ]\d{2}:\d{2}:")
OPAQUE = re.compile(r"(?<![\w])[A-Za-z0-9_+/=-]{32,}(?![\w])")


def redact(text):
    """Fail closed on credential-bearing records and their continuations."""
    text = unicodedata.normalize("NFKC", text)
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    text = "".join(c for c in text if c in "\n\t" or unicodedata.category(c) not in {"Cc", "Cf"})
    output, pem, continuation = [], False, False
    for line in text.splitlines():
        view = line
        for _ in range(2):
            view = unquote(view)
            view = re.sub(r"\\u([0-9a-fA-F]{4})|\\x([0-9a-fA-F]{2})",
                          lambda m: chr(int(m.group(1) or m.group(2), 16)), view)
        if "-----BEGIN " in view:
            pem = True
        if pem:
            output.append("[REDACTED PEM material]")
            if "-----END " in view:
                pem = False
            continue
        if RECORD.match(view):
            continuation = False
        if SECRET.search(view) or re.search(r"[a-z][a-z0-9+.-]*://[^\s/]+@", view, re.I):
            continuation = True
            output.append("[REDACTED credential-bearing log record]")
            continue
        if continuation:
            output.append("[REDACTED sensitive continuation]")
            continue
        if IDENTITY.search(view):
            output.append("[REDACTED identifying log record]")
            continue
        if re.fullmatch(r"\s*[A-Za-z0-9+/=]{8,}\s*", view):
            output.append("[REDACTED opaque material]")
            continue
        line = view
        line = re.sub(r"\b(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}\b", "[REDACTED_MAC]", line)
        line = re.sub(r"\b[0-9a-fA-F]{4}(?:\.[0-9a-fA-F]{4}){2}\b", "[REDACTED_MAC]", line)
        line = re.sub(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", "[REDACTED_IP]", line)
        def ipv6(match):
            value = match.group().strip("[]")
            try:
                ipaddress.IPv6Address(value.split("%", 1)[0])
                return "[REDACTED_IP]"
            except ValueError:
                return match.group()
        line = re.sub(r"\[?(?:[0-9a-fA-F]{0,4}:){2,}[0-9a-fA-F:.%A-Za-z0-9_-]*\]?", ipv6, line)
        line = re.sub(r"([a-z][a-z0-9+.-]*://)[^/\s]+", r"\1[REDACTED_HOST]", line, flags=re.I)
        line = re.sub(r"[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}", "[REDACTED_EMAIL]", line)
        line = re.sub(r"/(?:home|homes|Users)/[^\s\"']+|/volume\d+/(?!@)[^\s\"']+",
                      "[REDACTED_PRIVATE_PATH]", line)
        output.append(OPAQUE.sub("[REDACTED_OPAQUE]", line))
    return "\n".join(output) + ("\n" if output else "")


def application_logs(data_dir):
    """Open fixed leaf names through anchored directory FDs; never follow links."""
    entries, manifest = {}, []
    descriptors = []
    try:
        for path, parent in ((str(data_dir), None), ("log", "previous")):
            options = {"dir_fd": descriptors[-1]} if parent else {}
            fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, **options)
            descriptors.append(fd)
            info = os.fstat(fd)
            if info.st_uid != 0 or info.st_mode & 0o022:
                raise ValueError("unsafe directory")
        seen = set()
        for name in LOG_NAMES:
            item = {"name": "application/" + name}
            fd = None
            try:
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=descriptors[-1])
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o022:
                    raise ValueError("unsafe log")
                identity = (info.st_dev, info.st_ino)
                if identity in seen:
                    item["status"] = "rotation_changed_duplicate_inode"
                    manifest.append(item)
                    continue
                seen.add(identity)
                offset = max(0, info.st_size - FILE_LIMIT)
                os.lseek(fd, offset, os.SEEK_SET)
                data = os.read(fd, min(info.st_size, FILE_LIMIT))
                if offset:
                    data = data.partition(b"\n")[2]
                    # A tail can begin inside a multiline secret. Resume only at
                    # a complete structured application record, never its value.
                    match = re.search(rb"(?m)^\[?\d{4}[-/]\d{2}[-/]\d{2}[T ]\d{2}:\d{2}:", data)
                    data = data[match.start():] if match else b""
                # Drop a partial trailing record rather than expose a cut credential.
                if data and not data.endswith(b"\n"):
                    data = data.rpartition(b"\n")[0]
                entries[item["name"]] = redact(data.decode("utf-8", errors="replace"))
                item.update(status="included", original_bytes=info.st_size,
                            captured_bytes=len(data), truncated=bool(offset) or len(data) < info.st_size)
            except FileNotFoundError:
                item["status"] = "not_present"
            except (OSError, ValueError):
                item["status"] = "unsafe_or_unreadable"
            finally:
                if fd is not None:
                    os.close(fd)
            manifest.append(item)
    except FileNotFoundError:
        manifest.append({"name": "application", "status": "not_present"})
    except (OSError, ValueError):
        manifest.append({"name": "application", "status": "unsafe_or_unreadable"})
    finally:
        for fd in reversed(descriptors):
            os.close(fd)
    return entries, manifest


def journal_command():
    for path in (Path("/usr/bin/journalctl"), Path("/bin/journalctl")):
        try:
            resolved = path.resolve(strict=True)
            for part in (resolved,) + tuple(resolved.parents):
                info = part.stat()
                if info.st_uid != 0 or info.st_mode & 0o022:
                    raise ValueError("untrusted journalctl")
            if not resolved.is_file() or not os.access(resolved, os.X_OK):
                continue
            args = [str(path), "--no-pager", "--output=json", "--lines=2000", "--all"]
            for unit in UNITS:
                args += ["--unit=" + unit]
            return args
        except (OSError, ValueError):
            continue
    return None


def journal_logs():
    args = journal_command()
    if args is None:
        return "[service journal unavailable: journalctl missing or untrusted]\n", {"status": "unavailable", "records": 0}
    child, selector = None, selectors.DefaultSelector()
    chunks, total, state = [], 0, "included"
    try:
        child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "SYSTEMD_COLORS": "0"},
                                 start_new_session=True)
        selector.register(child.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 5
        while selector.get_map():
            if time.monotonic() >= deadline:
                state = "timeout"
                break
            for key, _ in selector.select(min(.1, max(0, deadline - time.monotonic()))):
                chunk = os.read(key.fd, min(65536, JOURNAL_LIMIT + 1 - total))
                if not chunk:
                    selector.unregister(key.fileobj)
                    break
                chunks.append(chunk)
                total += len(chunk)
                if total > JOURNAL_LIMIT:
                    state = "truncated"
                    break
            if total > JOURNAL_LIMIT:
                break
        if state == "included" and child.wait(timeout=.5) != 0:
            state = "command_failed"
    except (OSError, subprocess.TimeoutExpired):
        state = "command_failed"
    finally:
        selector.close()
        if child is not None:
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            child.wait()
            child.stdout.close()
    raw = b"".join(chunks)[:JOURNAL_LIMIT]
    if raw and not raw.endswith(b"\n"):
        raw = raw.rpartition(b"\n")[0]
    records, skipped, output = 0, 0, []
    for line in raw.splitlines():
        try:
            item = json.loads(line)
            unit = next((item.get(key) for key in ("UNIT", "_SYSTEMD_UNIT", "COREDUMP_UNIT") if item.get(key) in UNITS), None)
            if not unit:
                continue
            if not isinstance(item.get("MESSAGE"), str):
                skipped += 1
                continue
            stamp = int(item.get("__REALTIME_TIMESTAMP", 0)) / 1_000_000
            timestamp = datetime.fromtimestamp(stamp, timezone.utc).isoformat()
            output.append(timestamp + " [" + unit + "]\n" + redact(item["MESSAGE"]))
            records += 1
        except (ValueError, TypeError, OverflowError, AttributeError):
            skipped += 1
            continue
    if state == "included":
        if skipped:
            state = "partial"
        elif not records:
            state = "no_records"
    if not output:
        output.append("[no matching service journal records available]\n")
    return "".join(output), {"status": state, "records": records, "skipped_records": skipped,
                             "units": list(UNITS), "truncated": total > JOURNAL_LIMIT}


def export_logs(data_dir, *, include_journal=True, journal_reader=None):
    entries, files = application_logs(Path(data_dir))
    reader = journal_logs if journal_reader is None else journal_reader
    if include_journal:
        content, journal = reader()
    else:
        content, journal = "[host journal is not mounted in this deployment]\n", {"status": "unavailable", "records": 0}
    entries["services/root-startup.log"] = redact(content)
    manifest = {"format": 1, "redacted": True, "created_at": datetime.now(timezone.utc).isoformat(),
                "application": files, "services": journal,
                "limits": {"application_file_tail_bytes": FILE_LIMIT, "application_rotations": 10,
                           "journal_input_bytes": JOURNAL_LIMIT, "journal_records": 2000},
                "incomplete": journal["status"] != "included" or any(
                    i["status"] not in {"included", "not_present"} or i.get("truncated", False) for i in files)}
    entries["manifest.json"] = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    entries["README.txt"] = ("UGREEN Toolbox redacted log export\n"
        "Contains available application logs, rotations and ONLY this package's service journal.\n"
        "Each application file includes at most its last 256 KiB; see manifest for missing/truncated sources.\n"
        "No settings, password records, environment files or other NAS services are included.\n"
        "Credential-bearing records/continuations, PEM material and common device/network identifiers are redacted.\n"
        "Free-form logs may contain unrecognized personal information; review before sharing.\n")
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, text in entries.items():
            archive.writestr(name, text.encode("utf-8"))
    result = buffer.getvalue()
    if len(result) > 5 * 1024 * 1024:
        raise ValueError("export exceeds bounded reply size")
    return {"ok": True, "filename": "ugreen-toolbox-logs-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + ".zip",
            "mime": "application/zip", "data_base64": base64.b64encode(result).decode(), "redacted": True,
            "incomplete": manifest["incomplete"], "sources": manifest}


class LogHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(10)
        _, uid, _ = struct.unpack("3i", self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid not in (0, self.server.web_uid):
            return
        status, body = 400, {"ok": False, "error": "invalid log request"}
        try:
            raw = self.rfile.readline(2049)
            if len(raw) > 2048 or not raw.endswith(b"\n"):
                raise ValueError("request too large")
            request = json.loads(raw)
            if request == {"method": "GET", "target": "/healthz"}:
                status, body = 200, {"ok": True}
            elif request == {"method": "POST", "target": "/logs/export", "body": ""}:
                if self.server.export_lock.acquire(blocking=False):
                    try:
                        body = export_logs(self.server.data_dir)
                        status = 200
                    finally:
                        self.server.export_lock.release()
                else:
                    status, body = 429, {"ok": False, "error": "日志正在导出，请稍后重试"}
        except (OSError, ValueError, TypeError, OverflowError):
            pass
        self.wfile.write(json.dumps({"status": status, "body": body}, ensure_ascii=False).encode() + b"\n")


class LogServer(getattr(socketserver, "ThreadingUnixStreamServer", socketserver.ThreadingTCPServer)):
    daemon_threads = True

    def __init__(self, *args):
        self.slots = threading.BoundedSemaphore(4)
        super().__init__(*args)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
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


def run_log_service():
    import fcntl
    from portable_runtime import notify_systemd_ready
    if os.geteuid() != 0:
        raise ValueError("log collector must run as root")
    path = Path(os.environ["UGREEN_LOG_SOCKET"])
    guard = os.open(path.parent / "log-export.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    info = os.fstat(guard)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o077:
        raise ValueError("unsafe log service lock")
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if path.exists():
        if not stat.S_ISSOCK(path.lstat().st_mode) or path.lstat().st_uid != 0:
            raise ValueError("unsafe log socket")
        path.unlink()
    with LogServer(str(path), LogHandler) as server:
        server.web_uid = int(os.environ["UGREEN_WEB_UID"])
        server.data_dir = Path(os.environ["VAR_DIR"])
        server.export_lock = threading.Lock()
        os.chown(path, 0, int(os.environ["UGREEN_WEB_GID"]))
        path.chmod(0o660)
        def stop(signum, frame):
            threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGTERM, stop)
        try:
            notify_systemd_ready()
            server.serve_forever(poll_interval=.2)
        finally:
            path.unlink(missing_ok=True)
            os.close(guard)
