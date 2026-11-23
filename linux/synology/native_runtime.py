#!/usr/bin/env python3
"""Native DSM service entry point, executed only from the root-sealed runtime."""
import json
import os
from pathlib import Path
import stat
import sys


def main():
    path = Path("/run/ugreen-nas-native/environment.json")
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise ValueError("native environment must be a root-owned regular file")
    environment = json.loads(path.read_text())
    role = sys.argv[1]
    if role not in {"web", "broker", "logs"}:
        raise ValueError("unknown native service role")
    expected_uid = int(environment["UGREEN_WEB_UID"]) if role == "web" else 0
    if os.geteuid() != expected_uid:
        raise ValueError("native service identity does not match its role")
    if role == "web" and (os.getegid() != int(environment["UGREEN_WEB_GID"]) or 0 in os.getgroups()):
        raise ValueError("native Web must use the dedicated package group and no root supplementary group")
    if role == "web":
        # The package account removes its own one-use installation secret.
        Path("/var/packages/UGreenToolbox/var/setup.json").unlink(missing_ok=True)
        Path("/var/packages/UGreenToolbox/var/install-authorization.json").unlink(missing_ok=True)
    os.environ.update(environment)
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import portable_runtime
    sys.argv = ["portable_runtime.py", role]
    if role == "web":
        from native_desktop import DesktopWebServer
        portable_runtime.main(web_server_class=DesktopWebServer)
    elif role == "logs":
        from log_export import run_log_service
        run_log_service()
    else:
        portable_runtime.main()


if __name__ == "__main__":
    main()
