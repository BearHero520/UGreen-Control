#!/usr/bin/env python3
"""Build the experimental native DSM SPK locally; never push or publish."""
from __future__ import annotations
import argparse
import base64
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile
import sys

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
sys.path.insert(0, str(ROOT / "app/server"))
from portable_runtime import verify_payload

PACKAGE = "UGreenToolbox"
ACCOUNT = "ugreen-toolbox"
VERSION = "3.0.1-1006"
DESKTOP_APP = "SYNO.SDS.App.UGreenToolbox.Instance"


def tgz(files):
    output = io.BytesIO()
    with gzip.GzipFile(fileobj=output, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for name, (data, mode) in sorted(files.items()):
                entry = tarfile.TarInfo(name)
                entry.size, entry.mode, entry.mtime = len(data), mode, 0
                archive.addfile(entry, io.BytesIO(data))
    return output.getvalue()


def payload_files():
    verify_payload(ROOT / "app")
    canonical = REPOSITORY / "App.Native.UGreenLED/app/server/bin/ugreen_leds_cli"
    if canonical.read_bytes() != (ROOT / "app/server/bin/ugreen_leds_cli").read_bytes():
        raise ValueError("LED CLI must match the canonical application bundle")
    canonical_compat = REPOSITORY / "App.Native.UGreenLED/app/server/bin/ugreen_leds_cli.compat"
    if canonical_compat.is_file() and canonical_compat.read_bytes() != (ROOT / "app/server/bin/ugreen_leds_cli.compat").read_bytes():
        raise ValueError("LED compatibility ELF must match the canonical application bundle")
    files = {}
    for path in (ROOT / "app").rglob("*"):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        name = path.relative_to(ROOT).as_posix()
        if path.suffix == ".pyc":
            continue
        mode = 0o755 if name.startswith("app/server/bin/") else 0o644
        files[name] = (path.read_bytes(), mode)
    files["app/server/native_runtime.py"] = ((ROOT / "synology/native_runtime.py").read_bytes(), 0o644)
    files["app/server/native_desktop.py"] = ((ROOT / "synology/native_desktop.py").read_bytes(), 0o644)
    for name in ("native-login.html", "native-login.js", "native-login.css"):
        files["app/www/" + name] = ((ROOT / "synology/ui" / name).read_bytes(), 0o644)
    files["app/README.md"] = ((ROOT / "synology/README.md").read_bytes(), 0o644)
    files["LICENSE"] = ((ROOT / "LICENSE").read_bytes(), 0o644)
    return files


def service_units(payload_hash, python):
    config = {"source": "/var/packages/UGreenToolbox/target/native-payload.tgz", "sha256": payload_hash,
              "python": python, "version": VERSION}
    source = (ROOT / "synology/native_bootstrap.py").read_text(encoding="utf-8")
    source += "\nboot(" + repr(config) + ")\n"
    encoded = base64.b64encode(source.encode()).decode()
    hardware = f'''[Unit]
Description=UGREEN Toolbox native hardware broker
Requires=pkg-UGreenToolbox-logs.service
After=local-fs.target pkg-UGreenToolbox-logs.service
PartOf=pkgctl-UGreenToolbox.service
BindsTo=pkgctl-UGreenToolbox.service
Before=pkgctl-UGreenToolbox.service
RequiresMountsFor=/var/packages/UGreenToolbox/target

[Service]
Type=notify
NotifyAccess=main
User=root
ExecStart={python} -I -S -c "import base64;exec(base64.b64decode('{encoded}'))"
Restart=on-failure
RestartSec=3
TimeoutStopSec=90
TimeoutStartSec=90
KillMode=control-group
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true
PrivateTmp=true
CapabilityBoundingSet=CAP_CHOWN CAP_DAC_OVERRIDE CAP_SYS_RAWIO
RestrictAddressFamilies=AF_UNIX
UMask=0077
Slice=UGreenToolbox.slice
StandardOutput=journal
StandardError=journal
SyslogIdentifier=UGreenToolbox-hardware

[Install]
WantedBy=pkgctl-UGreenToolbox.service

[X-Synology]
DefaultEnable=yes
'''
    web = f'''[Unit]
Description=UGREEN Toolbox native Web service
Requires=pkg-UGreenToolbox-logs.service
Wants=pkg-UGreenToolbox-hardware.service
After=pkg-UGreenToolbox-hardware.service pkg-UGreenToolbox-logs.service
PartOf=pkgctl-UGreenToolbox.service
BindsTo=pkgctl-UGreenToolbox.service
Before=pkgctl-UGreenToolbox.service

[Service]
Type=simple
User={ACCOUNT}
Group={ACCOUNT}
ExecStart={python} -I -S /run/ugreen-nas-native/current/app/server/native_runtime.py web
Restart=on-failure
RestartSec=3
TimeoutStopSec=15
KillMode=control-group
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true
PrivateTmp=true
CapabilityBoundingSet=
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
UMask=0077
Slice=UGreenToolbox.slice
StandardOutput=journal
StandardError=journal
SyslogIdentifier=UGreenToolbox-web

[Install]
RequiredBy=pkgctl-UGreenToolbox.service

[X-Synology]
DefaultEnable=yes
'''
    log_config = dict(config, role="logs")
    log_source = (ROOT / "synology/native_bootstrap.py").read_text(encoding="utf-8") + "\nboot(" + repr(log_config) + ")\n"
    log_encoded = base64.b64encode(log_source.encode()).decode()
    logs = hardware.replace("native hardware broker", "read-only log collector").replace(
        "Requires=pkg-UGreenToolbox-logs.service\n", "").replace(
        "After=local-fs.target pkg-UGreenToolbox-logs.service", "After=local-fs.target").replace(
        encoded, log_encoded).replace(" CAP_SYS_RAWIO", "").replace(
        "WantedBy=", "RequiredBy=").replace("TimeoutStopSec=90", "TimeoutStopSec=15").replace(
        "SyslogIdentifier=UGreenToolbox-hardware", "SyslogIdentifier=UGreenToolbox-logs")
    return hardware, web, logs


def package_files(python, payload):
    return {
        "native-payload.tgz": (payload, 0o644),
        "ui/UGreenToolbox.js": ((ROOT / "synology/ui/UGreenToolbox.js").read_bytes(), 0o644),
        "ui/config": (json.dumps({"UGreenToolbox.js": {DESKTOP_APP: {
            "type": "app", "appWindow": DESKTOP_APP, "icon": "images/icon_64.png",
            "title": "UGREEN工具箱", "allUsers": False, "allowMultiInstance": False, "hidden": False,
        }}}).encode(), 0o644),
        "etc/dsm-desktop.conf": ((ROOT / "synology/dsm-desktop.conf").read_bytes(), 0o644),
        "ui/images/icon_64.png": ((REPOSITORY / "App.Native.UGreenLED/ICON.PNG").read_bytes(), 0o644),
        "ui/images/icon_256.png": ((REPOSITORY / "App.Native.UGreenLED/ICON_256.PNG").read_bytes(), 0o644),
    }


def lifecycle(python):
    # Pre-install is read-only and executes as the package account. Explicitly
    # reject an interpreter replaceable by that account before a root unit uses it.
    preinst = f'''#!/bin/sh
set -eu
if [ -n "${{SYNOPKG_TEMP_LOGFILE:-}}" ]; then exec 2>"$SYNOPKG_TEMP_LOGFILE"; fi
if [ ! -x {python} ]; then
 echo '未找到受信任的 DSM Python3 解释器。' >&2
 exit 1
fi
{python} -I -S - <<'PY'
import os,sys
from pathlib import Path
if sys.version_info < (3,8): raise SystemExit('需要 Python 3.8 或更高版本')
try: libc=os.confstr('CS_GNU_LIBC_VERSION').split()[-1]
except (ValueError,AttributeError): raise SystemExit('需要经过验证的 GNU libc 运行环境')
if tuple(map(int,libc.split('.')[:2]))<(2,27):
 raise SystemExit('当前固定硬件载荷需要 glibc >= 2.27；请先在上游生成 DSM 兼容载荷，不能绕过此检查')
for path in (Path({python!r}).resolve(),Path(sys.executable).resolve()):
 for item in (path,*path.parents):
  s=item.stat()
  if s.st_uid!=0 or s.st_mode&0o022: raise SystemExit('Python 路径必须为 root 所有且不可由套件用户修改')
password=os.environ.get('wizard_admin_password','')
if os.environ.get('SYNOPKG_PKG_STATUS')=='INSTALL' and (not 12<=len(password)<=256 or ':' in password):
 raise SystemExit('请设置 12–256 字符的管理密码，不能包含冒号')
for key in ('wizard_led_control','wizard_bios_control','wizard_update_hardware_authorization'):
 if os.environ.get(key,'') not in ('','true','false'):
  raise SystemExit('硬件授权选项无效，请重新选择')
PY
'''
    postinst = f'''#!/bin/sh
set -eu
{python} -I -S - <<'PY'
import json,os,tempfile
from pathlib import Path
status=os.environ.get('SYNOPKG_PKG_STATUS','')
if status not in ('INSTALL','UPGRADE'): raise SystemExit('只能从安装或升级流程保存授权')
choices={{}}
for key in ('wizard_led_control','wizard_bios_control','wizard_update_hardware_authorization'):
 value=os.environ.get(key,'')
 if value not in ('','true','false'): raise SystemExit('硬件授权选项无效')
 choices[key]=value=='true'
def save(name,value):
 directory=Path('/var/packages/UGreenToolbox/var')
 fd,temp=tempfile.mkstemp(prefix='.install-',dir=str(directory))
 try:
  with os.fdopen(fd,'w') as file:
   json.dump(value,file);file.write('\\n');file.flush();os.fsync(file.fileno())
  os.replace(temp,directory/name)
 finally:
  Path(temp).unlink(missing_ok=True)
password=os.environ.get('wizard_admin_password','')
if password:
 if not 12<=len(password)<=256 or ':' in password: raise SystemExit('管理密码格式无效')
 save('setup.json',{{'password':password}})
save('install-authorization.json',{{'version':{VERSION!r},'apply':status=='INSTALL' or choices['wizard_update_hardware_authorization'],
 'led_control':choices['wizard_led_control'],'bios_control':choices['wizard_bios_control']}})
PY
'''
    control = f'''#!/bin/sh
set -eu
health() {{
{python} -I -S - "$1" <<'PY'
from contextlib import closing
import http.client,inspect,json,sys,time
deadline=time.monotonic()+float(sys.argv[1])
options={{'timeout':2}}
if 'syno_bypass_synoproxy' in inspect.signature(http.client.HTTPConnection).parameters:
 options['syno_bypass_synoproxy']=True
while True:
 try:
  with closing(http.client.HTTPConnection('127.0.0.1',5088,**options)) as client:
   client.request('GET','/healthz')
   response=client.getresponse()
   if response.status==200 and json.loads(response.read(1024)).get('ok') is True: raise SystemExit(0)
 except (OSError,ValueError): pass
 if time.monotonic()>=deadline: raise SystemExit(3)
 time.sleep(.25)
PY
}}
# The system package unit starts/stops dependencies. A package-user
# synosystemctl call would select --user and cannot control these root units.
case "${{1:-}}" in
 start) health 20 ;;
 stop) exit 0 ;;
 status) health 0 ;;
 prestart|prestop) exit 0 ;;
 *) exit 1 ;;
esac
'''
    result = {"scripts/preinst": preinst, "scripts/postinst": postinst, "scripts/start-stop-status": control}
    result["scripts/preupgrade"] = preinst
    for name in ("postupgrade", "preuninst", "postuninst"):
        result["scripts/" + name] = "#!/bin/sh\n# DSM removes the system units; private state is deliberately retained.\nexit 0\n"
    return {name: (data.encode(), 0o755) for name, data in result.items()}


def wizard(upgrade=False):
    choices = [
        {"key": "wizard_led_control", "desc": "启用灯光控制（内置 LED CLI）", "defaultValue": False},
        {"key": "wizard_bios_control", "desc": "启用 BIOS／风扇／来电恢复控制（上游支持的绿联机型）", "defaultValue": False},
    ]
    steps = [] if upgrade else [{"step_title": "设置管理密码", "items": [{"type": "textfield", "subitems": [
        {"key": "wizard_admin_password", "desc": "管理密码（12–256字符，不能含冒号）", "inputType": "password",
         "validator": {"allowBlank": False, "minLength": 12, "maxLength": 256}}
    ]}]}]
    items = [{"desc": "选择允许在应用内操作的硬件功能。授权会在首次启动时保存，重启后保留；安装不会执行硬件写入。机型检测、共享锁和操作确认仍然生效。"}]
    if upgrade:
        items += [{"desc": "默认保留已有授权。需要更改时，勾选“采用以下授权选择”，再分别选择两类控制；两类都不勾选表示关闭全部写入。"},
                  {"type": "multiselect", "subitems": [{"key": "wizard_update_hardware_authorization",
                    "desc": "采用以下授权选择，替换已有硬件授权", "defaultValue": False}]}]
    items.append({"type": "multiselect", "subitems": choices})
    steps.append({"step_title": "硬件控制授权", "items": items})
    return steps


def build(output, python="/usr/bin/python3"):
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", python) or ".." in python:
        raise ValueError("Python interpreter must be an absolute executable path")
    payload = tgz(payload_files())
    payload_hash = hashlib.sha256(payload).hexdigest()
    hardware, web, logs = service_units(payload_hash, python)
    version = VERSION
    info = {"package": PACKAGE, "version": version, "displayname": "UGREEN工具箱（原生实验版）",
            "description": "原生 DSM Web 与独立硬件服务。未通过 DSM 实机验收；无需 Container Manager。",
            "arch": "x86_64", "os_min_ver": "7.2-64570", "maintainer": "BearHero520", "startable": "yes",
            "precheckstartstop": "yes", "dsmuidir": "ui", "dsmappname": DESKTOP_APP,
            "instuninst_restart_services": "nginx.service"}
    files = {
        "INFO": ("".join(k + "=" + json.dumps(v, ensure_ascii=False) + "\n" for k,v in info.items()).encode(), 0o644),
        "package.tgz": (tgz(package_files(python, payload)), 0o644),
        "conf/privilege": (json.dumps({"defaults": {"run-as": "package"}, "username": ACCOUNT, "groupname": ACCOUNT}).encode(), 0o644),
        "conf/resource": (json.dumps({"web-config": {"nginx-static-config": {"enable": [
            {"type": "dsm", "relpath": "etc/dsm-desktop.conf"}
        ]}}}).encode(), 0o644),
        "conf/systemd/pkg-UGreenToolbox-hardware.service": (hardware.encode(), 0o644),
        "conf/systemd/pkg-UGreenToolbox-web.service": (web.encode(), 0o644),
        "conf/systemd/pkg-UGreenToolbox-logs.service": (logs.encode(), 0o644),
        "WIZARD_UIFILES/install_uifile": (json.dumps(wizard(), ensure_ascii=False).encode(), 0o644),
        "WIZARD_UIFILES/upgrade_uifile": (json.dumps(wizard(upgrade=True), ensure_ascii=False).encode(), 0o644),
        "LICENSE": ((ROOT / "LICENSE").read_bytes(), 0o644),
        "PACKAGE_ICON.PNG": ((REPOSITORY / "App.Native.UGreenLED/ICON.PNG").read_bytes(), 0o644),
        "PACKAGE_ICON_256.PNG": ((REPOSITORY / "App.Native.UGreenLED/ICON_256.PNG").read_bytes(), 0o644),
    }
    files.update(lifecycle(python))
    output.mkdir(parents=True, exist_ok=True)
    path = output / f"UGreenToolbox-{version}-x86_64.spk"
    with tarfile.open(path, "w") as archive:
        for name, (data, mode) in sorted(files.items()):
            entry = tarfile.TarInfo(name); entry.size=len(data); entry.mode=mode; entry.mtime=0
            archive.addfile(entry, io.BytesIO(data))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_suffix(".spk.sha256").write_text(f"{digest}  {path.name}\n", encoding="utf-8")
    return path, payload_hash


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir",type=Path,default=REPOSITORY/"dist")
    parser.add_argument("--python",default="/usr/bin/python3")
    args=parser.parse_args()
    if args.output_dir.resolve() != (REPOSITORY/"dist").resolve():
        parser.error("最终 SPK 和校验文件必须写入仓库根 dist/")
    path, digest=build(args.output_dir,args.python)
    print(f"Built {path}\nNative payload SHA256: {digest}\nExperimental: DSM installation and ABI must be verified before production use.")


if __name__=="__main__":
    main()
