#!/usr/bin/env python3
"""Create audited, local Almighty archives; never install or publish anything."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import tarfile


PROGRAMS = ("ugreenctl", "ugreen_leds_cli", "ugreenctl-fand", "ugreenctl-ledd")
PLUGINS = (
    "dx4600", "dxp2800", "dxp4800", "dxp4800plus", "dxp4800s",
    "dxp480tplus", "dxp6800pro", "dxp6800plus", "dxp6800", "dxp8800plus", "dxp8800pro", "dxp4800gt", "idx6012",
)
ARCH_MACHINES = {"x86_64": 62, "aarch64": 183}
EXCLUDED = {"DH2100", "DH2100+"}
SOURCE_DIRS = (
    "core", "disk", "ec", "fan", "include", "led", "models", "network",
    "power", "superio", "tests",
)
# Deliberately explicit: firmware-inbox, build trees and arbitrary extras cannot
# enter an archive through a recursive copy or a user-supplied file pattern.
DOCUMENTS = (
    "LICENSE", "THIRD_PARTY_NOTICES.md", "README.md", "README.zh-CN.md",
    "led/LICENSE.upstream-led", "disk/NOTICE.md",
    "docs/ALMIGHTY_PACKAGING.zh-CN.md", "docs/ARCHITECTURE.md",
    "docs/RELEASE_v0.3.0-preview.1.zh-CN.md",
    "docs/RELEASE_v0.3.0.zh-CN.md",
    "docs/UGREEN_CONTROL_MIGRATION.zh-CN.md",
    "docs/HARDWARE_SUPPORT.md", "docs/COMPATIBILITY.md",
    "docs/DXP8800_PRO_AUDIT.zh-CN.md",
    "docs/DXP4800GT_HARDWARE_AUDIT.zh-CN.md",
    "models/dxp4800gt-fan-curve.json",
    "docs/COMPATIBILITY.zh-CN.md", "docs/FAN_CURVE.md",
    "docs/LED_CONTROL.zh-CN.md", "docs/LED_EVIDENCE_COLLECTION.zh-CN.md",
    "docs/LINUX_DEPLOYMENT.zh-CN.md",
    "docs/ALMIGHTY_STATUS.zh-CN.md",
    "docs/BUILD_REPRODUCIBILITY.zh-CN.md",
    "docs/AC_RECOVERY_PERSISTENCE.zh-CN.md",
    "docs/N76_DIRECT_CONTROL.zh-CN.md",
    "tools/n76/sources.json",
    "integration/llled/ac-recovery/README.zh-CN.md",
    "integration/llled/ac-recovery/llled-startup-persistence.patch",
    "integration/llled/ac-recovery/source-hashes.json",
    "docs/DISK_MAPPING_FIRMWARE.zh-CN.md",
    "docs/ALL_MODELS_FIRMWARE_INTAKE.zh-CN.md",
    "docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md",
    "docs/ALL_MODELS_DISK_EVIDENCE.zh-CN.md",
    "docs/ALL_MODELS_WAKE_EVIDENCE.zh-CN.md",
    "validation/almighty/2026-10-27-wake-audit/firmware-evidence.json",
    "docs/FIRMWARE_RUNTIME_TRACE_20261026.zh-CN.md",
    "docs/DH2100_FIRMWARE_SEARCH.zh-CN.md",
    "docs/DXP2800_GT_LED_REVERSE.zh-CN.md",
    "docs/DXP6800_1.20_FIRMWARE_CHECK.zh-CN.md",
    "docs/DXP6800_SUPPORT.zh-CN.md",
    "docs/DX4600_REVERSE_ENGINEERING.zh-CN.md",
    "docs/DXP4800_REVERSE_ENGINEERING.md",
    "docs/DXP4800S_REVERSE_ENGINEERING.md",
    "docs/DXP4800S_REVERSE_ENGINEERING.zh-CN.md",
    "docs/DXP480T_PLUS_FAN_REVERSE_ENGINEERING.md",
    "docs/DXP6800_PRO_REVERSE_ENGINEERING.md",
    "docs/WOL_FIRMWARE_EVIDENCE.md", "docs/WOL_FIRMWARE_EVIDENCE.zh-CN.md",
    "docs/SCHEDULED_POWER_FIRMWARE_EVIDENCE.md",
)
EXAMPLES = (
    "scripts/ugreenctl-ledd.service", "scripts/ugreenctl-ledd-gt.service",
    "scripts/collect-disk-evidence.sh", "scripts/collect-led-evidence.sh",
    "scripts/ugreenctl-startup.py",
    "scripts/ugreenctl-startup.service",
)


class PackageError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PackageError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def load_json(data: bytes, label: str) -> dict:
    def unique_pairs(items: list) -> dict:
        result = {}
        for key, value in items:
            require(key not in result, f"Duplicate JSON key {key!r} in {label}")
            result[key] = value
        return result
    try:
        value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_pairs)
    except (ValueError, UnicodeError) as exc:
        raise PackageError(f"Invalid JSON in {label}: {exc}") from exc
    require(isinstance(value, dict), f"Expected JSON object: {label}")
    return value


def inside(path: Path, directory: Path) -> bool:
    return path == directory or directory in path.parents


def read_regular(path: Path, boundary: Path | None = None) -> bytes:
    require(path.is_file() and not path.is_symlink(), f"Missing/non-regular/symlink input: {path}")
    resolved = path.resolve()
    if boundary is not None:
        require(inside(resolved, boundary.resolve()), f"Input escapes allowed directory: {path}")
    for parent in path.parents:
        if boundary is not None and parent == boundary:
            break
        require(not parent.is_symlink(), f"Symlink ancestor is not accepted: {path}")
    return path.read_bytes()


def relative_key(key: str) -> PurePosixPath:
    require(isinstance(key, str) and "\\" not in key and ":" not in key,
            f"Expected portable repository-relative path: {key!r}")
    path = PurePosixPath(key)
    require(not path.is_absolute() and bool(path.parts) and ".." not in path.parts,
            f"Unsafe repository-relative path: {key!r}")
    return path


def path_identity(path: Path) -> str:
    return os.path.normcase(str(path.resolve()))


def verified_hashes(root: Path, entries: object, label: str, source: bool) -> dict:
    require(isinstance(entries, dict) and bool(entries), f"Missing {label} hash map")
    result = {}
    errors = []
    for key, expected in entries.items():
        require(isinstance(expected, str) and re.fullmatch(r"[0-9a-fA-F]{64}", expected) is not None,
                f"Invalid SHA-256 in {label}: {key}")
        if source:
            path = root.joinpath(*relative_key(key).parts)
        else:
            require(isinstance(key, str), f"Invalid binary path in {label}")
            path = Path(key)
            if not path.is_absolute():
                path = root.joinpath(*relative_key(key).parts)
        data = read_regular(path, root if source else None)
        identity = path_identity(path)
        require(identity not in result, f"Duplicate file aliases in {label}: {path}")
        if sha256(data) != expected.lower():
            errors.append(str(key))
        result[identity] = (data, expected.lower())
    require(not errors, f"Stale {label} SHA-256: " + ", ".join(errors))
    return result


def source_inputs(root: Path) -> set[str]:
    inputs = {".gitignore", ".gitattributes", "CMakeLists.txt", "scripts/package-almighty.py", "tests/test_package_runtime.py",
              "tests/test_package_almighty.py", *DOCUMENTS, *EXAMPLES,
              "models/compatibility.json", "models/official-products.json",
              "tests/test_startup_state.py"}
    # Author-maintained reproduction source and external-input metadata, never
    # the actual downloaded runtimes or firmware.
    for directory in ("tools/testing", "tools/toolchains"):
        if (root / directory).is_dir():
            for path in (root / directory).rglob("*"):
                if path.is_file() and path.suffix in (".py", ".cmake", ".cmd", ".json"):
                    inputs.add(path.relative_to(root).as_posix())
    for directory in SOURCE_DIRS:
        require((root / directory).is_dir(), f"Missing source directory: {directory}")
        for path in (root / directory).rglob("*"):
            if path.suffix in (".c", ".h") and path.is_file():
                inputs.add(path.relative_to(root).as_posix())
    return inputs


def check_elf(data: bytes, architecture: str, label: str, module: bool = False) -> None:
    require(len(data) >= 64 and data[:7] == b"\x7fELF\x02\x01\x01",
            f"Expected ELF64 little-endian v1: {label}")
    elf_type, machine, version = struct.unpack_from("<HHI", data, 16)
    require(machine == ARCH_MACHINES[architecture] and version == 1,
            f"Wrong ELF machine/version for {architecture}: {label}")
    require(elf_type == 3 if module else elf_type in (2, 3), f"Wrong ELF type: {label}")
    require(struct.unpack_from("<H", data, 52)[0] == 64, f"Wrong ELF header length: {label}")


def elf_runtime(data: bytes, label: str) -> dict:
    """Inspect load-time dependencies without executing the file or using ldd.

    ELF64 program/dynamic headers and GNU version requirements are read from
    file-backed PT_LOAD ranges, so stripped/sectionless files work too. This
    describes an ABI requirement, not proof that a distribution was tested.
    """
    def span(offset: int, size: int) -> bytes:
        require(0 <= offset <= len(data) and 0 <= size <= len(data)-offset,
                f"ELF runtime data outside file: {label}")
        return data[offset:offset+size]

    def cstring(raw: bytes, offset: int = 0) -> str:
        require(0 <= offset < len(raw), f"ELF runtime string offset out of range: {label}")
        end = raw.find(b"\0", offset)
        require(end >= 0, f"Unterminated ELF runtime string: {label}")
        value = raw[offset:end]
        require(bool(value) and all(32 <= c < 127 for c in value),
                f"Invalid ELF runtime string: {label}")
        return value.decode("ascii")

    require(len(data) >= 64 and data[:7] == b"\x7fELF\x02\x01\x01",
            f"Expected ELF64 runtime headers: {label}")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phsize, phcount = struct.unpack_from("<HH", data, 54)
    require(phsize == 56 and 0 < phcount < 65535,
            f"Unsupported/missing ELF program headers: {label}")
    span(phoff, phsize*phcount)
    loads, dynamics, interpreters = [], [], []
    for index in range(phcount):
        kind, _, offset, address, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, phoff+index*phsize)
        if kind == 0:  # PT_NULL members have no defined semantics.
            continue
        raw = span(offset, filesz)
        if kind == 1:
            require(filesz <= memsz, f"ELF load file size exceeds memory size: {label}")
            loads.append((address, offset, filesz))
        elif kind == 2:
            require(filesz <= memsz, f"ELF dynamic file size exceeds memory size: {label}")
            dynamics.append((offset, address, raw))
        elif kind == 3:
            interpreters.append(cstring(raw))
    require(loads and len(dynamics) <= 1 and len(interpreters) <= 1,
            f"Invalid ELF load/dynamic/interpreter segments: {label}")

    def file_address(address: int, size: int) -> int:
        matches = {offset+address-base for base, offset, length in loads
                   if base <= address and size <= length and address-base <= length-size}
        require(len(matches) == 1, f"ELF runtime address is not uniquely file-backed: {label}")
        return matches.pop()

    result = {"interpreter": interpreters[0] if interpreters else None,
              "needed_libraries": [], "required_symbol_versions": {},
              "glibc_symbol_floor": None, "glibc_extra_requirements": [],
              "rpath": None, "runpath": None}
    if not dynamics:
        return result
    dynamic_offset, dynamic_address, dynamic = dynamics[0]
    require(file_address(dynamic_address, len(dynamic)) == dynamic_offset,
            f"ELF dynamic file offset disagrees with load address: {label}")
    require(len(dynamic) % 16 == 0, f"Invalid ELF dynamic segment length: {label}")
    tags = {}
    for offset in range(0, len(dynamic), 16):
        tag, value = struct.unpack_from("<qQ", dynamic, offset)
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    else:
        raise PackageError(f"ELF dynamic segment has no DT_NULL: {label}")
    for tag in (5, 10, 15, 29, 0x6ffffffe, 0x6fffffff):
        require(len(tags.get(tag, [])) <= 1, f"Duplicate ELF runtime tag {tag}: {label}")
    require(5 in tags and 10 in tags and tags[10][0] > 0,
            f"Missing ELF dynamic string table: {label}")
    strings = span(file_address(tags[5][0], tags[10][0]), tags[10][0])
    result["needed_libraries"] = sorted({cstring(strings, value) for value in tags.get(1, [])})
    for tag, key in ((15, "rpath"), (29, "runpath")):
        if tag in tags:
            # An explicitly empty search path is legal; preserve it as empty.
            value = tags[tag][0]
            require(value < len(strings), f"ELF search path offset out of range: {label}")
            result[key] = "" if strings[value] == 0 else cstring(strings, value)
    require((0x6ffffffe in tags) == (0x6fffffff in tags),
            f"Incomplete ELF version requirement tags: {label}")
    numeric_glibc, extra_glibc = set(), set()
    if 0x6ffffffe in tags:
        count = tags[0x6fffffff][0]
        require(0 < count <= 256, f"Invalid ELF version requirement count: {label}")
        address = tags[0x6ffffffe][0]
        for index in range(count):
            offset = file_address(address, 16)
            version, aux_count, library, aux, next_entry = struct.unpack_from("<HHIII", data, offset)
            require(version == 1 and 0 < aux_count <= 4096 and aux >= 16,
                    f"Invalid ELF version requirement: {label}")
            library = cstring(strings, library)
            require(library in result["needed_libraries"],
                    f"ELF versions reference an undeclared library: {label}")
            versions = result["required_symbol_versions"].setdefault(library, [])
            auxiliary = address+aux
            for item in range(aux_count):
                offset = file_address(auxiliary, 16)
                _, _, _, name, next_aux = struct.unpack_from("<IHHII", data, offset)
                name = cstring(strings, name)
                versions.append(name)
                if name.startswith("GLIBC_"):
                    match = re.fullmatch(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", name)
                    if match:
                        numeric_glibc.add(tuple(int(part) for part in match[1].split(".")))
                    else:
                        extra_glibc.add(name)
                require(next_aux >= 16 if item+1 < aux_count else next_aux == 0,
                        f"Invalid ELF version auxiliary chain: {label}")
                auxiliary += next_aux
            require(next_entry >= 16 if index+1 < count else next_entry == 0,
                    f"Invalid ELF version requirement chain: {label}")
            address += next_entry
        result["required_symbol_versions"] = {key: sorted(set(values))
                                               for key, values in result["required_symbol_versions"].items()}
    result["glibc_symbol_floor"] = ".".join(str(part) for part in max(numeric_glibc)) if numeric_glibc else None
    result["glibc_extra_requirements"] = sorted(extra_glibc)
    return result


def cmake_cache(path: Path) -> dict:
    result = {}
    for line in read_regular(path).decode("utf-8-sig").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)$", line)
        if match:
            result[match.group(1)] = match.group(2)
    return result


def linux_path(value: str, label: str, absolute: bool) -> PurePosixPath:
    path = PurePosixPath(value)
    require(bool(value) and "\\" not in value and ":" not in value and ".." not in path.parts
            and path.is_absolute() == absolute, f"Invalid Linux {label}: {value!r}")
    return path


def check_suite(value: object, architecture: str, test_count: int) -> None:
    if isinstance(value, str):
        match = re.fullmatch(r"([0-9]+)/([0-9]+) passed", value)
        require(match is not None and int(match[1]) == test_count and int(match[2]) == test_count,
                f"Expected {test_count}/{test_count} passed full suite for {architecture}")
    else:
        require(isinstance(value, dict) and value.get("status") == "passed"
                and type(value.get("passed")) is int and value["passed"] == test_count
                and type(value.get("total")) is int and value["total"] == test_count
                and value.get("failed", 0) == 0 and value.get("skipped", 0) == 0
                and value.get("not_run", 0) == 0,
                f"Invalid structured full-suite result for {architecture}")


def suite_artifacts(root: Path, verification: dict) -> dict:
    records = verification.get("full_suite_artifacts")
    require(isinstance(records, dict) and set(records) == set(ARCH_MACHINES),
            "Expected full_suite_artifacts for both architectures")
    logs = {}
    paths = set()
    for architecture, record in records.items():
        require(isinstance(record, dict) and isinstance(record.get("path"), str)
                and isinstance(record.get("sha256"), str)
                and re.fullmatch(r"[0-9a-fA-F]{64}", record["sha256"]) is not None,
                f"Invalid full-suite artifact: {architecture}")
        path = root.joinpath(*relative_key(record["path"]).parts)
        identity = path_identity(path)
        require(identity not in paths, "Architectures must use distinct full-suite log paths")
        paths.add(identity)
        require(path.suffix == ".log" and inside(path.resolve(), (root / "firmware-inbox/work").resolve()),
                f"Full-suite artifact must be a .log in firmware-inbox/work: {architecture}")
        data = read_regular(path, root)
        require(len(data) <= 8 * 1024 * 1024, f"Full-suite log is too large: {architecture}")
        require(sha256(data) == record["sha256"].lower(), f"Stale full-suite log SHA-256: {architecture}")
        logs[architecture] = data
    return logs


def check_suite_log(data: bytes, architecture: str, configured: list[str]) -> None:
    text = data.decode("utf-8-sig").replace("\r\n", "\n")
    total = len(configured)
    summaries = re.findall(r"(?m)^100% tests passed(?:, 0 tests failed)? out of ([0-9]+)\s*$", text)
    results = re.findall(r"(?m)^\s*([0-9]+)/([0-9]+)\s+Test\s+#\s*([0-9]+):\s+(\S+)\s+\.+\s+Passed\b[^\r\n]*$", text)
    require(summaries == [str(total)] and len(results) == total
            and {int(result[0]) for result in results} == set(range(1, total + 1))
            and all(int(result[1]) == total for result in results)
            and {int(result[2]) for result in results} == set(range(1, total + 1))
            and len({result[3] for result in results}) == total
            and {result[3] for result in results} == set(configured),
            f"Full-suite log does not prove all configured tests passed: {architecture}")


def check_catalog(compatibility: dict, official: dict, verification: dict, preview: bool) -> dict:
    try:
        integration = compatibility["builtin_capabilities"]["all_model_integration"]
        profiles = compatibility["builtin_capabilities"]["led"]["profiles"]
        products = official["products"]
        per_model = integration["per_model"]
        official_names = {p["name"] for p in products}
        require(len(products) == len(official_names) == 27, "Official catalog must contain 27 unique entries")
        require(len({p["official_product_id"] for p in products}) == 27, "Official product IDs must be unique")
        require(len(per_model) == 27 and {p["product_name"] for p in per_model} == official_names,
                "Compatibility per_model does not match official catalog")
        targets = {p["product_name"] for p in per_model if p["scope"] == "target"}
        excluded = {p["product_name"] for p in per_model if p["scope"] == "excluded-by-user"}
        require(len(targets) == 25 and excluded == EXCLUDED and targets | excluded == official_names,
                "Expected exact 25 targets and DH2100/DH2100+ exclusions")
        require({p["name"] for p in products if p["current_delivery_scope"] == "target"} == targets
                and {p["name"] for p in products if p["current_delivery_scope"] == "excluded-by-user"} == EXCLUDED,
                "Official delivery_scope entries disagree with compatibility")
        for scope in (integration, official["delivery_scope"], verification):
            require(scope.get("target_product_count", scope.get("target_count")) == 25
                    and len(scope["excluded_by_user"]) == 2 and set(scope["excluded_by_user"]) == EXCLUDED,
                    "Verification/catalog scope counts or exclusions disagree")
        require(integration["official_product_count"] == 27
                and integration["verified_current_firmware_product_count"] == 25,
                "Wrong official/verified firmware product count")
        require(len(profiles) == 26 and len({p["profile"] for p in profiles}) == 26
                and {p["product_name"] for p in profiles} == targets,
                "Expected 26 distinct LED profiles covering the exact 25 targets")
        counts = {name: sum(p["product_name"] == name for p in profiles) for name in targets}
        require(all(count == (2 if name == "DXP2800" else 1) for name, count in counts.items()),
                "Unexpected LED model/revision profile multiplicity")
        require({p.get("product_version") for p in profiles if p["product_name"] == "DXP2800"}
                == {"EM_DXP2800_V1.0.24", "DXP2800 V1.2 251215"},
                "DXP2800 must retain its two independently evidenced exact revisions")
        require({m["id"] for m in compatibility["models"]} == set(PLUGINS)
                and len(compatibility["models"]) == len(PLUGINS), "Plugin catalog differs from model whitelist")
        require(type(verification.get("complete")) is bool, "verification.complete must be a JSON boolean")
        completed_led_states = {"implemented", "implemented-experimental", "implemented-conditional", "verified"}
        completed_disk_states = {"implemented", "implemented-conditional", "not-applicable", "verified"}
        unknown_m2 = compatibility["builtin_capabilities"]["disk_mapping"]["unknown_m2_physical_slot_models"]
        require(isinstance(unknown_m2, list), "Invalid unknown M.2 mapping list")
        scope_complete = (verification["complete"] is True
                          and integration.get("state") in ("complete", "completed")
                          and unknown_m2 == []
                          and all(model.get("led") in completed_led_states
                                  and model.get("sata_physical_slots") in completed_disk_states
                                  and model.get("nvme_physical_slots") in completed_disk_states
                                  and model.get("remaining") == []
                                  for model in per_model if model["scope"] == "target"))
        if not preview:
            require(verification["complete"] is True, "verification.complete is not true; only explicit --preview is allowed")
            require(integration.get("state") in ("complete", "completed"),
                    "Compatibility integration is still incomplete; only --preview is allowed")
            for model in per_model:
                if model["scope"] != "target":
                    continue
                name = model["product_name"]
                require(model.get("led") in completed_led_states,
                        f"LED delivery remains incomplete: {name}")
                for capability in ("sata_physical_slots", "nvme_physical_slots"):
                    require(model.get(capability) in completed_disk_states,
                            f"{capability} remains incomplete: {name}")
                require(model.get("remaining") == [], f"Open delivery work remains: {name}")
            require(unknown_m2 == [],
                    "Unknown M.2 mapping list is not empty")
        return {"official_count": 27, "target_count": 25, "profile_count": 26,
                "targets": sorted(targets), "excluded_by_user": sorted(EXCLUDED),
                "scope_complete": scope_complete,
                "per_model": per_model, "release_status": integration.get("release_status", "unknown")}
    except (KeyError, TypeError) as exc:
        raise PackageError(f"Missing/invalid catalog field: {exc}") from exc


def prepare(root: Path, builds: dict, verification_path: Path, preview: bool, release: bool = False) -> list[dict]:
    require(not (preview and release), "Choose either preview or release channel")
    verification_data = read_regular(verification_path)
    verification = load_json(verification_data, str(verification_path))
    # Fail the scope gate first, even when other checkpoint hashes are stale.
    require(preview or release or verification.get("complete") is True,
            "verification.complete is not true; explicitly select --preview or --release without changing capability status")
    sources = verified_hashes(root, verification.get("source_sha256"), "source", True)
    binaries = verified_hashes(root, verification.get("binary_sha256"), "binary", False)
    missing = sorted(name for name in source_inputs(root) if path_identity(root / name) not in sources)
    require(not missing, "Missing source/document SHA-256 entries: " + ", ".join(missing))

    def source(name: str) -> bytes:
        return sources[path_identity(root / name)][0]

    compatibility = load_json(source("models/compatibility.json"), "compatibility")
    official = load_json(source("models/official-products.json"), "official-products")
    catalog = check_catalog(compatibility, official, verification, preview or release)
    logs = suite_artifacts(root, verification)
    version_match = re.search(rb"project\(ugreenctl\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s", source("CMakeLists.txt"))
    require(version_match is not None, "Cannot determine CMake project version")
    version = version_match[1].decode("ascii")
    plans = []
    for architecture, build in builds.items():
        cache = cmake_cache(build / "CMakeCache.txt")
        require(cache.get("CMAKE_BUILD_TYPE") == "Release", f"{architecture} build must be Release")
        prefix = linux_path(cache.get("CMAKE_INSTALL_PREFIX", ""), "install prefix", True)
        bindir = linux_path(cache.get("CMAKE_INSTALL_BINDIR", "bin"), "bindir", False)
        libdir = linux_path(cache.get("CMAKE_INSTALL_LIBDIR", "lib"), "libdir", False)
        docdir = linux_path(cache.get("CMAKE_INSTALL_DOCDIR") or "share/doc/ugreenctl", "docdir", False)
        require(cache.get("CMAKE_PROJECT_VERSION", version) == version, f"Stale CMake version: {architecture}")
        test_file = read_regular(build / "CTestTestfile.cmake", build)
        test_count = len(re.findall(rb"(?m)^add_test\(", test_file))
        require(test_count >= 23, f"Expected at least 23 configured tests: {architecture}")
        test_names = re.findall(rb'(?m)^add_test\((?:"([^"]+)"|\[=\[([^\]]+)\]=\]|([A-Za-z0-9_.+-]+))\s', test_file)
        configured = [b"".join(groups).decode("utf-8") for groups in test_names]
        require(len(configured) == test_count and all(configured) and len(set(configured)) == test_count,
                f"Cannot read unique configured CTest names: {architecture}")
        full_suite = verification.get("full_suite", {})
        require(isinstance(full_suite, dict), "Missing full_suite results")
        check_suite(full_suite.get(architecture), architecture, test_count)
        check_suite_log(logs[architecture], architecture, configured)
        actual_plugins = {p.name for p in (build / "models").glob("*.so")}
        require(actual_plugins == {name + ".so" for name in PLUGINS},
                f"Unexpected/missing model .so set in {architecture}: {sorted(actual_plugins)}")
        artifacts = [(build / name, (prefix / bindir / name).as_posix().lstrip("/"), 0o755, False) for name in PROGRAMS]
        artifacts += [(build / "models" / (name + ".so"),
                       (prefix / libdir / "ugreenctl/models" / (name + ".so")).as_posix().lstrip("/"),
                       0o755, True) for name in PLUGINS]
        entries, runtime_files = {}, {}
        for path, target, mode, module in artifacts:
            identity = path_identity(path)
            require(identity in binaries, f"Missing binary SHA-256 entry: {path}")
            data = binaries[identity][0]
            require(inside(path.resolve(), build.resolve()), f"Build artifact escapes build: {path}")
            check_elf(data, architecture, str(path), module)
            runtime_files[target] = elf_runtime(data, str(path))
            entries[target] = (data, mode)
        # Reject a stale Windows loader prefix even if a cache was edited alone.
        model_dir = str(prefix / libdir / "ugreenctl/models").encode("utf-8")
        require(model_dir + b"\0" in binaries[path_identity(build / "ugreenctl")][0],
                f"ugreenctl does not embed the configured Linux model directory: {architecture}")
        doc_base = (prefix / docdir).as_posix().lstrip("/")
        for name in (*DOCUMENTS, "models/compatibility.json", "models/official-products.json"):
            entries[f"{doc_base}/{name}"] = (source(name), 0o644)
        for name in EXAMPLES:
            entries[f"{doc_base}/examples/{Path(name).name}"] = (source(name), 0o755 if name.endswith(".sh") else 0o644)
        entries[f"{doc_base}/verification-root.json"] = (verification_data, 0o644)
        for log_architecture, data in logs.items():
            entries[f"{doc_base}/verification/ctest-{log_architecture}.log"] = (data, 0o644)
        status = "experimental-incomplete" if not catalog["scope_complete"] else "experimental-preview" if preview else "complete-local"
        stem = f"ugreenctl-almighty-{version}-linux-{architecture}" + ("-preview" if preview else "")
        glibc_versions = {tuple(int(part) for part in item["glibc_symbol_floor"].split("."))
                          for item in runtime_files.values() if item["glibc_symbol_floor"]}
        runtime = {
            "interpreters": sorted({item["interpreter"] for item in runtime_files.values() if item["interpreter"]}),
            "needed_libraries": sorted({name for item in runtime_files.values() for name in item["needed_libraries"]}),
            "glibc_symbol_floor": ".".join(str(part) for part in max(glibc_versions)) if glibc_versions else None,
            "glibc_extra_requirements": sorted({name for item in runtime_files.values() for name in item["glibc_extra_requirements"]}),
            "files": runtime_files,
            "notice": "ELF interpreter, DT_NEEDED and version requirements only. A numeric glibc floor is necessary, not sufficient, for compatibility; it does not establish kernel, driver or distribution validation. Libraries and loaders are not bundled.",
        }
        manifest = {
            "schema_version": 1, "package": stem, "architecture": architecture,
            "version": version, "branch": "main", "status": status,
            "preview": preview, "scope_complete": catalog["scope_complete"],
            "release_channel": "stable" if release else "preview" if preview else "complete-local",
            "verification_complete": verification["complete"],
            "physical_validation": verification.get("physical_validation", "unknown"),
            "install_prefix": str(prefix), "plugin_directory": model_dir.decode("utf-8"),
            "runtime_abi": runtime,
            "compatibility_cli_installation": "Opt-in; never overwrite an existing ugreen_leds_cli without administrator choice",
            "services": "Examples only; no installation or startup is performed",
            "verification_sha256": sha256(verification_data), "catalog": catalog,
            "full_suite": full_suite[architecture],
            "full_suite_artifacts": verification["full_suite_artifacts"],
            "source_sha256": verification["source_sha256"],
            "files": [{"path": name, "size": len(data), "mode": f"{mode:04o}", "sha256": sha256(data)}
                      for name, (data, mode) in sorted(entries.items())],
        }
        plans.append({"stem": stem, "entries": entries, "manifest": json_bytes(manifest)})
    return plans


def archive_bytes(plan: dict) -> bytes:
    output = io.BytesIO()
    # Fixed timestamps, owners and compression header make identical inputs
    # reproducible. All members are regular files with safe, relative names.
    with gzip.GzipFile(fileobj=output, mode="wb", mtime=0, filename="") as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
            entries = dict(plan["entries"])
            entries["manifest.json"] = (plan["manifest"], 0o644)
            for name, (data, mode) in sorted(entries.items()):
                relative_key(name)
                info = tarfile.TarInfo(f"{plan['stem']}/{name}")
                info.size, info.mode, info.mtime = len(data), mode, 0
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                archive.addfile(info, io.BytesIO(data))
    return output.getvalue()


def write_packages(plans: list[dict], output: Path) -> list[str]:
    payloads = {}
    for plan in plans:
        payloads[plan["stem"] + ".tar.gz"] = archive_bytes(plan)
        payloads[plan["stem"] + ".manifest.json"] = plan["manifest"]
    payloads["SHA256SUMS"] = "".join(f"{sha256(data)}  {name}\n" for name, data in sorted(payloads.items())).encode("ascii")
    for name in payloads:
        require(not (output / name).exists(), f"Refusing to overwrite existing output: {output / name}")
    output.mkdir(parents=True, exist_ok=True)
    # Exclusive creation also prevents a concurrent run from overwriting files.
    for name, data in payloads.items():
        with (output / name).open("xb") as stream:
            stream.write(data)
    return list(payloads)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--x86_64-build", type=Path, required=True)
    parser.add_argument("--aarch64-build", type=Path, required=True)
    parser.add_argument("--verification", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="Default: SOURCE/firmware-inbox/work/almighty-packages")
    channel = parser.add_mutually_exclusive_group()
    channel.add_argument("--preview", action="store_true", help="Explicitly select the preview distribution channel")
    channel.add_argument("--release", action="store_true", help="Select the stable distribution channel; retain experimental capability status and all validation requirements")
    parser.add_argument("--check-only", action="store_true", help="Validate everything without writing archives")
    args = parser.parse_args()
    try:
        root = args.source_root.resolve()
        output = (args.output or root / "firmware-inbox/work/almighty-packages").resolve()
        require(not inside(output, root / "dist"), "dist is not a local preview destination; use firmware-inbox/work or another new directory")
        builds = {"x86_64": args.x86_64_build.resolve(), "aarch64": args.aarch64_build.resolve()}
        require(builds["x86_64"] != builds["aarch64"], "Architecture build directories must differ")
        require(not inside(output, root / "firmware-inbox/input")
                and all(not inside(output, build) for build in builds.values()),
                "Output cannot be an original-input or build directory")
        plans = prepare(root, builds, args.verification.resolve(), args.preview, args.release)
        if args.check_only:
            print("Validated " + ", ".join(plan["stem"] for plan in plans) + "; no files written")
        else:
            for name in write_packages(plans, output):
                print(output / name)
        return 0
    except (PackageError, OSError, UnicodeError) as exc:
        print(f"Packaging refused: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
