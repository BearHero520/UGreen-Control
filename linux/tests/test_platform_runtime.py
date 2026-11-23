import base64
from contextlib import closing
import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import socket
import socketserver
import struct
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load("platform_runtime", ROOT / "app/server/portable_runtime.py")
deploy = load("platform_deploy", ROOT / "deploy/prepare.py")


class PolicyTests(unittest.TestCase):
    def app(self, **environment):
        return runtime.Application(environ=environment)

    def request(self, method="POST", target="/led/off", body="", **environment):
        app = self.app(**environment)
        app.run_cgi = Mock(return_value=(200, {"ok": True}))
        response = app.dispatch(dict(method=method, target=target, body=body))
        return response, app.run_cgi

    def test_default_denies_every_control_plane(self):
        for route in runtime.LED_ROUTES | runtime.BIOS_ROUTES:
            with self.subTest(route=route):
                (status, _), cgi = self.request(target=route)
                self.assertEqual(status, 403)
                cgi.assert_not_called()

    def test_control_planes_are_independently_authorized(self):
        for target, environment, expected in [
            ("/led/off", {"UGREEN_ALLOW_LED_CONTROL": "true"}, 200),
            ("/led/off", {"UGREEN_ALLOW_BIOS_CONTROL": "true"}, 403),
            ("/bios/startup", {"UGREEN_ALLOW_BIOS_CONTROL": "true"}, 200),
            ("/bios/startup", {"UGREEN_ALLOW_LED_CONTROL": "true"}, 403),
        ]:
            (status, _), _ = self.request(target=target, **environment)
            self.assertEqual(status, expected)

    def test_mutating_get_unknown_route_and_wrong_method_never_reach_cgi(self):
        for method, route, expected in [("GET", "/led/off", 405), ("POST", "/hardware/status", 405),
                                        ("DELETE", "/settings", 405), ("GET", "/execute", 404),
                                        ("GET", "/status%00", 400), ("GET", "//other/status", 400)]:
            (status, _), cgi = self.request(method, route)
            self.assertEqual(status, expected)
            cgi.assert_not_called()

    def test_host_lifecycle_and_driver_controls_are_not_misrepresented(self):
        for route in runtime.UNSUPPORTED_ROUTES:
            (status, _), cgi = self.request(target=route, UGREEN_ALLOW_BIOS_CONTROL="true", UGREEN_ALLOW_LED_CONTROL="true")
            self.assertEqual(status, 501)
            cgi.assert_not_called()
        (status, _), cgi = self.request(target="/bios/fan-curve?action=start", UGREEN_ALLOW_BIOS_CONTROL="true")
        self.assertEqual(status, 501)
        cgi.assert_not_called()

    def test_settings_cannot_inject_shell_syntax_spoof_model_or_bypass_bios(self):
        for body in ["bios.write_risk_acknowledgement=x", "fan_curve.enabled=true", "power_schedule.enabled=true",
                     "hardware.profile=dxp4800", "hardware.backend=sysfs", "power.brightness=$(id)",
                     "power.brightness=a[1]", "power.brightness=256", "disk_colors.active=999 0 0",
                     "mode.global=unknown", "daemon.enabled=yes", "power.brightness=1\x00"]:
            with self.subTest(body=body):
                (status, _), cgi = self.request(target="/settings", body=body, UGREEN_ALLOW_LED_CONTROL="true")
                self.assertEqual(status, 400)
                cgi.assert_not_called()
        (status, _), cgi = self.request(target="/settings", body="disk_colors.active=0 255 0\npower.brightness=40\nhardware.profile=auto", UGREEN_ALLOW_LED_CONTROL="true")
        self.assertEqual(status, 200)
        cgi.assert_called_once()

    def test_cgi_does_not_inherit_overrides_or_credentials(self):
        env = self.app(TRIM_APPDEST="/tmp/evil", UGREEN_PRODUCT_NAME="DXP4800", UGREEN_CLI_OVERRIDE="/tmp/evil",
                       LD_PRELOAD="/tmp/evil", HTTP_AUTHORIZATION="secret", VAR_DIR="/data").cgi_environment()
        for key in ("TRIM_APPDEST", "UGREEN_PRODUCT_NAME", "UGREEN_CLI_OVERRIDE", "LD_PRELOAD", "HTTP_AUTHORIZATION"):
            self.assertNotIn(key, env)
        self.assertEqual(env["VAR_DIR"], "/data")
        self.assertEqual(env["UGREEN_INTERNAL_CGI"], "1")

    def test_cgi_fixed_executable_and_status_parsing(self):
        child = Mock(returncode=0)
        child.communicate.return_value = (b'Status: 403 Forbidden\nContent-Type: application/json\n\n{"ok":false,"error":"denied"}', b"")
        with patch.object(runtime.subprocess, "Popen", return_value=child) as spawn:
            status, body = self.app().run_cgi("GET", "/status", "x=1", "")
        self.assertEqual(status, 403)
        self.assertFalse(body["ok"])
        self.assertEqual(spawn.call_args.args[0], ["/bin/bash", str(ROOT / "app/ui/api.cgi")])
        self.assertNotIn("shell", spawn.call_args.kwargs)

    def test_platform_metadata_does_not_offer_fpk_downloads(self):
        app = self.app(UGREEN_PLATFORM="synology")
        self.assertEqual(app.dispatch({"method": "GET", "target": "/about/info"})[1]["platform"], "synology")
        data = app.dispatch({"method": "GET", "target": "/update/check"})[1]
        self.assertNotIn("download_url", data)
        self.assertFalse(app.dispatch({"method": "GET", "target": "/platform/status"})[1]["led_control"])
        self.assertIn("群晖", app.dispatch({"method": "GET", "target": "/about/readme"})[1]["content"])
        native = self.app(UGREEN_PLATFORM="synology", UGREEN_DISTRIBUTION="spk")
        self.assertEqual(native.dispatch({"method": "GET", "target": "/about/info"})[1]["distribution"], "spk")
        self.assertIn("SPK", native.dispatch({"method": "GET", "target": "/update/check"})[1]["message"])

    def test_socket_rejects_wrong_uid_before_reading_or_dispatching(self):
        handler = object.__new__(runtime.BrokerHandler)
        handler.connection = Mock()
        handler.connection.getsockopt.return_value = struct.pack("3i", 100, 999, 568)
        handler.server = Mock(web_uid=568)
        handler.rfile, handler.wfile = Mock(), Mock()
        with patch.object(runtime.socket, "SO_PEERCRED", 17, create=True):
            handler.handle()
        handler.rfile.readline.assert_not_called()
        handler.server.application.dispatch.assert_not_called()

    def test_payload_is_unchanged_and_led_matches_required_canonical_bundle(self):
        runtime.verify_payload(ROOT / "app")
        self.assertEqual((ROOT / "app/server/bin/ugreen_leds_cli").read_bytes(),
                         (ROOT.parent / "App.Native.UGreenLED/app/server/bin/ugreen_leds_cli").read_bytes())

    def test_payload_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "server/lib/ugreenctl").mkdir(parents=True)
            (root / "server/bin").mkdir()
            (root / "server/bin/ugreenctl").write_bytes(b"tampered")
            (root / "server/lib/ugreenctl/build-info.json").write_text(json.dumps({"files": {"ugreenctl": hashlib.sha256(b"valid").hexdigest()}}))
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                runtime.verify_payload(root)

    def test_built_entrypoint_can_resolve_assets_without_fnos_cgi_base(self):
        html = (ROOT / "app/www/index.html").read_text(encoding="utf-8")
        self.assertNotIn("/cgi/ThirdParty/", html)
        references = re.findall(r'(?:src|href)="([^"]+)"', html)
        self.assertTrue(references)
        for reference in references:
            self.assertTrue((ROOT / "app/www" / reference.removeprefix("./").lstrip("/")).is_file(), reference)


class HTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        (cls.root / "www").mkdir()
        (cls.root / "www/index.html").write_bytes(b"fixture page")
        cls.auth_path = cls.root / "auth.json"
        cls.auth_path.write_text(json.dumps(runtime.password_record("fixture-long-password")))
        cls.auth_path.chmod(0o640)
        cls.header = "Basic " + base64.b64encode(b"admin:fixture-long-password").decode()
        cls.auth = runtime.Authentication(cls.auth_path)
        cls.server = runtime.WebServer(("127.0.0.1", 0), auth=cls.auth, broker_path="unused", root=cls.root,
                                       public_origin="https://nas.example.test")
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, method="GET", path="/api/status", *, auth=True, headers=None, body=None):
        request_headers = {"Authorization": self.header} if auth else {}
        request_headers.update(headers or {})
        with closing(http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)) as client:
            client.request(method, path, body=body, headers=request_headers)
            response = client.getresponse()
            return response.status, dict(response.getheaders()), response.read()

    def test_all_sensitive_reads_and_writes_require_authentication(self):
        with patch.object(runtime, "call_broker") as broker:
            for method, path in [("GET", "/"), ("GET", "/api/settings"), ("GET", "/api/logs"), ("POST", "/api/led/off")]:
                status, headers, _ = self.request(method, path, auth=False)
                self.assertEqual(status, 401)
                self.assertIn("WWW-Authenticate", headers)
            broker.assert_not_called()

    def test_bad_password_and_non_ascii_username_are_rejected(self):
        for raw in (b"admin:wrong-password", "测试:wrong-password".encode()):
            status, _, _ = self.request(headers={"Authorization": "Basic " + base64.b64encode(raw).decode()})
            self.assertEqual(status, 401)

    def test_only_same_origin_marked_post_is_forwarded_without_auth_header(self):
        with patch.object(runtime, "call_broker", return_value=(200, {"ok": True})) as broker:
            status, _, _ = self.request("POST", "/api/led/off?led=power", headers={"X-UGREEN-Request": "1", "Origin": "https://nas.example.test"})
            self.assertEqual(status, 200)
            self.assertEqual(broker.call_args.args[1], {"method": "POST", "target": "/led/off?led=power", "body": ""})

    def test_csrf_protection_and_no_cors_response(self):
        with patch.object(runtime, "call_broker") as broker:
            for headers in ({}, {"X-UGREEN-Request": "1", "Origin": "https://evil.test"},
                            {"X-UGREEN-Request": "1", "Sec-Fetch-Site": "cross-site"},
                            {"X-UGREEN-Request": "1", "Origin": "https://evil.test", "X-Forwarded-Host": "evil.test"}):
                status, response_headers, _ = self.request("POST", headers=headers)
                self.assertEqual(status, 403)
                self.assertNotIn("Access-Control-Allow-Origin", response_headers)
            broker.assert_not_called()

    def test_oversized_and_ambiguous_request_body_never_reaches_broker(self):
        with patch.object(runtime, "call_broker") as broker:
            status, _, _ = self.request("POST", headers={"X-UGREEN-Request": "1", "Content-Length": "65537"})
            self.assertEqual(status, 413)
            status, _, _ = self.request("POST", headers={"X-UGREEN-Request": "1", "Transfer-Encoding": "chunked"})
            self.assertEqual(status, 400)
            broker.assert_not_called()

    def test_static_files_are_confined_and_cgi_source_cannot_be_read(self):
        for path in ("/../auth.json", "/%2e%2e/auth.json", "/server/portable_runtime.py", "/ui/api.cgi", "/www/"):
            self.assertEqual(self.request(path=path)[0], 404)
        status, headers, body = self.request(path="/")
        self.assertEqual((status, body), (200, b"fixture page"))
        self.assertEqual(headers["X-Frame-Options"], "DENY")

    def test_broker_unavailable_is_explicit_and_health_is_non_sensitive(self):
        with patch.object(runtime, "call_broker", side_effect=OSError("permission denied")):
            self.assertEqual(self.request()[0], 503)
            self.assertEqual(self.request(path="/healthz", auth=False)[0], 503)
        with patch.object(runtime, "call_broker", return_value=(200, {"ok": True})):
            status, _, body = self.request(path="/healthz", auth=False)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"ok": True})


@unittest.skipUnless(hasattr(socket, "SO_PEERCRED"), "requires Linux Unix socket peer credentials")
class UnixSocketTests(unittest.TestCase):
    def test_real_socket_roundtrip_with_application_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "hardware.sock")
            with socketserver.UnixStreamServer(path, runtime.BrokerHandler) as server:
                server.application = runtime.Application(environ={})
                server.web_uid = os.getuid()
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    self.assertEqual(runtime.call_broker(path, {"method": "GET", "target": "/platform/status"})[0], 200)
                    self.assertEqual(runtime.call_broker(path, {"method": "POST", "target": "/led/off"})[0], 403)
                finally:
                    server.shutdown()
                    thread.join()


class DeploymentTests(unittest.TestCase):
    def document(self, platform_name="truenas", **kwargs):
        return deploy.compose_document(Path("/mnt/tank/apps/ugreen"), platform_name=platform_name, **kwargs)

    def test_both_platforms_have_no_privileged_web_hardware_or_host_control_socket(self):
        for platform_name in ("synology", "truenas"):
            services = self.document(platform_name)["services"]
            web, broker = services["web"], services["hardware"]
            self.assertEqual(web["user"], "568:568")
            self.assertEqual(web["cap_drop"], ["ALL"])
            self.assertEqual({m["target"] for m in web["volumes"]}, {"/ipc", "/auth"})
            self.assertTrue(all(m["read_only"] for m in web["volumes"]))
            self.assertEqual(broker["network_mode"], "none")
            self.assertEqual(broker["cap_add"], ["CHOWN"])
            self.assertNotIn("ports", broker)
            self.assertNotIn("devices", broker)
            self.assertNotIn("privileged", broker)
            self.assertNotIn("/var/run/docker.sock", json.dumps(services))
            for name in runtime.LOCK_NAMES:
                self.assertIn(deploy.bind("/run/" + name, "/run/" + name, False), broker["volumes"])
            self.assertEqual(web["ports"], ["127.0.0.1:5088:5088"])

    def test_raw_io_is_explicit_does_not_relax_seccomp_or_apparmor_and_never_reaches_web(self):
        services = self.document(led=True, raw_io=True, devices=["/dev/i2c-2"])["services"]
        self.assertIn("SYS_RAWIO", services["hardware"]["cap_add"])
        self.assertEqual(services["hardware"]["security_opt"], ["no-new-privileges:true"])
        self.assertEqual(services["hardware"]["devices"], ["/dev/i2c-2:/dev/i2c-2:rw"])
        self.assertNotIn("cap_add", services["web"])

    def test_uid_full_disk_passthrough_and_non_tls_public_binding_are_rejected(self):
        for kwargs in ({"uid": 0}, {"gid": 0}, {"raw_io": True},
                       {"led": True, "devices": ["/dev/sda"]},
                       {"led": True, "devices": ["/dev"]}, {"bind_address": "0.0.0.0"},
                       {"public_origin": "http://nas.test"}, {"public_origin": "https://nas.test/path"}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.document(**kwargs)

    def test_hwmon_write_access_is_limited_to_explicit_real_device_directory(self):
        path = "/sys/devices/platform/it87.656/hwmon/hwmon3"
        broker = self.document(bios=True, hwmon=[path])["services"]["hardware"]
        self.assertIn(deploy.bind(path, path, False), broker["volumes"])
        for value in ("/sys", "/sys/class/hwmon", "/sys/devices/../class/hwmon/hwmon0"):
            with self.assertRaises(ValueError):
                self.document(bios=True, hwmon=[value])

    def test_boot_command_has_no_storage_pool_dependency(self):
        command = deploy.boot_command()
        self.assertTrue(command.startswith("python3 -c "))
        self.assertNotIn("/mnt/", command)
        self.assertNotIn("/volume1/", command)
        self.assertIn("O_NOFOLLOW", command)

    def test_preparation_refuses_nonempty_unrelated_data_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            deploy.check_deployment_directory(path)
            (path / "user-data.txt").write_bytes(b"preserve")
            with self.assertRaisesRegex(ValueError, "非空"):
                deploy.check_deployment_directory(path)
            self.assertEqual((path / "user-data.txt").read_bytes(), b"preserve")
            (path / ".llled-deployment.json").write_text(json.dumps({"application": "ugreen-nas-tool", "schema": 1}))
            deploy.check_deployment_directory(path)


if __name__ == "__main__":
    unittest.main()
