# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
from pathlib import Path
from typing import Any

from harness.engine.common import first_existing
from harness.engine.common import run_command
from harness.engine.common import which
from harness.engine.common import write_json

#
# 背景：msobjdump 之类的 CANN 工具可能是 x86_64 build 但部署到 aarch64 主机，
# 仅 ``chmod -x`` 是不够的（会误判 permission_error，掩盖架构失配的真正原因）。
# 这里先尝试 ``file`` 命令（Linux/macOS/WSL2 都有），兜底自己读 ELF / PE
# magic byte，识别 32/64 位 + CPU 架构。

_ELF_E_MACHINE_MAP: dict[int, str] = {
    0x03: "i386",
    0x28: "arm",
    0x3E: "x86_64",
    0xB7: "aarch64",
    0xF3: "riscv",
}

# PE COFF Machine codes（IMAGE_FILE_MACHINE_*）。
_PE_MACHINE_MAP: dict[int, str] = {
    0x014C: "i386",
    0x0200: "ia64",
    0x8664: "x86_64",
    0x01C0: "arm",
    0xAA64: "aarch64",
}


def _detect_arch_via_file_cmd(path: Path) -> str | None:
    """如果系统有 ``file`` 命令，调用它取一行简要描述。"""
    file_cmd = shutil.which("file")
    if not file_cmd:
        return None
    res = run_command([file_cmd, str(path)], timeout=5)
    if res.get("returncode") != 0:
        return None
    out = (res.get("output") or "").strip()
    return out or None


def _parse_arch_from_file_output(text: str) -> dict[str, Any]:
    lower = text.lower()
    arch = "unknown"
    bits: int | None = None
    fmt = "unknown"
    if "elf" in lower:
        fmt = "ELF"
    elif "pe32" in lower or "ms-dos" in lower or "ms windows" in lower:
        fmt = "PE"
    elif "mach-o" in lower:
        fmt = "Mach-O"
    elif "script" in lower or "shell script" in lower or "python" in lower:
        fmt = "script"
    for needle, value in (
        ("x86-64", "x86_64"),
        ("x86_64", "x86_64"),
        ("aarch64", "aarch64"),
        ("arm64", "aarch64"),
        ("intel 80386", "i386"),
        ("intel i386", "i386"),
        ("arm,", "arm"),
        ("riscv", "riscv"),
    ):
        if needle in lower:
            arch = value
            break
    if "32-bit" in lower:
        bits = 32
    elif "64-bit" in lower:
        bits = 64
    # 从 arch 推断 bits（"PE32+ executable x86-64" 没显式标 64-bit 时兜底）
    if bits is None:
        if arch in ("x86_64", "aarch64", "ia64"):
            bits = 64
        elif arch in ("i386", "arm"):
            bits = 32
    return {"format": fmt, "arch": arch, "bits": bits, "source": "file", "raw": text}


def _detect_arch_via_magic(path: Path) -> dict[str, Any] | None:
    """读前 64 字节识别 ELF / PE / Mach-O。读不到或非已知格式时返回 None。"""
    try:
        with open(path, "rb") as fh:
            head = fh.read(4096)
    except OSError:
        return None
    if not head:
        return None

    # Shebang script
    if head[:2] == b"#!":
        first_line = head.split(b"\n", 1)[0].decode("utf-8", errors="replace")
        return {
            "format": "script",
            "arch": "unknown",
            "bits": None,
            "source": "magic",
            "raw": first_line,
        }

    # ELF: \x7fELF
    if head[:4] == b"\x7fELF" and len(head) >= 20:
        ei_class = head[4]
        ei_data = head[5]
        endian = "<" if ei_data == 1 else ">"
        try:
            e_machine = struct.unpack(endian + "H", head[18:20])[0]
        except struct.error:
            e_machine = 0
        arch = _ELF_E_MACHINE_MAP.get(e_machine, f"elf_machine_0x{e_machine:x}")
        bits = 32 if ei_class == 1 else 64 if ei_class == 2 else None
        return {
            "format": "ELF",
            "arch": arch,
            "bits": bits,
            "source": "magic",
            "raw": f"ELF e_machine=0x{e_machine:x}",
        }

    # PE: starts with MZ, PE header offset at 0x3C
    if head[:2] == b"MZ" and len(head) >= 0x40:
        try:
            e_lfanew = struct.unpack("<I", head[0x3C:0x40])[0]
        except struct.error:
            return None
        # 已读 4096 字节，多数 PE header 都在前 1KB 内
        if e_lfanew + 6 <= len(head) and head[e_lfanew : e_lfanew + 4] == b"PE\x00\x00":
            try:
                machine = struct.unpack("<H", head[e_lfanew + 4 : e_lfanew + 6])[0]
            except struct.error:
                machine = 0
            arch = _PE_MACHINE_MAP.get(machine, f"pe_machine_0x{machine:x}")
            bits = (
                64
                if arch in {"x86_64", "aarch64", "ia64"}
                else 32
                if arch in {"i386", "arm"}
                else None
            )
            return {
                "format": "PE",
                "arch": arch,
                "bits": bits,
                "source": "magic",
                "raw": f"PE Machine=0x{machine:x}",
            }
        # 老式 DOS exe
        return {
            "format": "PE",
            "arch": "i386",
            "bits": 16,
            "source": "magic",
            "raw": "MZ legacy",
        }

    # Mach-O thin / fat
    if head[:4] in (
        b"\xfe\xed\xfa\xce",
        b"\xfe\xed\xfa\xcf",
        b"\xce\xfa\xed\xfe",
        b"\xcf\xfa\xed\xfe",
    ):
        bits = 64 if head[:4] in (b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe") else 32
        return {
            "format": "Mach-O",
            "arch": "unknown",
            "bits": bits,
            "source": "magic",
            "raw": "Mach-O thin",
        }
    if head[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        return {
            "format": "Mach-O",
            "arch": "universal",
            "bits": None,
            "source": "magic",
            "raw": "Mach-O fat",
        }

    return None


def detect_binary_arch(path: str | Path) -> dict[str, Any]:
    """返回 ``{"format", "arch", "bits", "source", "raw"}``。

    - 优先调系统 ``file``；不可用 / 失败时退到 ELF/PE magic byte 解析。
    - 文件不存在 / 不可读时返回 ``{format=unknown, ...}``。
    - **永不抛异常**：上层 summarize_native_tools 期望幂等。
    """
    p = Path(path) if isinstance(path, str) else path
    default = {
        "format": "unknown",
        "arch": "unknown",
        "bits": None,
        "source": "none",
        "raw": "",
    }
    if not p or not p.exists() or not p.is_file():
        default["raw"] = f"missing: {p}"
        return default
    via_file = _detect_arch_via_file_cmd(p)
    if via_file:
        parsed = _parse_arch_from_file_output(via_file)
        if parsed["arch"] != "unknown" or parsed["format"] != "unknown":
            return parsed
    via_magic = _detect_arch_via_magic(p)
    if via_magic:
        return via_magic
    if via_file:
        return _parse_arch_from_file_output(via_file)
    return default


def host_arch_summary() -> dict[str, str]:
    """主机自身架构，与 detect_binary_arch 结果对比用。"""
    import platform

    machine = platform.machine() or "unknown"
    # 归一化：x86_64/amd64/aarch64/arm64
    norm = {
        "AMD64": "x86_64",
        "amd64": "x86_64",
        "X86_64": "x86_64",
        "ARM64": "aarch64",
        "arm64": "aarch64",
    }.get(machine, machine.lower())
    return {"machine": machine, "normalized": norm, "system": platform.system()}


TOOL_NAMES = [
    "cmake",
    "ninja",
    "make",
    "gcc",
    "g++",
    "python3",
    "msopgen",
    "msOpGen",
    "msopst",
    "msOpST",
    "msobjdump",
    "ccec",
    "bisheng",
    "clang-tidy",
    "cppcheck",
    "scan-build",
    "creduce",
    "cvise",
]


def read_cann_metadata(install_dir: Path) -> dict[str, Any]:
    """Read supported metadata layouts without guessing versions from directories."""
    observations = []
    for relative in (
        "version.info",
        "share/info/asc-devkit/version.info",
        "aarch64-linux/ascend_toolkit_install.info",
        "x86_64-linux/ascend_toolkit_install.info",
        "ascend_toolkit_install.info",
        "compiler/ascend_install.info",
    ):
        info = install_dir / relative
        if not info.is_file():
            continue
        try:
            text = info.read_text(encoding="utf-8", errors="replace")
        except OSError:
            observations.append(
                {"path": str(info), "version": None, "status": "unreadable"}
            )
            continue
        match = re.search(r"(?im)^\s*(?:cann[_ ]?)?version\s*[:=]\s*([^\s]+)", text)
        if match:
            observations.append(
                {"path": str(info), "version": match.group(1), "status": "read"}
            )
    versions = sorted({x["version"] for x in observations if x.get("version")})
    return {
        "version": versions[0] if len(versions) == 1 else "unknown",
        "version_status": "conflict"
        if len(versions) > 1
        else "observed"
        if versions
        else "unknown",
        "version_observations": observations,
    }


def detect_cann() -> dict[str, Any]:
    candidates = [
        os.environ.get("ASCEND_CANN_PACKAGE_PATH"),
        os.environ.get("ASCEND_TOOLKIT_HOME"),
        os.environ.get("ASCEND_HOME_PATH"),
        "/usr/local/Ascend/ascend-toolkit/latest",
        "/usr/local/Ascend/cann/latest",
        "/usr/local/Ascend/latest",
    ]
    install_dir = first_existing(candidates)
    metadata = (
        read_cann_metadata(install_dir)
        if install_dir
        else {
            "version": "unknown",
            "version_status": "unknown",
            "version_observations": [],
        }
    )
    return {
        "found": bool(install_dir),
        "install_dir": str(install_dir) if install_dir else None,
        **metadata,
    }


def collect_tools() -> dict[str, str | None]:
    tools: dict[str, str | None] = {}
    for name in TOOL_NAMES:
        key = {
            "msOpGen": "msopgen_alt",
            "msOpST": "msopst_alt",
        }.get(name, name)
        tools[key] = which(name)
    if not tools.get("msopgen") and tools.get("msopgen_alt"):
        tools["msopgen"] = tools["msopgen_alt"]
    if not tools.get("msopst") and tools.get("msopst_alt"):
        tools["msopst"] = tools["msopst_alt"]
    return tools


def summarize_native_tools(
    tools: dict[str, str | None], help_excerpt: dict[str, Any] | None = None
) -> dict[str, Any]:
    help_excerpt = help_excerpt or {}
    host = host_arch_summary()
    native: dict[str, Any] = {}
    for tool in ["msopgen", "msopst", "msobjdump", "ccec", "bisheng"]:
        path = tools.get(tool)
        help_info = help_excerpt.get(tool) or {}
        status = "missing"
        reason = "not found"
        arch_info: dict[str, Any] | None = None
        if path:
            status = "available"
            reason = "found"
            arch_info = detect_binary_arch(path)
            if help_info:
                rc = help_info.get("returncode")
                if rc == 0:
                    status = "available"
                    reason = "help returned 0"
                elif rc == 126:
                    # 不匹配时改回 arch_mismatch，避免上层误判 chmod 修不好。
                    if arch_info and arch_info.get("arch") not in (
                        "unknown",
                        host["normalized"],
                        "universal",
                    ):
                        status = "arch_mismatch"
                        reason = (
                            f"binary arch={arch_info['arch']} bits={arch_info.get('bits')} "
                            f"does not match host {host['normalized']}"
                        )
                    else:
                        status = "permission_error"
                        reason = "help returned 126"
                else:
                    status = "help_failed"
                    reason = f"help returned {rc}"
        entry: dict[str, Any] = {
            "path": path,
            "status": status,
            "reason": reason,
            "help_returncode": help_info.get("returncode") if help_info else None,
        }
        if arch_info is not None:
            entry["arch"] = arch_info
        native[tool] = entry
    native["_host"] = host
    return native


def write_native_tools_report(env_data: dict[str, Any], md_out: Path) -> None:
    native = env_data.get("native_tools") or summarize_native_tools(
        env_data.get("tools") or {}, env_data.get("help") or {}
    )
    host = native.get("_host") or host_arch_summary()
    lines = ["# Ascend Native Tools Report\n\n"]
    cann = env_data.get("cann") or {}
    lines.append(f"- CANN found: `{cann.get('found')}`\n")
    lines.append(f"- CANN path: `{cann.get('install_dir')}`\n")
    lines.append(f"- CANN version: `{cann.get('version')}`\n")
    lines.append(
        f"- Host: `{host.get('system')} / {host.get('machine')} (normalized {host.get('normalized')})`\n\n"
    )
    lines.append("| Tool | Status | Path | Arch | Reason |\n")
    lines.append("| --- | --- | --- | --- | --- |\n")
    for name, info in native.items():
        if name.startswith("_"):
            continue
        arch = info.get("arch") or {}
        if arch:
            bits = arch.get("bits")
            bits_str = f" ({bits}b)" if bits else ""
            arch_cell = f"{arch.get('format', '?')} / {arch.get('arch', '?')}{bits_str}"
        else:
            arch_cell = "—"
        lines.append(
            f"| `{name}` | `{info.get('status')}` | `{info.get('path')}` | `{arch_cell}` | {info.get('reason')} |\n"
        )
    lines.append("\n## Notes\n\n")
    lines.append(
        "- Missing or failing optional native tools do not block compile diagnosis.\n"
    )
    lines.append(
        "- Tool arguments must come from local `--help` output or config files.\n"
    )
    lines.append(
        "- `arch_mismatch` status means the binary's CPU arch differs from the host; reinstall the matching CANN package instead of `chmod +x`.\n"
    )
    md_out.parent.mkdir(parents=True, exist_ok=True)
    md_out.write_text("".join(lines), encoding="utf-8")


def probe_npu() -> dict[str, Any]:
    npu_smi = which("npu-smi")
    result: dict[str, Any] = {
        "npu_smi_available": bool(npu_smi),
        "npu_smi": npu_smi,
        "devices": [],
    }
    if npu_smi:
        cmd = run_command([npu_smi, "info"], timeout=20)
        result["info_returncode"] = cmd["returncode"]
        result["info_excerpt"] = "\n".join((cmd.get("output") or "").splitlines()[:80])
        for line in (cmd.get("output") or "").splitlines():
            if re.search(
                r"\b[0-9]+\s+\S+.*(OK|Normal|Health|910|310|A2|A3|950)",
                line,
                re.IGNORECASE,
            ):
                result["devices"].append(line.strip())
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Probe local CANN and Ascend toolchain."
    )
    parser.add_argument("--json-out", required=True)
    parser.add_argument(
        "--include-help",
        action="store_true",
        help="Capture short --help excerpts for CANN tools.",
    )
    parser.add_argument(
        "--native-tools-report",
        help="Optional Markdown report path for native Ascend tool status.",
    )
    args = parser.parse_args()

    warnings: list[str] = []
    cann = detect_cann()
    tools = collect_tools()
    if not cann["found"]:
        warnings.append(
            "No CANN install directory found in common locations or environment variables."
        )
    for required in ["cmake", "make", "gcc", "g++", "python3"]:
        if not tools.get(required):
            warnings.append(f"Required build tool not found: {required}")
    for optional in ["msopgen", "msopst", "msobjdump", "ccec", "bisheng"]:
        if not tools.get(optional):
            warnings.append(f"Optional Ascend tool not found: {optional}")

    help_excerpt: dict[str, Any] = {}
    if args.include_help:
        for tool in ["msopgen", "msopst", "msobjdump", "ccec", "bisheng"]:
            path = tools.get(tool)
            if path:
                res = run_command([path, "--help"], timeout=15)
                help_excerpt[tool] = {
                    "returncode": res["returncode"],
                    "excerpt": "\n".join((res.get("output") or "").splitlines()[:60]),
                }

    env_keys = [
        "ASCEND_CANN_PACKAGE_PATH",
        "ASCEND_TOOLKIT_HOME",
        "ASCEND_HOME_PATH",
        "CMAKE_PREFIX_PATH",
        "LD_LIBRARY_PATH",
        "PYTHONPATH",
        "SOC_VERSION",
        "CMAKE_ASC_ARCHITECTURES",
    ]
    data: dict[str, Any] = {
        "cann": cann,
        "tools": tools,
        "native_tools": summarize_native_tools(tools, help_excerpt),
        "npu": probe_npu(),
        "env": {k: os.environ.get(k) for k in env_keys if os.environ.get(k)},
        "help": help_excerpt,
        "warnings": warnings,
    }
    write_json(Path(args.json_out), data)
    if args.native_tools_report:
        write_native_tools_report(data, Path(args.native_tools_report))
    print(str(Path(args.json_out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
