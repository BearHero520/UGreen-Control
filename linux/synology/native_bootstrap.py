"""Embedded into the root-owned DSM system unit by build_spk.py.

The package account owns the installed package directory. Never import or execute
code from it with root privileges: verify an in-memory archive snapshot first,
then activate a root-owned immutable copy outside the package directory.
"""
import base64
import hashlib
import grp
import io
import json
import os
from pathlib import Path, PurePosixPath
import pwd
import re
import shutil
import stat
import sys
import tarfile
import tempfile

PACKAGE = "UGreenToolbox"
ACCOUNT = "ugreen-toolbox"
RUN_ROOT = Path("/run/ugreen-nas-native")
LOCKS = ("llled-hardware.lock", "ugreenctl-led-mcu.lock", "ugreenctl-it8613.lock",
         "ugreenctl-fand.lock", "ugreenctl-wol.lock", "ugreenctl-rtc.lock")


def owned_directory(path, mode=0o755, gid=0):
    if path.is_symlink():
        raise ValueError("native state directory cannot be a symlink")
    path.mkdir(mode=mode, exist_ok=True)
    info = path.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise ValueError("native state directory must be root-owned and not group/other writable")
    os.chown(path, 0, gid)
    path.chmod(mode)


def activate(source, state, expected):
    owned_directory(state)
    versions = state / "versions"
    owned_directory(versions)
    destination = versions / expected
    if destination.exists():
        owned_directory(destination)
        if (destination / ".payload-sha256").read_text().strip() != expected:
            raise ValueError("invalid private runtime receipt")
        return destination
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ValueError("native payload must be a regular file")
        with os.fdopen(fd, "rb", closefd=False) as file:
            snapshot = file.read(64 * 1024 * 1024 + 1)
    finally:
        os.close(fd)
    if len(snapshot) > 64 * 1024 * 1024 or hashlib.sha256(snapshot).hexdigest() != expected:
        raise ValueError("native payload checksum mismatch; refusing package-user controlled code")
    stage = Path(tempfile.mkdtemp(prefix=".stage-", dir=versions))
    try:
        with tarfile.open(fileobj=io.BytesIO(snapshot), mode="r:gz") as archive:
            members = archive.getmembers()
            if sum(member.size for member in members) > 64 * 1024 * 1024:
                raise ValueError("native payload is too large")
            seen = set()
            for member in members:
                name = PurePosixPath(member.name)
                if name.is_absolute() or ".." in name.parts or not name.parts or member.name in seen or not member.isfile():
                    raise ValueError("unsafe or duplicate native payload member")
                seen.add(member.name)
                target = stage.joinpath(*name.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as input_file:
                    target.write_bytes(input_file.read())
                target.chmod(0o755 if "/server/bin/" in "/" + member.name else 0o644)
        for directory, _, _ in os.walk(stage):
            Path(directory).chmod(0o755)
        (stage / ".payload-sha256").write_text(expected + "\n")
        os.rename(stage, destination)
    finally:
        if stage.exists():
            resolved = stage.resolve()
            if resolved.parent != versions.resolve() or not resolved.name.startswith(".stage-"):
                raise ValueError("unsafe staging cleanup")
            shutil.rmtree(resolved)
    return destination


def protected_json(path, initial):
    if path.is_symlink():
        raise ValueError("native configuration cannot be a symlink")
    if not path.exists():
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as file:
            json.dump(initial, file)
            file.write("\n")
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o022:
        raise ValueError("native configuration must be root-owned and not writable by the package account")
    return json.loads(path.read_text())


def save_policy(path, value):
    """Replace policy and its installation receipt in one durable transaction."""
    fd, temporary = tempfile.mkstemp(prefix=".policy-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as file:
            json.dump(value, file)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def installation_authorization(state, setup_file, uid, version):
    """Consume one bounded installer request per root-pinned SPK version.

    DSM installs the pinned root unit before starting the package. The logs unit
    consumes choices before Web starts, and records the version in root policy.
    A package account cannot replay a request to alter policy on later restarts.
    """
    import fcntl
    if os.geteuid() != 0 or not re.fullmatch(r"\d+\.\d+\.\d+-\d+", version):
        raise ValueError("invalid trusted installation version")
    guard = os.open(state / "policy-install.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(guard)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o077:
            raise ValueError("invalid installation lock")
        fcntl.flock(guard, fcntl.LOCK_EX)
        policy = protected_json(state / "policy.json", {})
        receipts = policy.get("installation_authorizations", {})
        if not isinstance(receipts, dict):
            raise ValueError("invalid installation receipt")
        if version in receipts:
            return policy
        result, request = "missing", None
        try:
            fd = os.open(setup_file.parent / "install-authorization.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            try:
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_uid != uid or info.st_nlink != 1 or info.st_mode & 0o077:
                    raise ValueError("unsafe installer request")
                raw = os.read(fd, 4097)
            finally:
                os.close(fd)
            if len(raw) > 4096:
                raise ValueError("oversized installer request")
            request = json.loads(raw)
            if (not isinstance(request, dict) or set(request) != {"version", "apply", "led_control", "bios_control"}
                    or request["version"] != version
                    or any(type(request[key]) is not bool for key in ("apply", "led_control", "bios_control"))):
                raise ValueError("invalid installer choices")
            if request["apply"]:
                policy.update(led_control=request["led_control"], bios_control=request["bios_control"])
                result = "applied"
            else:
                result = "preserved"
        except FileNotFoundError:
            pass
        except (OSError, ValueError, TypeError):
            result = "rejected"
            # Never include untrusted request contents or installation secrets.
            print("UGreenToolbox: installation authorization rejected; retaining existing policy", file=sys.stderr)
        receipts[version] = {"result": result}
        policy["installation_authorizations"] = receipts
        save_policy(state / "policy.json", policy)
        return policy
    finally:
        os.close(guard)


def initialize(runtime, state, account, setup_file, prepare_hardware=True, installation_version=None):
    identity = pwd.getpwnam(account)
    web_gid = grp.getgrnam(account).gr_gid
    if identity.pw_uid == 0 or web_gid == 0:
        raise ValueError("native Web account must be unprivileged")
    owned_directory(state / "data", 0o700)
    owned_directory(state / "auth", 0o750, web_gid)
    auth = state / "auth/auth.json"
    if not auth.exists():
        fd = os.open(setup_file, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != identity.pw_uid or info.st_nlink != 1 or info.st_mode & 0o077:
                raise ValueError("invalid package installation credentials")
            with os.fdopen(fd, "r", closefd=False) as file:
                setup = json.loads(file.read(8193))
        finally:
            os.close(fd)
        sys.path.insert(0, str(runtime / "app/server"))
        from portable_runtime import password_record
        protected_json(auth, password_record(setup["password"]))
    protected_json(auth, {})
    os.chown(auth, 0, web_gid)
    auth.chmod(0o640)
    policy = protected_json(state / "policy.json", {"led_control": False, "bios_control": False, "public_origin": ""})
    if not isinstance(policy.get("led_control"), bool) or not isinstance(policy.get("bios_control"), bool):
        raise ValueError("invalid root-owned hardware policy")
    if installation_version is not None:
        policy = installation_authorization(state, setup_file, identity.pw_uid, installation_version)
    origin = policy.get("public_origin", "")
    if origin:
        from urllib.parse import urlsplit
        parsed = urlsplit(origin)
        if parsed.scheme != "https" or not parsed.hostname or parsed.path or parsed.query or parsed.fragment:
            raise ValueError("native public_origin must be an HTTPS origin")
    owned_directory(RUN_ROOT)
    owned_directory(RUN_ROOT / "ipc", 0o750, web_gid)
    for name in LOCKS if prepare_hardware else ():
        fd = os.open(Path("/run") / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or info.st_mode & 0o077:
                raise ValueError("invalid shared hardware lock: " + name)
        finally:
            os.close(fd)
    environment = {
        "UGREEN_PLATFORM": "synology", "UGREEN_DISTRIBUTION": "spk", "UGREEN_WEB_UID": str(identity.pw_uid), "UGREEN_WEB_GID": str(web_gid),
        "UGREEN_BROKER_SOCKET": str(RUN_ROOT / "ipc/hardware.sock"), "UGREEN_RUNTIME_DIR": str(RUN_ROOT / "state"),
        "UGREEN_LOG_SOCKET": str(RUN_ROOT / "ipc/logs.sock"),
        "UGREEN_AUTH_FILE": str(auth), "VAR_DIR": str(state / "data"), "UGREEN_BIND": "127.0.0.1", "PORT": "5088",
        "UGREEN_PUBLIC_ORIGIN": origin, "UGREEN_ALLOW_LED_CONTROL": str(policy["led_control"]).lower(),
        "UGREEN_ALLOW_BIOS_CONTROL": str(policy["bios_control"]).lower(), "UGREEN_NETWORK_MODE": "carrier", "UGREEN_DISK_MODE": "stats",
    }
    environment_file = RUN_ROOT / "environment.json"
    if environment_file.is_symlink():
        raise ValueError("invalid runtime environment path")
    environment_file.write_text(json.dumps(environment) + "\n")
    environment_file.chmod(0o644)
    temporary = RUN_ROOT / "current.next"
    temporary.unlink(missing_ok=True)
    temporary.symlink_to(runtime)
    os.replace(temporary, RUN_ROOT / "current")
    return environment


def boot(config):
    if os.geteuid() != 0:
        raise ValueError("native bootstrap requires the DSM system unit")
    os.umask(0o077)
    source = Path(config["source"])
    real = source.resolve()
    if len(real.parts) < 3 or not real.parts[1].startswith("volume") or not real.parts[1][6:].isdigit():
        raise ValueError("native package must be installed on a DSM /volumeN storage volume")
    volume = Path("/") / real.parts[1]
    info = volume.stat()
    if info.st_uid != 0 or info.st_mode & 0o022:
        raise ValueError("DSM volume root is not protected")
    state = volume / "@UGreenToolbox"
    runtime = activate(source, state, config["sha256"])
    role = config.get("role", "broker")
    if role not in {"broker", "logs"}:
        raise ValueError("invalid root service role")
    environment = initialize(runtime, state, ACCOUNT, Path("/var/packages") / PACKAGE / "var/setup.json",
                             role == "broker", config.get("version"))
    os.environ.update(environment)
    os.execv(config["python"], [config["python"], "-I", "-S", str(runtime / "app/server/native_runtime.py"), role])
