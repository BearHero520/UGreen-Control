#!/usr/bin/env python3
"""Root host boot recovery, scoped to existing containers of one deployment.

The deployment generator writes a standalone copy with DEPLOYMENT_ID set.
No Docker socket is mounted into an application container.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import subprocess
import time

DEPLOYMENT_ID = ""
LOCK_NAMES = ("llled-hardware.lock", "ugreenctl-led-mcu.lock", "ugreenctl-it8613.lock",
              "ugreenctl-fand.lock", "ugreenctl-wol.lock", "ugreenctl-rtc.lock")
LABEL = "io.bearhero.llled.deployment"


def docker(*arguments):
    process = subprocess.run(["docker", *arguments], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, timeout=20)
    if process.returncode:
        raise RuntimeError(process.stderr.strip()[-1000:] or "Docker command failed")
    return process.stdout


def containers(deployment_id):
    identifiers = docker("ps", "-a", "--filter", f"label={LABEL}={deployment_id}", "--format", "{{.ID}}").split()
    if not identifiers:
        raise RuntimeError("没有找到本部署的已安装容器；请先通过 NAS 界面安装")
    entries = json.loads(docker("inspect", *identifiers))
    found = {}
    for entry in entries:
        role = entry["Config"]["Labels"].get("io.bearhero.llled.role")
        if role not in {"hardware", "web"} or role in found:
            raise RuntimeError("部署标签存在重复或未知服务；请先检查 NAS 中的应用实例")
        found[role] = entry
    if set(found) != {"hardware", "web"}:
        raise RuntimeError("部署必须同时包含 hardware 和 web 容器")
    return found


def prepare_locks(lock_root=Path("/run")):
    for name in LOCK_NAMES:
        fd = os.open(lock_root / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o077:
                raise RuntimeError("宿主锁权限不安全：" + name)
        finally:
            os.close(fd)


def recover(deployment_id, timeout=180):
    deadline = time.monotonic() + timeout
    while True:
        try:
            docker("info", "--format", "{{.ID}}")
            break
        except (RuntimeError, OSError, subprocess.TimeoutExpired):
            if time.monotonic() >= deadline:
                raise RuntimeError("等待 Docker 启动超时")
            time.sleep(1)
    found = containers(deployment_id)
    if found["hardware"]["State"]["Running"] and any(not Path("/run", n).exists() for n in LOCK_NAMES):
        raise RuntimeError("运行中的硬件服务丢失宿主锁；拒绝新建 inode，请先人工停止应用")
    prepare_locks()
    for role in ("hardware", "web"):
        identifier = found[role]["Id"]
        docker("start", identifier)
        while True:
            state = json.loads(docker("inspect", identifier))[0]["State"]
            if state["Running"] and state.get("Health", {}).get("Status") == "healthy":
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(role + " 恢复健康状态超时；请检查应用日志、权限和共享锁")
            time.sleep(1)
    return {"ok": True, "deployment": deployment_id, "restored": ["hardware", "web"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if os.geteuid() != 0 or not DEPLOYMENT_ID or not 10 <= args.timeout <= 600:
        parser.error("请以 root 运行 prepare.py 为本部署生成的恢复脚本")
    print(json.dumps(recover(DEPLOYMENT_ID, args.timeout), ensure_ascii=False))


if __name__ == "__main__":
    main()
