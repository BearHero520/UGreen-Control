import base64
from contextlib import closing
import http.client
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app/server"))
import portable_runtime as runtime
spec = importlib.util.spec_from_file_location("native_desktop", ROOT / "synology/native_desktop.py")
desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop)


class DesktopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        (cls.root / "www").mkdir()
        for name in ("native-login.html", "native-login.js", "native-login.css"):
            (cls.root / "www" / name).write_bytes((ROOT / "synology/ui" / name).read_bytes())
        (cls.root / "www/index.html").write_bytes(b"authenticated application")
        path = cls.root / "auth.json"
        path.write_text(json.dumps(runtime.password_record("native-desktop-password")))
        path.chmod(0o640)
        cls.auth = "Basic " + base64.b64encode(b"admin:native-desktop-password").decode()
        cls.server = desktop.DesktopWebServer(("127.0.0.1", 0), auth=runtime.Authentication(path), broker_path="unused", root=cls.root)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, path="/session", method="POST", **headers):
        request_headers = {"Host": "nas.test:5001", "Origin": "https://nas.test:5001",
                           "X-UGREEN-Request": "1", "X-UGREEN-DSM-Proxy": "1", "X-Forwarded-Proto": "https"}
        request_headers.update(headers)
        with closing(http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)) as client:
            client.request(method, path, headers=request_headers)
            response = client.getresponse()
            return response.status, dict(response.getheaders()), response.read()

    def login(self):
        status, headers, _ = self.request(Authorization=self.auth)
        self.assertEqual(status, 200)
        return headers["Set-Cookie"]

    def test_log_export_requires_login_same_origin_empty_post_and_independent_socket(self):
        cookie = self.login()
        with patch.object(runtime, 'call_broker', return_value=(200, {'ok': True})) as broker:
            self.assertEqual(self.request('/api/logs/export', 'POST')[0], 401)
            self.assertEqual(self.request('/api/logs/export', 'POST', Cookie=cookie, Origin='https://evil.test')[0], 403)
            for target, method in (('/api/logs/export', 'GET'), ('/api/logs/export?path=/etc/shadow', 'POST')):
                self.assertEqual(self.request(target, method, Cookie=cookie)[0], 400)
            broker.assert_not_called()
            self.assertEqual(self.request('/api/logs/export', 'POST', Cookie=cookie)[0], 200)
            self.assertEqual(broker.call_args.args[0], '/run/ugreen-nas-native/ipc/logs.sock')
            self.assertEqual(broker.call_args.args[1], {'method': 'POST', 'target': '/logs/export', 'body': ''})

    def test_iframe_login_and_sensitive_api_authentication(self):
        status, headers, body = self.request("/", "GET")
        self.assertEqual(status, 200)
        self.assertIn(b'login-form', body)
        self.assertEqual(headers["X-Frame-Options"], "SAMEORIGIN")
        self.assertIn("frame-ancestors 'self'", headers["Content-Security-Policy"])
        with patch.object(runtime, "call_broker") as broker:
            status, headers, _ = self.request("/api/settings", "GET")
            self.assertEqual(status, 401)
            self.assertEqual(headers["X-UGREEN-Login"], "required")
            self.assertNotIn("WWW-Authenticate", headers)
            broker.assert_not_called()
        self.assertEqual(self.request("/", "GET", Cookie=self.login())[2], b"authenticated application")

    def test_cookie_is_secure_scoped_and_revocable(self):
        cookie = self.login()
        for field in ("Secure", "HttpOnly", "SameSite=Strict", "Path=/ugreen-toolbox/", "Max-Age=3600"):
            self.assertIn(field, cookie)
        with patch.object(runtime, "call_broker", return_value=(200, {"ok": True})):
            self.assertEqual(self.request("/api/settings", "GET", Cookie=cookie)[0], 200)
        self.assertEqual(self.request("/session/logout", Cookie=cookie)[0], 200)
        self.assertEqual(self.request("/api/settings", "GET", Cookie=cookie)[0], 401)

    def test_missing_origin_forged_forwarding_and_cross_site_are_rejected(self):
        cookie = self.login()
        for headers in ({"Origin": ""}, {"Origin": "https://evil.test"}, {"X-UGREEN-Request": "0"},
                        {"X-Forwarded-Proto": "http"}, {"X-UGREEN-DSM-Proxy": "0"},
                        {"Sec-Fetch-Site": "same-site"}, {"Origin": "https://evil.test", "X-Forwarded-Host": "evil.test"}):
            with self.subTest(headers=headers), patch.object(runtime, "call_broker") as broker:
                self.assertEqual(self.request(Authorization=self.auth, **headers)[0], 403)
                self.assertEqual(self.request("/api/led/off", Cookie=cookie, **headers)[0], 403)
                broker.assert_not_called()

    def test_expired_forged_and_pre_restart_sessions_cannot_authenticate(self):
        cookie = self.login()
        with patch.object(desktop.time, "monotonic", return_value=10**12):
            self.assertEqual(self.request("/api/settings", "GET", Cookie=cookie)[0], 401)
        self.assertEqual(self.request("/api/settings", "GET", Cookie="ugreen_toolbox_session=forged")[0], 401)
        self.assertFalse(desktop.Sessions().verify(cookie))

    def test_bad_credentials_and_login_protocol_never_reach_hardware(self):
        with patch.object(runtime, "call_broker") as broker:
            self.assertEqual(self.request(Authorization="Basic YWRtaW46YmFk")[0], 401)
            self.assertEqual(self.request("/session", "GET")[0], 405)
            self.assertEqual(self.request(**{"Content-Length": "4"})[0], 400)
            self.assertEqual(self.request(**{"Transfer-Encoding": "chunked"})[0], 400)
            broker.assert_not_called()


if __name__ == "__main__":
    unittest.main()
