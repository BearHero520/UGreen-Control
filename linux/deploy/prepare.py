#!/usr/bin/env python3
"""Prepare a local, least-privilege Compose deployment on the NAS (no hardware I/O)."""
from __future__ import annotations

import argparse
import getpass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import shlex
import stat
import sys
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app/server"))
from portable_runtime import LOCK_NAMES, password_record

IMAGE = "llled-platform:3.0.1-platform.1"


def deployment_id(directory: Path):
    return hashlib.sha256(str(directory).encode()).hexdigest()[:24]


def bind(source, target, readonly=True):
    return {"type": "bind", "source": str(source), "target": target,
            "read_only": readonly, "bind": {"create_host_path": False}}


def compose_document(directory: Path, *, platform_name: str, uid=568, gid=568,
                     led=False, bios=False, raw_io=False, devices=(), hwmon=(),
                     bind_address="127.0.0.1", public_origin="", image=IMAGE, apparmor_profile=""):
    if uid <= 0 or gid <= 0:
        raise ValueError("Web UID/GID 必须大于 0")
    address = ipaddress.IPv4Address(bind_address)
    if public_origin:
        origin = urlsplit(public_origin)
        if origin.scheme != "https" or not origin.hostname or origin.path or origin.query or origin.fragment or origin.username:
            raise ValueError("外部地址必须为不带路径的 HTTPS origin，如 https://nas.example.com")
    if not address.is_loopback and not public_origin:
        raise ValueError("绑定非回环地址时必须提供 --public-origin https://... 并配置 HTTPS 反向代理")
    if platform_name not in {"synology", "truenas"}:
        raise ValueError("unsupported platform")
    if raw_io and not (led or bios):
        raise ValueError("只有授权硬件控制后才可添加 SYS_RAWIO")
    for device in devices:
        if not re.fullmatch(r"/dev/(?:i2c-\d+|gpiochip\d+|port|sio|ugreen[^/]*|leds?[^/]*)", str(device)):
            raise ValueError("只允许明确指定控制设备，不能映射磁盘或整个 /dev")
    for path in hwmon:
        if not re.fullmatch(r"/sys/devices/[A-Za-z0-9_./:+-]+/hwmon/hwmon\d+", str(path)) or ".." in str(path):
            raise ValueError("hwmon 必须使用 /sys/devices/.../hwmon/hwmonN 的真实路径")
        if not bios:
            raise ValueError("可写 hwmon 挂载需要 --bios-control")
    if devices and not (led or bios):
        raise ValueError("设备映射需要先授权硬件控制")
    mounts = [bind(directory / "data", "/var/lib/ugreen-nas", False),
              bind(directory / "ipc", "/ipc", False), bind("/sys", "/sys"),
              bind("/proc", "/proc"), bind(directory / "host-os-release", "/host/os-release"),
              bind(directory / "host-name", "/host/hostname")]
    mounts.extend(bind("/run/" + name, "/run/" + name, False) for name in LOCK_NAMES)
    mounts.extend(bind(path, str(path), False) for path in hwmon)
    security = ["no-new-privileges:true"]
    if apparmor_profile:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", apparmor_profile) or not (led or bios):
            raise ValueError("AppArmor 配置必须为已加载的专用 profile 名称，且须授权硬件控制")
        security.append("apparmor=" + apparmor_profile)
    broker = {
        "labels": {"io.bearhero.llled.deployment": deployment_id(directory), "io.bearhero.llled.role": "hardware"},
        "image": image, "platform": "linux/amd64", "user": "0:0", "init": True,
        "command": ["broker"], "restart": "unless-stopped", "network_mode": "none", "stop_grace_period": "90s",
        "read_only": True, "cap_drop": ["ALL"],
        "cap_add": ["CHOWN"] + (["SYS_RAWIO"] if raw_io else []),
        "security_opt": security, "pids_limit": 128,
        "tmpfs": ["/tmp:rw,nosuid,nodev,noexec,mode=1777", "/run:rw,nosuid,nodev,noexec,mode=0755"],
        "environment": {
            "UGREEN_PLATFORM": platform_name, "UGREEN_WEB_UID": str(uid), "UGREEN_WEB_GID": str(gid),
            "UGREEN_ALLOW_LED_CONTROL": str(led).lower(), "UGREEN_ALLOW_BIOS_CONTROL": str(bios).lower(),
            "VAR_DIR": "/var/lib/ugreen-nas", "UGREEN_RUNTIME_DIR": "/run/ugreen-nas",
            "SYSTEM_INFO_PROC_ROOT": "/proc", "SYSTEM_INFO_HOSTNAME_FILE": "/host/hostname",
            "SYSTEM_INFO_OS_RELEASE": "/host/os-release", "UGREEN_NETWORK_MODE": "carrier",
            "UGREEN_DISK_MODE": "stats",
        },
        "volumes": mounts,
        "healthcheck": {"test": ["CMD", "python3", "/opt/ugreen/app/server/portable_runtime.py", "healthcheck"],
                        "interval": "30s", "timeout": "75s", "start_period": "20s", "retries": 3},
        "logging": {"driver": "json-file", "options": {"max-size": "5m", "max-file": "3"}},
    }
    if devices:
        broker["devices"] = [f"{device}:{device}:rw" for device in devices]
    web = {
        "labels": {"io.bearhero.llled.deployment": deployment_id(directory), "io.bearhero.llled.role": "web"},
        "image": image, "platform": "linux/amd64", "user": f"{uid}:{gid}", "init": True,
        "command": ["web"], "restart": "unless-stopped", "read_only": True,
        "cap_drop": ["ALL"], "security_opt": ["no-new-privileges:true"], "pids_limit": 64,
        "ports": [f"{bind_address}:5088:5088"], "tmpfs": ["/tmp:rw,nosuid,nodev,noexec,mode=1777"],
        "environment": {"UGREEN_BIND": "0.0.0.0", "UGREEN_PUBLIC_ORIGIN": public_origin,
                        "UGREEN_AUTH_FILE": "/auth/auth.json"},
        "volumes": [bind(directory / "ipc", "/ipc"), bind(directory / "auth", "/auth")],
        "depends_on": {"hardware": {"condition": "service_healthy"}},
        "healthcheck": {
            "test": ["CMD", "python3", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:5088/healthz', timeout=10).close()"],
            "interval": "30s", "timeout": "15s", "start_period": "30s", "retries": 3,
        },
        "logging": {"driver": "json-file", "options": {"max-size": "5m", "max-file": "3"}},
    }
    return {"services": {"hardware": broker, "web": web}}


def secure_directory(path: Path, mode: int, gid=0):
    if path.is_symlink():
        raise ValueError(f"拒绝符号链接目录：{path}")
    path.mkdir(mode=mode, exist_ok=True)
    info = path.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0:
        raise ValueError(f"目录必须归 root 所有；请使用专用目录而非共享数据集：{path}")
    os.chown(path, 0, gid)
    path.chmod(mode)


def check_deployment_directory(path: Path):
    marker = path / ".llled-deployment.json"
    if not path.exists():
        return
    if not path.is_dir() or path.is_symlink():
        raise ValueError("部署目标必须为普通专用目录")
    if marker.is_symlink():
        raise ValueError("部署标记不能为链接")
    if marker.exists():
        if (not marker.is_file() or marker.stat().st_nlink != 1 or
                json.loads(marker.read_text(encoding="utf-8")) != {"application": "ugreen-nas-tool", "schema": 1}):
            raise ValueError("不是本应用创建的部署目录")
    elif any(path.iterdir()):
        raise ValueError("拒绝修改已有非空目录；请选择新建的专用应用目录")


def verify_web_permissions(directory: Path, uid: int, gid: int):
    """Probe the real UID/GID and ACL using directory FDs, matching bind mounts.

    The private deployment parent remains 0700; it is intentionally not traversed
    by the Web process inside its separate auth/ipc bind mounts.
    """
    auth = os.open(directory / "auth", os.O_RDONLY | os.O_DIRECTORY)
    ipc = os.open(directory / "ipc", os.O_RDONLY | os.O_DIRECTORY)
    try:
        pid = os.fork()
        if pid == 0:
            try:
                os.setgroups([])
                os.setgid(gid)
                os.setuid(uid)
                os.close(os.open("auth.json", os.O_RDONLY | os.O_NOFOLLOW, dir_fd=auth))
                os.close(os.open(".", os.O_RDONLY | os.O_DIRECTORY, dir_fd=ipc))
            except OSError:
                os._exit(1)
            os._exit(0)
        _, status = os.waitpid(pid, 0)
        if status != 0:
            raise ValueError(f"ACL 或权限阻止 Web {uid}:{gid} 读取 auth 或访问 ipc；请只修正本应用目录的 ACL，不能使用 777")
    finally:
        os.close(auth)
        os.close(ipc)


def prepare_lock(path: Path):
    # Opening a lock file is filesystem preparation, not device/register access.
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o077:
            raise ValueError(f"锁文件权限不安全，请先检查其来源：{path}")
    finally:
        os.close(fd)


def boot_command():
    # Inline code has no dependency on storage pools being mounted or Python packages.
    code = "import os,stat\nassert os.geteuid()==0\n"
    code += "for name in " + repr(LOCK_NAMES) + ":\n"
    code += " fd=os.open('/run/'+name,os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)\n"
    code += " s=os.fstat(fd)\n assert stat.S_ISREG(s.st_mode) and s.st_uid==0 and s.st_nlink==1 and not s.st_mode&0o077\n os.close(fd)\n"
    return "python3 -c " + shlex.quote(code)


def host_os_release(platform_name):
    if platform_name == "synology":
        version_file = Path("/etc.defaults/VERSION")
        version = dict(re.findall(r'^([a-z_]+)="([^"\r\n]*)"', version_file.read_text(), re.M))
        pretty = "Synology DSM " + ".".join(version.get(k, "?") for k in ("majorversion", "minorversion"))
    else:
        source = Path("/etc/os-release").read_text()
        pretty = next((line.split("=", 1)[1].strip('"') for line in source.splitlines() if line.startswith("PRETTY_NAME=")), "TrueNAS Linux")
        if "truenas" not in (source + Path("/etc/version").read_text()).lower():
            raise ValueError("没有检测到 TrueNAS；拒绝生成误标为 TrueNAS 的部署")
    return f"PRETTY_NAME={json.dumps(pretty)}\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--print-boot-command", action="store_true")
    parser.add_argument("--platform", choices=["synology", "truenas"])
    parser.add_argument("--directory", type=Path, help="新建的专用应用目录，不能使用共享根目录")
    parser.add_argument("--web-uid", type=int, default=568)
    parser.add_argument("--web-gid", type=int, default=568)
    parser.add_argument("--led-control", action="store_true")
    parser.add_argument("--bios-control", action="store_true")
    parser.add_argument("--raw-io", action="store_true", help="仅对硬件容器授权 SYS_RAWIO；不会启动任何控制")
    parser.add_argument("--apparmor-profile", default="", help="可选：已加载的专用 AppArmor profile；保持默认时不放宽策略")
    parser.add_argument("--device", action="append", default=[])
    parser.add_argument("--hwmon-path", type=Path, action="append", default=[])
    parser.add_argument("--bind-address", default="127.0.0.1")
    parser.add_argument("--public-origin", default="")
    parser.add_argument("--image", default=IMAGE)
    parser.add_argument("--reset-password", action="store_true")
    args = parser.parse_args()
    if args.print_boot_command:
        print(boot_command())
        return
    if not args.platform or args.directory is None:
        parser.error("需要 --platform 和 --directory")
    if sys.platform != "linux" or os.geteuid() != 0 or platform.machine() not in {"x86_64", "amd64"}:
        parser.error("请在 x86_64 NAS 宿主机上以 root 执行；不支持 TrueNAS CORE / ARM")
    original = args.directory.absolute()
    directory = original.resolve()
    if original != directory or directory.parent in {Path("/"), Path("/mnt"), Path("/volume1")}:
        parser.error("必须指定存储卷下新建的专用应用子目录，不能使用符号链接或卷根目录")
    if not directory.parent.is_dir():
        parser.error("应用目录的父目录必须已存在")
    check_deployment_directory(directory)
    hwmon = [path.resolve(strict=True) for path in args.hwmon_path]
    document = compose_document(directory, platform_name=args.platform, uid=args.web_uid, gid=args.web_gid,
                                led=args.led_control, bios=args.bios_control, raw_io=args.raw_io,
                                devices=args.device, hwmon=hwmon, bind_address=args.bind_address,
                                public_origin=args.public_origin, image=args.image, apparmor_profile=args.apparmor_profile)
    os_info = host_os_release(args.platform)
    for device in args.device:
        if not stat.S_ISCHR(os.stat(device).st_mode):
            raise ValueError(f"设备不是字符设备：{device}")
    # Never recursively chmod/chown a NAS dataset or its existing contents.
    os.umask(0o077)
    secure_directory(directory, 0o700)
    marker = directory / ".llled-deployment.json"
    marker.write_text(json.dumps({"application": "ugreen-nas-tool", "schema": 1}) + "\n", encoding="utf-8")
    secure_directory(directory / "data", 0o700)
    secure_directory(directory / "ipc", 0o750, args.web_gid)
    secure_directory(directory / "auth", 0o750, args.web_gid)
    for name in LOCK_NAMES:
        prepare_lock(Path("/run") / name)
    auth = directory / "auth/auth.json"
    if auth.is_symlink() or (auth.exists() and (not auth.is_file() or auth.stat().st_nlink != 1 or auth.stat().st_uid != 0)):
        raise ValueError("认证文件必须为 root 所有的普通文件，不能为链接")
    if not auth.exists() or args.reset_password:
        password = getpass.getpass("设置 Web 管理密码（至少 12 个字符）：")
        if password != getpass.getpass("再次输入密码："):
            raise ValueError("两次输入的密码不一致")
        record = password_record(password)
        if auth.is_symlink():
            raise ValueError("拒绝符号链接认证文件")
        auth.write_text(json.dumps(record) + "\n", encoding="utf-8")
    if auth.is_symlink() or not auth.is_file() or auth.stat().st_nlink != 1 or auth.stat().st_uid != 0:
        raise ValueError("认证文件必须为 root 所有的普通文件，不能为链接")
    os.chown(auth, 0, args.web_gid)
    auth.chmod(0o640)
    verify_web_permissions(directory, args.web_uid, args.web_gid)
    recovery = (Path(__file__).with_name("host_recovery.py")).read_text(encoding="utf-8")
    recovery = recovery.replace('DEPLOYMENT_ID = ""', 'DEPLOYMENT_ID = ' + repr(deployment_id(directory)))
    for filename, content in (("compose.yaml", json.dumps(document, ensure_ascii=False, indent=2) + "\n"),
                              ("host-os-release", os_info), ("host-name", platform.node() + "\n"),
                              ("recover.py", recovery)):
        target = directory / filename
        if target.is_symlink() or (target.exists() and (not target.is_file() or target.stat().st_nlink != 1)):
            raise ValueError(f"输出必须为普通文件，不能为链接：{target}")
        target.write_text(content, encoding="utf-8")
    (directory / "host-os-release").chmod(0o644)
    (directory / "host-name").chmod(0o644)
    (directory / "recover.py").chmod(0o700)
    print(f"已生成 {directory / 'compose.yaml'}；没有启动容器或执行硬件控制。")
    print("宿主机重启后请重新执行此命令以准备 /run 锁文件，再启动应用。")
    print("也可将以下命令配置为容器服务启动前的 root 开机任务：\n" + boot_command())
    print("Docker 启动后的 root 恢复任务（只启动本部署已有的容器）：\npython3 " + shlex.quote(str(directory / "recover.py")))


if __name__ == "__main__":
    main()
