#!/usr/bin/env python3
"""Real Docker integration checks. Run only on a disposable Linux Docker host.

Creates canonical application lock files on that host; never maps control devices
or authorizes hardware writes. Platform labels use explicit OS metadata fixtures.
"""
from contextlib import closing, redirect_stdout
import argparse
import base64
import fcntl
import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("deployment", ROOT / "deploy/prepare.py")
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)
PASSWORD = "disposable-docker-smoke-password"


def run(*args, check=True, timeout=180):
    process = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=timeout)
    if check and process.returncode:
        raise RuntimeError(f"command failed ({process.returncode}): {args[:3]}\n{process.stdout[-6000:]}")
    return process


class Smoke:
    def __init__(self, directory, image, profile, uid, gid):
        self.directory, self.image, self.profile, self.uid, self.gid = directory, image, profile, uid, gid
        self.project = "llled-smoke-" + profile
        self.file = directory / "compose.yaml"
        self.checks = {}

    def record(self, name, detail=True):
        self.checks[name] = detail
        print(f"[{self.profile}] PASS {name}", flush=True)

    def compose(self, *args, **kwargs):
        return run("docker", "compose", "-p", self.project, "-f", str(self.file), *args, **kwargs)

    def container(self, role):
        return self.compose("ps", "-a", "-q", role).stdout.strip()

    def inspect(self, role):
        return json.loads(run("docker", "inspect", self.container(role)).stdout)[0]

    def python(self, role, code, user=None):
        args = ["docker", "exec"]
        if user:
            args += ["--user", user]
        return run(*args, self.container(role), "python3", "-c", code)

    def http(self, target="/api/platform/status", method="GET", auth=True, headers=None):
        request_headers = {"Authorization": "Basic " + base64.b64encode(("admin:" + PASSWORD).encode()).decode()} if auth else {}
        request_headers.update(headers or {})
        with closing(http.client.HTTPConnection("127.0.0.1", 5088, timeout=8)) as client:
            client.request(method, target, headers=request_headers)
            response = client.getresponse()
            return response.status, response.read()

    def ready(self, timeout=100):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if self.http()[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.5)
        raise RuntimeError(self.compose("logs", "--tail", "35").stdout)

    def wait_log(self, role, fragment, timeout=30):
        deadline = time.monotonic() + timeout
        since = self.inspect(role)["State"]["StartedAt"]
        while time.monotonic() < deadline:
            output = self.compose("logs", "--since", since, "--tail", "50", role).stdout
            if fragment in output:
                return
            time.sleep(0.5)
        raise AssertionError(output)

    def prepare(self):
        arguments = ["prepare.py", "--platform", self.profile, "--directory", str(self.directory),
                     "--image", self.image, "--web-uid", str(self.uid), "--web-gid", str(self.gid)]
        os_fixture = 'PRETTY_NAME="Synology DSM fixture"\n' if self.profile == "synology" else 'PRETTY_NAME="TrueNAS Linux fixture"\n'
        with patch.object(sys, "argv", arguments), patch.object(deployment, "host_os_release", return_value=os_fixture), \
                patch.object(deployment.getpass, "getpass", return_value=PASSWORD), redirect_stdout(io.StringIO()):
            deployment.main()

    def verify(self):
        self.ready()
        web, broker = self.inspect("web"), self.inspect("hardware")
        assert web["Config"]["User"] == f"{self.uid}:{self.gid}"
        assert web["HostConfig"]["CapDrop"] == ["ALL"]
        assert web["HostConfig"]["ReadonlyRootfs"]
        assert broker["HostConfig"]["NetworkMode"] == "none"
        assert not broker["HostConfig"]["Privileged"]
        assert [value.removeprefix("CAP_") for value in broker["HostConfig"]["CapAdd"]] == ["CHOWN"]
        assert not broker["HostConfig"]["Devices"]
        assert not broker["NetworkSettings"]["Ports"]
        assert {m["Destination"] for m in web["Mounts"]} == {"/auth", "/ipc"}
        assert all(not m["RW"] for m in web["Mounts"])
        assert all(not m["RW"] for m in broker["Mounts"] if m["Destination"] in {"/proc", "/sys", "/host/os-release", "/host/hostname"})
        identity = json.loads(self.python("web", "import json,os; print(json.dumps({'uid':os.getuid(),'gid':os.getgid(),'status':open('/proc/self/status').read()}))").stdout)
        assert identity["uid"] == self.uid and identity["gid"] == self.gid
        status = dict(line.split(":", 1) for line in identity["status"].splitlines() if ":" in line)
        assert int(status["CapEff"].strip(), 16) == 0
        assert status["NoNewPrivs"].strip() == "1" and status["Seccomp"].strip() == "2"
        broker_status = self.python("hardware", "print(open('/proc/self/status').read())").stdout
        broker_status = dict(line.split(":", 1) for line in broker_status.splitlines() if ":" in line)
        assert int(broker_status["CapEff"].strip(), 16) == 1
        self.record("unprivileged_web_and_offline_broker", {"uid": self.uid, "gid": self.gid, "web_cap_eff": 0, "seccomp": 2})
        for target in ("/", "/api/settings", "/api/logs"):
            assert self.http(target, auth=False)[0] == 401
        for target in ("/api/settings", "/api/hardware/status", "/api/about/info"):
            response, raw = self.http(target)
            assert response == 200, (target, response, raw)
            assert isinstance(json.loads(raw), dict)
        for target in ("/api/led/off", "/api/bios/fan?pwm=255&channel=all"):
            assert self.http(target, method="POST", headers={"X-UGREEN-Request": "1"})[0] == 403
        assert self.http("/api/logs/clear?confirm=clear-logs", method="POST", headers={"X-UGREEN-Request": "1", "Origin": "https://wrong.test"})[0] == 403
        assert self.http("/api/platform/status")[1]
        self.record("authenticated_real_cgi_and_default_write_denial")
        self.python('hardware', """import os
from pathlib import Path
directory=Path(os.environ.get('VAR_DIR','/var/lib/ugreen-nas'))/'log'
directory.mkdir(mode=0o700,exist_ok=True)
for name in ('app.log','app.log.1','app.log.5'):
 (directory/name).write_text('[2026-10-30T08:00:00+0000] password=docker-export-CANARY\\n[2026-10-30T08:00:01+0000] export fixture='+name+'\\n')
""")
        assert self.http('/api/logs/export', method='POST', auth=False)[0] == 401
        status, raw = self.http('/api/logs/export', method='POST', headers={'X-UGREEN-Request':'1'})
        assert status == 200, raw[:300]
        result = json.loads(raw)
        assert result['redacted'] and result['incomplete']
        assert result['sources']['services']['status'] == 'unavailable'
        with zipfile.ZipFile(io.BytesIO(base64.b64decode(result['data_base64']))) as archive:
            assert all('application/'+name in archive.namelist() for name in ('app.log','app.log.1','app.log.5'))
            assert 'CANARY' not in '\n'.join(archive.read(name).decode() for name in archive.namelist())
        self.record('redacted_log_export_includes_rotations_and_explicit_host_journal_unavailable')
        denial = self.python("web", """import os,json
paths=['/auth/forbidden','/ipc/forbidden','/opt/ugreen/forbidden','/var/lib/ugreen-nas/forbidden']
denied=[]
for path in paths:
 try: fd=os.open(path,os.O_CREAT|os.O_WRONLY,0o600); os.close(fd)
 except OSError: denied.append(path)
assert denied==paths
assert not os.path.exists('/var/run/docker.sock')
assert not os.path.exists('/dev/port')
print(json.dumps(denied))
""")
        self.record("web_cannot_write_auth_ipc_code_or_hardware_data", json.loads(denial.stdout))
        remote = json.loads(self.python("hardware", "import os,json; names=" + repr(deployment.LOCK_NAMES) + "; print(json.dumps({n:[os.stat('/run/'+n).st_dev,os.stat('/run/'+n).st_ino] for n in names}))").stdout)
        for name in deployment.LOCK_NAMES:
            info = os.stat("/run/" + name)
            assert remote[name] == [info.st_dev, info.st_ino]
        with open("/run/llled-hardware.lock", "r+") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                raise AssertionError("broker lock not held on host inode")
            except BlockingIOError:
                pass
        for name in deployment.LOCK_NAMES[1:]:
            with open("/run/" + name, "r+") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                code = "import fcntl; f=open('/run/" + name + "','r+'); denied=False\ntry: fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)\nexcept BlockingIOError: denied=True\nassert denied"
                self.python("hardware", code)
        self.record("all_locks_share_host_inode_and_flock", remote)
        foreign = self.python("hardware", """import sys
sys.path.insert(0,'/opt/ugreen/app/server')
from portable_runtime import call_broker
denied=False
try: call_broker('/ipc/hardware.sock', {'method':'GET','target':'/platform/status'})
except (OSError,ValueError): denied=True
assert denied
""", user=f"{self.uid+1}:{self.gid}")
        self.record("same_group_wrong_uid_rejected_by_peer_credentials")

    def exercise(self):
        self.prepare()
        sentinel = self.directory / "data/persistence-fixture.txt"
        sentinel.write_text("preserve across recreation and reboot\n")
        self.compose("up", "-d", "--wait", "--wait-timeout", "160", timeout=200)
        self.verify()
        root_web = run("docker", "run", "--rm", "--network", "none", "--cap-drop", "ALL", "--user", "0:0", self.image, "web", check=False)
        assert root_web.returncode and "普通用户" in root_web.stdout
        self.record("root_web_startup_refused")
        duplicate = self.compose("run", "--rm", "--no-deps", "hardware", check=False)
        assert duplicate.returncode and "BlockingIOError" in duplicate.stdout, duplicate.stdout
        self.ready()
        self.record("second_broker_refused_without_disturbing_first")
        auth_path = self.directory / "auth/auth.json"
        auth_hash = hashlib.sha256(auth_path.read_bytes()).hexdigest()
        lock_inodes = {n: os.stat("/run/" + n).st_ino for n in deployment.LOCK_NAMES}
        self.compose("restart", "hardware")
        self.ready()
        self.record("graceful_broker_restart_reconnects_existing_web")
        count = self.inspect("hardware")["RestartCount"]
        # Kill the disposable container's host PID, not the Docker API: Docker
        # treats `docker kill` as an administrative stop and suppresses restart.
        os.kill(self.inspect("hardware")["State"]["Pid"], signal.SIGKILL)
        self.ready()
        assert self.inspect("hardware")["RestartCount"] > count
        assert lock_inodes == {n: os.stat("/run/" + n).st_ino for n in deployment.LOCK_NAMES}
        self.record("sigkill_restart_replaces_stale_socket_preserves_lock_inodes")
        os.chown(auth_path, 0, self.gid + 1)
        self.compose("restart", "web")
        self.wait_log("web", "PermissionError")
        self.prepare()
        self.compose("restart", "web")
        self.ready()
        assert hashlib.sha256(auth_path.read_bytes()).hexdigest() == auth_hash
        self.record("wrong_auth_group_fails_closed_and_scoped_prepare_repairs_it")
        run("setfacl", "-m", f"u:{self.uid}:---", str(auth_path))
        try:
            self.prepare()
            raise AssertionError("restrictive auth ACL was not detected")
        except ValueError as exc:
            assert "ACL" in str(exc)
        self.compose("restart", "web")
        self.wait_log("web", "PermissionError")
        run("setfacl", "-b", str(auth_path))
        self.prepare()
        self.compose("restart", "web")
        self.ready()
        self.record("posix_acl_denial_detected_without_widening_or_erasing_acl")
        ipc = self.directory / "ipc"
        run("setfacl", "-m", f"u:{self.uid}:---", str(ipc))
        assert self.http()[0] == 503
        assert self.http("/healthz", auth=False)[0] == 503
        run("setfacl", "-b", str(ipc))
        self.prepare()
        self.ready()
        self.record("socket_acl_failure_marks_web_unhealthy_and_recovers_without_recreation")
        self.compose("down", "--timeout", "95")
        lock_path = Path("/run/llled-hardware.lock")
        assert lock_path.is_file() and not lock_path.is_symlink()
        lock_path.unlink()
        absent = self.compose("up", "-d", check=False)
        assert absent.returncode, absent.stdout
        self.record("missing_boot_lock_prevents_unsafe_private_lock_fallback")
        run("/bin/sh", "-c", deployment.boot_command())
        self.compose("up", "-d", "--wait", "--wait-timeout", "160", timeout=200)
        self.ready()
        assert sentinel.read_text() == "preserve across recreation and reboot\n"
        assert hashlib.sha256(auth_path.read_bytes()).hexdigest() == auth_hash
        self.record("boot_lock_preparation_and_recreation_preserve_auth_and_data")
        return {"platform_metadata": "fixture on disposable Linux, not NAS validation", "checks": self.checks,
                "directory": str(self.directory), "project": self.project, "auth_sha256": auth_hash}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--isolated-host", action="store_true", help="确认这是可创建宿主锁文件的临时测试主机")
    parser.add_argument("--image", default=deployment.IMAGE)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if not args.isolated_host or os.geteuid() != 0:
        parser.error("只能在一次性 Linux Docker 主机上以 root 使用 --isolated-host")
    args.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    marker = args.directory / ".docker-smoke-owner"
    if not marker.exists() and any(Path("/run", name).exists() for name in deployment.LOCK_NAMES):
        parser.error("宿主已有应用控制锁，拒绝干扰其他安装；请使用干净的测试机")
    marker.write_text("llled disposable docker validation\n")
    report = {"host": platform.platform(), "docker": json.loads(run("docker", "version", "--format", "{{json .}}").stdout),
              "compose": run("docker", "compose", "version", "--short").stdout.strip(),
              "security_options": json.loads(run("docker", "info", "--format", "{{json .SecurityOptions}}").stdout),
              "image_id": json.loads(run("docker", "image", "inspect", args.image).stdout)[0]["Id"], "profiles": {}}
    try:
        for name, uid, gid in (("synology", 1030, 65539), ("truenas", 568, 568)):
            smoke = Smoke(args.directory / name, args.image, name, uid, gid)
            report["profiles"][name] = smoke.exercise()
            if name == "synology":
                smoke.compose("down", "--timeout", "95")
    finally:
        (args.directory / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print("Docker profile smoke checks passed; TrueNAS fixture left running for cold reboot validation.")


if __name__ == "__main__":
    main()
