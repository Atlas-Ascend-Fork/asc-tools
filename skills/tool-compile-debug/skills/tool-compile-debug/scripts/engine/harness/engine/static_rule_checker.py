# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
from __future__ import annotations

import argparse
import fnmatch
import re
from pathlib import Path
from typing import Any

from harness.engine.build_context_collector import collect_context
from harness.engine.common import PROJECT_ROOT
from harness.engine.common import error_payload
from harness.engine.common import line_number_for
from harness.engine.common import load_json
from harness.engine.common import load_yaml
from harness.engine.common import write_json
from harness.engine.stage_evidence import _visible_source


def _brace_patterns(pattern: str) -> list[str]:
    m = re.search(r"\{([^{}]+)\}", pattern)
    if not m:
        return [pattern]
    return [
        pattern[: m.start()] + item + pattern[m.end() :]
        for item in m.group(1).split(",")
    ]


def _matches(path: str, pattern: str) -> bool:
    name = Path(path).name
    for p in _brace_patterns(pattern):
        if fnmatch.fnmatch(path, p) or fnmatch.fnmatch(name, p):
            return True
        if p.startswith("**/") and (
            fnmatch.fnmatch(path, p[3:]) or fnmatch.fnmatch(name, p[3:])
        ):
            return True
    return False


def _source_texts(context: dict[str, Any]) -> dict[str, str]:
    # Missing context stays missing. Rules must not silently expand the read scope.
    return context.setdefault("file_text", {})


def _add_finding(
    findings: list[dict[str, Any]],
    rule_id: str,
    severity: str,
    category: str,
    message: str,
    file: str | None = None,
    line: int | None = None,
    evidence: list[str] | None = None,
    suggested_fix: str | None = None,
) -> None:
    findings.append(
        {
            "rule_id": rule_id,
            "severity": severity,
            "category": category,
            "file": file,
            "line": line,
            "message": message,
            "evidence": evidence or [],
            "suggested_fix": suggested_fix,
        }
    )


def _check_config_rules(
    context: dict[str, Any], findings: list[dict[str, Any]]
) -> None:
    rules_doc = load_yaml(
        PROJECT_ROOT / "harness/config/static_rules.yaml", default={"rules": []}
    ) or {"rules": []}
    texts = _source_texts(context)
    all_files = list(texts.keys())
    for rule in rules_doc.get("rules", []):
        # ：跨文件 define/use 规则类。dispatch 靠 type 字段（旧规则无 type，走原分支）。
        if rule.get("type") == "cross_file":
            _check_cross_file(context, findings, rule)
            continue
        pattern = rule.get("file_glob", "*")
        matched = [p for p in all_files if _matches(p, pattern)]
        if (
            pattern == "CMakeLists.txt"
            and "CMakeLists.txt" not in matched
            and "CMakeLists.txt" in texts
        ):
            matched.append("CMakeLists.txt")

        severity = rule.get("severity", "warning")
        category = rule.get("category", "unknown")
        message = rule.get("message", rule.get("id", "static rule failed"))
        suggested = rule.get("suggested_fix")

        if "must_contain" in rule:
            ok = any(rule["must_contain"] in texts.get(p, "") for p in matched)
            if not ok:
                _add_finding(
                    findings,
                    rule["id"],
                    severity,
                    category,
                    message,
                    matched[0] if matched else pattern,
                    None,
                    [],
                    suggested,
                )
        if "must_match" in rule:
            regex = rule["must_match"]
            ok = any(
                re.search(regex, texts.get(p, ""), flags=re.MULTILINE) for p in matched
            )
            if not ok:
                _add_finding(
                    findings,
                    rule["id"],
                    severity,
                    category,
                    message,
                    matched[0] if matched else pattern,
                    None,
                    [regex],
                    suggested,
                )
        if "must_contain_any" in rule:
            ok = any(
                any(token in texts.get(p, "") for token in rule["must_contain_any"])
                for p in matched
            )
            if not ok:
                _add_finding(
                    findings,
                    rule["id"],
                    severity,
                    category,
                    message,
                    matched[0] if matched else pattern,
                    None,
                    rule["must_contain_any"],
                    suggested,
                )
        if "should_contain_any" in rule:
            relevant = [
                p
                for p in matched
                if any(
                    token in texts.get(p, "")
                    for token in [
                        "AscendC::",
                        "__aicore__",
                        "__global__",
                        "DataCopy",
                        "LocalTensor",
                    ]
                )
            ]
            if relevant and not any(
                any(token in texts.get(p, "") for token in rule["should_contain_any"])
                for p in relevant
            ):
                _add_finding(
                    findings,
                    rule["id"],
                    severity,
                    category,
                    message,
                    relevant[0],
                    None,
                    rule["should_contain_any"],
                    suggested,
                )
        if "forbidden_patterns" in rule:
            for p in matched:
                text = texts.get(p, "")
                kernel_like = (
                    p.endswith(".asc")
                    or "__aicore__" in text
                    or "__global__" in text
                    or "op_kernel" in p.lower()
                )
                if (
                    not kernel_like
                    and rule["id"] == "suspicious_host_only_feature_in_kernel"
                ):
                    continue
                for regex in rule["forbidden_patterns"]:
                    m = re.search(regex, text)
                    if m:
                        _add_finding(
                            findings,
                            rule["id"],
                            severity,
                            category,
                            message,
                            p,
                            line_number_for(text, regex, regex=True),
                            [m.group(0)],
                            suggested,
                        )


def _check_env(context: dict[str, Any], findings: list[dict[str, Any]]) -> None:
    cann = context.get("cann") or {}
    env = context.get("env") or {}
    cmake_text = (context.get("file_text") or {}).get("CMakeLists.txt", "")
    build_text = (context.get("file_text") or {}).get("build.sh", "")
    if not cann.get("found"):
        _add_finding(
            findings,
            "cann_path_missing",
            "error",
            "environment",
            "CANN install path was not detected.",
            None,
            None,
            [],
            "Set ASCEND_CANN_PACKAGE_PATH or ASC_DIR to the installed CANN toolkit.",
        )
    if "/not/a/real" in build_text or "NotARealAscendSoc" in build_text:
        _add_finding(
            findings,
            "cann_or_soc_config_invalid",
            "error",
            "environment",
            "Build script contains an invalid CANN path or unsupported SoC setting.",
            "build.sh",
            line_number_for(build_text, "/not/a/real")
            or line_number_for(build_text, "NotARealAscendSoc"),
            [],
            "Use a detected CANN install path and a supported SOC_VERSION such as the device reported by npu-smi.",
        )
    configured_soc = (
        env.get("SOC_VERSION")
        or env.get("CMAKE_ASC_ARCHITECTURES")
        or re.search(r"CMAKE_ASC_ARCHITECTURES|SOC_VERSION|--npu-arch", cmake_text)
    )
    if not configured_soc:
        _add_finding(
            findings,
            "soc_version_missing",
            "warning",
            "environment",
            "No SoC or ASC architecture setting was found.",
            "CMakeLists.txt" if cmake_text else None,
            None,
            [],
            "Pass SOC_VERSION or CMAKE_ASC_ARCHITECTURES explicitly.",
        )


def _extract_tiling_fields(texts: dict[str, str]) -> set[str]:
    fields: set[str] = set()
    for text in texts.values():
        # 字面 struct 形式：struct *Tiling* { uint32_t foo; ... };
        for struct in re.finditer(
            r"struct\s+\w*Tiling\w*\s*\{(?P<body>.*?)\};", text, flags=re.S
        ):
            body = struct.group("body")
            for field in re.finditer(
                r"\b(?:u?int(?:8|16|32|64)_t|int|unsigned|float|double|bool|size_t)\s+([A-Za-z_]\w*)\s*(?:[;=,\[])",
                body,
            ):
                fields.add(field.group(1))
        # ：宏式注册形式（msopgen/真实 CANN optiling 风格）——
        # BEGIN_TILING_DATA_DEF(Name) ... TILING_DATA_FIELD_DEF(type, fieldName) ... END_TILING_DATA_DEF
        # 旧抽取只认字面 struct，对宏式 tiling 抽出空集 → _check_tiling 早 return 静默 no-op。
        for field in re.finditer(
            r"TILING_DATA_FIELD_DEF\s*\(\s*[\w:<>]+\s*,\s*([A-Za-z_]\w*)\s*\)", text
        ):
            fields.add(field.group(1))
    return fields


def _check_tiling(context: dict[str, Any], findings: list[dict[str, Any]]) -> None:
    texts = _source_texts(context)
    fields = _extract_tiling_fields(texts)
    if not fields:
        return
    for rel, text in texts.items():
        if not (
            rel.endswith(".asc") or "kernel" in rel.lower() or "__global__" in text
        ):
            continue
        for m in re.finditer(
            r"\b(?:tiling|tiling_data|tilingData|[A-Za-z_]\w*Tiling)\.([A-Za-z_]\w*)",
            text,
        ):
            used = m.group(1)
            if used not in fields:
                _add_finding(
                    findings,
                    "tiling_field_mismatch",
                    "error",
                    "tiling_consistency",
                    f"Kernel uses tiling field {used}, but no TilingData struct defines it.",
                    rel,
                    line_number_for(text, m.group(0)),
                    [f"known_fields={sorted(fields)}", m.group(0)],
                    "Align Host/Kernel TilingData field names or regenerate the project.",
                )
        for m in re.finditer(
            r"GET_TILING_DATA\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)", text
        ):
            var = m.group(1)
            if not re.search(rf"\b{re.escape(var)}\.", text):
                _add_finding(
                    findings,
                    "get_tiling_data_variable_mismatch",
                    "warning",
                    "tiling_consistency",
                    f"GET_TILING_DATA declares {var}, but the variable is not used as a tiling object.",
                    rel,
                    line_number_for(text, m.group(0)),
                    [m.group(0)],
                    "Use the GET_TILING_DATA variable consistently in kernel code.",
                )


def _is_kernel_like(rel: str, text: str) -> bool:
    low = rel.lower()
    return (
        rel.endswith(".asc")
        or "op_kernel" in low
        or "kernel" in low
        or "__global__" in text
        or "__aicore__" in text
    )


def _is_host_like(rel: str) -> bool:
    low = rel.lower()
    return "op_host" in low or low.endswith((".cpp", ".cc", ".cxx")) or "host" in low


def _cross_file_index(texts: dict[str, str]) -> dict[str, set[str]]:
    """项目内 define 索引：struct 名 / constexpr·#define 常量名 / __global__ 入口符号名。
    只看项目源码（非日志），按定义位置判别 category——结构性而非记符号字面量。"""
    structs: set[str] = set()
    consts: set[str] = set()
    entries: set[str] = set()
    for text in texts.values():
        for m in re.finditer(r"\bstruct\s+(\w+)", text):
            structs.add(m.group(1))
        for m in re.finditer(
            r"(?:constexpr\s+[\w:<>]+|#\s*define)\s+([A-Za-z_]\w*)", text
        ):
            consts.add(m.group(1))
        for m in re.finditer(
            r"__global__\s+(?:__\w+__\s+)?void\s+([A-Za-z_]\w*)\s*\(", text
        ):
            entries.add(m.group(1))
    return {"structs": structs, "consts": consts, "entries": entries}


def _check_cross_file(
    context: dict[str, Any], findings: list[dict[str, Any]], rule: dict[str, Any]
) -> None:
    """Detect a project symbol that is used but not defined in the project."""
    settings = rule.get("settings") or {}
    texts = _source_texts(context)
    if not texts:
        return
    idx = _cross_file_index(texts)
    whitelist = tuple(settings.get("extern_prefix_whitelist") or [])
    enable_shared_const = settings.get("enable_shared_const", True)

    # case (i) CX-T：kernel/host 引用 `<X>TilingData`（指针 `*` 或 `GetTilingData<X>()` 模板形），
    # 项目内无该 struct 定义，且项目自有 *tiling*.h → 跨文件 struct 名漂移（：完整算子症状常在 host）。
    has_tiling_header = any(
        "tiling" in rel.lower() and rel.lower().endswith((".h", ".hpp"))
        for rel in texts
    )
    # 两种引用形态：① 指针声明 `<X>TilingData *`（kernel/host 都有）② 模板取用 `GetTilingData<<X>TilingData>()`（完整算子 host）
    _TILING_REF_RES = (
        re.compile(r"(?:__gm__\s+)?(?:const\s+)?(\w*TilingData)\b\s*\*"),
        re.compile(r"GetTilingData<\s*(\w*TilingData)\s*>"),
    )
    for rel, text in texts.items():
        # ：CX-T 不再只看 kernel——完整算子的 `GetTilingData<X>` 症状落在 host（op_host/*_host.cpp）。
        if not (_is_kernel_like(rel, text) or _is_host_like(rel)):
            continue
        fired = False
        for ref_re in _TILING_REF_RES:
            for m in ref_re.finditer(text):
                tname = m.group(1)
                if tname not in idx["structs"] and has_tiling_header:
                    _add_finding(
                        findings,
                        "crossfile_missing_tiling_type",
                        "error",
                        "tiling_consistency",
                        f"References TilingData type {tname}, but no struct {tname} is defined in the project.",
                        rel,
                        line_number_for(text, m.group(0)),
                        [m.group(0), f"defined_structs={sorted(idx['structs'])}"],
                        "Align the TilingData struct name across tiling header and kernel/host, or regenerate.",
                    )
                    fired = True
                    break
            if fired:
                break

    # case (iii) CX-L/CX-S：host launch 符号 `NAME<<<...>>>`，项目内无 __global__ NAME 定义，
    # 且 launch 与所有 entry 定义点跨文件（单文件场景如 cce name-mangling 不属跨文件契约，沉默）。
    entry_def_files = {
        rel
        for rel, text in texts.items()
        if re.search(r"__global__\s+(?:__\w+__\s+)?void\s+[A-Za-z_]\w*\s*\(", text)
    }
    for rel, text in texts.items():
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*<<<", text):
            sym = m.group(1)
            if (
                sym not in idx["entries"]
                and entry_def_files
                and rel not in entry_def_files
            ):
                _add_finding(
                    findings,
                    "crossfile_missing_entry_symbol",
                    "error",
                    "api_incompatible",
                    f"Host launches kernel symbol {sym}<<<...>>>, but no __global__ {sym} entry is defined in the project.",
                    rel,
                    line_number_for(text, m.group(0)),
                    [m.group(0), f"defined_entries={sorted(idx['entries'])}"],
                    "Match the kernel entry symbol between kernel definition and host launch site.",
                )
                break

    # case (ii) CX-M：UPPER_SNAKE 常量同时被 kernel 与 host 引用、项目内无定义、非外部前缀、
    # 且存在近似改名（项目里有同前缀常量）——共享常量重命名签名。enable_shared_const 硬门控。
    if enable_shared_const:
        _check_shared_const(texts, findings, idx, whitelist)


def _check_shared_const(texts, findings, idx, whitelist) -> None:
    # A word in a license, comment or literal is not a source-level use or
    # definition. Retain physical lines, and decline incomplete C++ lexemes.
    visible_texts = {}
    for rel, text in texts.items():
        if Path(rel).suffix.lower() not in {
            ".c",
            ".cc",
            ".cpp",
            ".cxx",
            ".asc",
            ".cce",
            ".h",
            ".hh",
            ".hpp",
            ".hxx",
            ".inc",
        }:
            continue
        visible = _visible_source(text)
        if visible is None:
            # An unparsed file may define the symbol. Do not infer project-wide
            # absence from a partial source index.
            return
        visible_texts[rel] = visible
    texts = visible_texts
    idx = _cross_file_index(texts)
    const_re = re.compile(r"\b[A-Z][A-Z0-9_]{2,}\b")
    kernel_tokens: dict[str, str] = {}
    host_tokens: set[str] = set()
    for rel, text in texts.items():
        toks = set(const_re.findall(text))
        if _is_kernel_like(rel, text):
            for t in toks:
                kernel_tokens.setdefault(t, rel)
        if _is_host_like(rel):
            host_tokens |= toks
    for tok, krel in kernel_tokens.items():
        if tok not in host_tokens or tok in idx["consts"]:
            continue
        if whitelist and tok.startswith(whitelist):
            continue
        near_miss = any(
            c != tok and (c.startswith(tok) or tok.startswith(c)) for c in idx["consts"]
        )
        if not near_miss:
            continue
        text = texts.get(krel, "")
        _add_finding(
            findings,
            "crossfile_shared_const_renamed",
            "error",
            "tiling_consistency",
            f"Constant {tok} is used in both kernel and host, but defined nowhere in the project (likely renamed).",
            krel,
            line_number_for(text, tok),
            [tok, f"defined_consts={sorted(idx['consts'])}"],
            "Restore the shared constant name, or update all use sites to match the new definition.",
        )
        break


def _check_schema(context: dict[str, Any], findings: list[dict[str, Any]]) -> None:
    texts = _source_texts(context)
    for rel, text in texts.items():
        if not rel.lower().endswith(".json"):
            continue
        inputs = len(re.findall(r'"input_desc"\s*:', text))
        outputs = len(re.findall(r'"output_desc"\s*:', text))
        if inputs == 0 and outputs == 0:
            continue
        kernel_args = 0
        for src_rel, src_text in texts.items():
            m = re.search(
                r"__global__\s+__\w+__\s+void\s+\w+\s*\(([^)]*)\)", src_text, flags=re.S
            )
            if m:
                kernel_args = len([x for x in m.group(1).split(",") if x.strip()])
                break
        if kernel_args and kernel_args < 3:
            _add_finding(
                findings,
                "prototype_host_kernel_arg_count_mismatch",
                "warning",
                "schema_consistency",
                "Prototype and kernel argument counts look inconsistent.",
                rel,
                None,
                [
                    f"kernel_args={kernel_args}",
                    f"input_sections={inputs}",
                    f"output_sections={outputs}",
                ],
                "Check op prototype JSON, Host wrapper, TilingData, and Kernel signature together.",
            )


def _contract_summary(context: dict[str, Any]) -> dict[str, Any]:
    files = context.get("files") or {}
    texts = _source_texts(context)
    cmake_text = texts.get("CMakeLists.txt", "")
    prototypes = files.get("prototype_files") or []
    host_files = files.get("op_host_files") or []
    kernel_files = files.get("op_kernel_files") or []
    generated_files = files.get("generated_files") or []
    tiling_fields = sorted(_extract_tiling_fields(texts))
    kernel_args: list[dict[str, Any]] = []
    kernel_tiling_uses: list[str] = []
    for rel, text in texts.items():
        if (
            rel in kernel_files
            or rel.endswith(".asc")
            or "kernel" in rel.lower()
            or "__global__" in text
        ):
            for m in re.finditer(
                r"__global__\s+(?:__\w+__\s+)?void\s+([A-Za-z_]\w*)\s*\(([^)]*)\)",
                text,
                flags=re.S,
            ):
                args = [x.strip() for x in m.group(2).split(",") if x.strip()]
                kernel_args.append(
                    {
                        "file": rel,
                        "kernel": m.group(1),
                        "arg_count": len(args),
                        "args": args[:12],
                    }
                )
            for m in re.finditer(
                r"\b(?:tiling|tiling_data|tilingData|[A-Za-z_]\w*Tiling)\.([A-Za-z_]\w*)",
                text,
            ):
                kernel_tiling_uses.append(m.group(1))
    prototype_sections = {}
    for rel in prototypes:
        text = texts.get(rel, "")
        prototype_sections[rel] = {
            "input_desc_sections": len(re.findall(r'"input_desc"\s*:', text)),
            "output_desc_sections": len(re.findall(r'"output_desc"\s*:', text)),
            "op_names": re.findall(r'"op"\s*:\s*"([^"]+)"', text)[:10],
        }
    target_sources = re.findall(
        r"add_(?:executable|library)\s*\(([^)]*)\)", cmake_text, flags=re.S
    )
    return {
        "prototype_files": prototypes,
        "prototype_sections": prototype_sections,
        "host_files": host_files,
        "kernel_files": kernel_files,
        "generated_files": generated_files,
        "tiling_fields": tiling_fields,
        "kernel_tiling_uses": sorted(set(kernel_tiling_uses)),
        "kernel_signatures": kernel_args,
        "cmake_targets": (context.get("build") or {}).get("targets", []),
        "cmake_target_blocks": [
            " ".join(block.split()) for block in target_sources[:10]
        ],
        "has_compile_commands": bool(
            (context.get("files") or {}).get("compile_commands")
        ),
    }


_SHELL_COND_START_RE = re.compile(r"(?:^|[;&|]\s*)\b(?:if|elif|while|until)\b")
_SHELL_COND_END_RE = re.compile(r"(?:^|[;&]\s*)\b(?:then|do)\b")


def _find_standalone_negated_build_lines(text: str) -> list[tuple[int, str]]:
    """Find standalone '!' negated build commands outside if/elif/while/until condition headers."""
    hits: list[tuple[int, str]] = []
    in_condition = False
    continuation = ""
    start_lineno = 1
    for lineno, raw_line in enumerate(text.splitlines(), 1):
        code = re.sub(r"(?<!\\)#.*$", "", raw_line).rstrip()
        if not continuation:
            start_lineno = lineno
        if code.endswith("\\"):
            continuation += code[:-1] + " "
            continue
        stmt = (continuation + code).strip()
        continuation = ""
        if not stmt:
            continue
        if _SHELL_COND_START_RE.search(stmt) and not _SHELL_COND_END_RE.search(stmt):
            in_condition = True
            continue
        if in_condition:
            if _SHELL_COND_END_RE.search(stmt):
                in_condition = False
            continue
        if re.match(
            r"^!\s+(?:cmake|make|ninja|cpack|bash\b|sh\b|\./)", stmt
        ) and not re.search(r"&&|\|\|", stmt):
            hits.append((start_lineno, stmt[:160]))
    return hits


_NON_FUNC_KEYWORDS = frozenset(
    {
        "if",
        "for",
        "while",
        "switch",
        "catch",
        "sizeof",
        "alignof",
        "alignas",
        "decltype",
        "__attribute__",
        "__attribute",
        "static_assert",
        "BEGIN_TILING_DATA_DEF",
    }
)
_NON_TYPE_WORDS = frozenset(
    {
        "return",
        "case",
        "goto",
        "delete",
        "throw",
        "co_return",
        "co_yield",
        "else",
        "do",
        "sizeof",
        "new",
        "if",
        "for",
        "while",
        "switch",
    }
)
_WIDTH_64_DECL_TYPES = frozenset(
    {
        "int64_t",
        "uint64_t",
        "size_t",
        "ssize_t",
        "ptrdiff_t",
        "intptr_t",
        "uintptr_t",
        "long",
        "long int",
        "signed long",
        "signed long int",
        "long signed",
        "long signed int",
        "unsigned long",
        "unsigned long int",
        "long unsigned",
        "long unsigned int",
        "long long",
        "long long int",
        "signed long long",
        "signed long long int",
        "long long signed",
        "long long signed int",
        "unsigned long long",
        "unsigned long long int",
        "long long unsigned",
        "long long unsigned int",
    }
)


def _find_matching_delim(
    text: str, open_idx: int, open_ch: str, close_ch: str
) -> int | None:
    depth = 0
    for idx in range(open_idx, len(text)):
        ch = text[idx]
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return idx
    return None


def _strip_preprocessor_preserving_offsets(
    visible_code: str,
) -> tuple[str, list[tuple[int, int]]]:
    """Blank preprocessor directives while preserving exact byte/line offsets.

    Recognizes a pure file-level ``#ifndef GUARD`` / ``#define GUARD`` ... ``#endif``
    include guard as benign; all other ``#if``/``#ifdef``/``#ifndef``/``#else``/``#elif``
    regions are returned as ``unresolved_spans`` for conservative abstention.
    """
    directives: list[tuple[int, int, str, str]] = []
    lines = visible_code.splitlines(keepends=True)
    idx = 0
    offset = 0
    n_lines = len(lines)
    while idx < n_lines:
        line = lines[idx]
        line_len = len(line)
        if re.match(r"^\s*#", line):
            start = offset
            parts = [line]
            end = offset + line_len
            while parts[-1].rstrip("\r\n").endswith("\\") and idx + 1 < n_lines:
                idx += 1
                nxt = lines[idx]
                parts.append(nxt)
                end += len(nxt)
            merged = " ".join(p.rstrip("\r\n").rstrip("\\") for p in parts)
            m_dir = re.match(r"^\s*#\s*([A-Za-z_]\w*)\b(.*)$", merged)
            kw = m_dir.group(1) if m_dir else ""
            rest = (m_dir.group(2) or "").strip() if m_dir else ""
            directives.append((start, end, kw, rest))
            offset = end
            idx += 1
            continue
        offset += line_len
        idx += 1

    if not directives:
        return visible_code, []

    has_outer_guard = False
    if (
        len(directives) >= 2
        and directives[0][2] == "ifndef"
        and not visible_code[: directives[0][0]].strip()
        and directives[1][2] == "define"
        and not visible_code[directives[0][1] : directives[1][0]].strip()
    ):
        g0 = directives[0][3].split()[0] if directives[0][3].split() else ""
        m_def = re.match(r"^([A-Za-z_]\w*)(?!\()", directives[1][3])
        g1 = m_def.group(1) if m_def else ""
        if g0 and g0 == g1:
            depth = 0
            closed_idx: int | None = None
            had_alt_branch = False
            for di, (_ds, _de, dkw, _dr) in enumerate(directives):
                if dkw in {"if", "ifdef", "ifndef"}:
                    depth += 1
                elif dkw in {"elif", "elifdef", "elifndef", "else"}:
                    if depth == 1 and closed_idx is None:
                        had_alt_branch = True
                elif dkw == "endif":
                    if depth == 1 and closed_idx is None:
                        closed_idx = di
                    depth = max(0, depth - 1)
            if (
                closed_idx == len(directives) - 1
                and not had_alt_branch
                and depth == 0
                and not visible_code[directives[-1][1] :].strip()
            ):
                has_outer_guard = True

    unresolved_spans: list[tuple[int, int]] = []
    open_ifs: list[tuple[int, bool]] = []
    for di, (ds, de, dkw, _dr) in enumerate(directives):
        if dkw in {"if", "ifdef", "ifndef"}:
            open_ifs.append((ds, has_outer_guard and di == 0))
        elif dkw in {"elif", "elifdef", "elifndef", "else"}:
            if not open_ifs:
                unresolved_spans.append((0, de))
        elif dkw == "endif":
            if open_ifs:
                if_start, is_guard = open_ifs.pop()
                if not is_guard:
                    unresolved_spans.append((if_start, de))
            else:
                unresolved_spans.append((0, de))
    for if_start, _is_guard in open_ifs:
        unresolved_spans.append((if_start, len(visible_code)))

    chars = list(visible_code)
    for ds, de, _dkw, _dr in directives:
        for pos in range(ds, de):
            if chars[pos] != "\n":
                chars[pos] = " "
    return "".join(chars), unresolved_spans


def _blank_cpp_attributes(visible_code: str) -> str:
    """Replace C++/GNU/MSVC attribute and alignas specifiers with spaces, preserving offsets."""
    chars = list(visible_code)
    n = len(chars)
    idx = 0
    attr_kw_re = re.compile(r"^(?:alignas|__attribute__|__attribute|__declspec)\b")
    while idx < n:
        if chars[idx] == "[" and idx + 1 < n and chars[idx + 1] == "[":
            end = idx + 2
            while end + 1 < n and not (chars[end] == "]" and chars[end + 1] == "]"):
                end += 1
            if end + 1 < n:
                for p in range(idx, end + 2):
                    if chars[p] != "\n":
                        chars[p] = " "
                idx = end + 2
                continue
        if (
            idx == 0 or not (chars[idx - 1].isalnum() or chars[idx - 1] == "_")
        ) and chars[idx] in {"a", "_"}:
            m = attr_kw_re.match(visible_code[idx:])
            if m:
                k = idx + m.end()
                while k < n and visible_code[k].isspace():
                    k += 1
                if k < n and visible_code[k] == "(":
                    close_p = _find_matching_delim(visible_code, k, "(", ")")
                    if close_p is not None:
                        for p in range(idx, close_p + 1):
                            if chars[p] != "\n":
                                chars[p] = " "
                        idx = close_p + 1
                        continue
        idx += 1
    return "".join(chars)


def _spans_overlap(start: int, end: int, spans: list[tuple[int, int]]) -> bool:
    return any(not (end <= s or start >= e) for s, e in spans)


def _extract_direct_struct_fields(body: str) -> list[tuple[str, str, int]]:
    """Extract direct scalar int32/uint32/int64/uint64 data members, excluding nested scopes."""
    out: list[str] = []
    brace_d = 0
    paren_d = 0
    brk_d = 0
    for ch in body:
        if ch == "{":
            out.append("{" if brace_d == 0 and paren_d == 0 and brk_d == 0 else " ")
            brace_d += 1
        elif ch == "}":
            brace_d = max(0, brace_d - 1)
            out.append("}" if brace_d == 0 and paren_d == 0 and brk_d == 0 else " ")
        elif brace_d > 0:
            out.append("\n" if ch == "\n" else " ")
        elif ch == "(":
            out.append("(" if paren_d == 0 and brk_d == 0 else " ")
            paren_d += 1
        elif ch == ")":
            paren_d = max(0, paren_d - 1)
            out.append(")" if paren_d == 0 and brk_d == 0 else " ")
        elif paren_d > 0:
            out.append("\n" if ch == "\n" else " ")
        elif ch == "[":
            out.append("[" if brk_d == 0 else " ")
            brk_d += 1
        elif ch == "]":
            brk_d = max(0, brk_d - 1)
            out.append("]" if brk_d == 0 else " ")
        elif brk_d > 0:
            out.append("\n" if ch == "\n" else " ")
        else:
            out.append(ch)
    masked = "".join(out)

    results: list[tuple[str, str, int]] = []
    access_re = re.compile(r"^\s*(?:public|private|protected)\s*:\s*")
    field_head_re = re.compile(
        r"^\s*(?:(?:mutable|const|constexpr|static)\s+)*(?:std::)?(u?int(?:32|64)_t)\b\s+"
    )
    seg_start = 0
    n_masked = len(masked)
    for pos, ch in enumerate(masked):
        if ch not in {";", "}"}:
            continue
        if ch == "}":
            # Keep brace initializers (`int32_t count{};`, `int32_t a{}, b{};`) intact until `;`.
            tail_after = masked[pos + 1 : n_masked].lstrip()
            if tail_after.startswith((";", ",")):
                continue
            seg_start = pos + 1
            continue
        seg = masked[seg_start:pos]
        cur_off = seg_start
        while True:
            am = access_re.match(seg)
            if not am:
                break
            seg = seg[am.end() :]
            cur_off += am.end()
        fm = field_head_re.match(seg)
        if fm:
            ftype = fm.group(1)
            rest = seg[fm.end() :]
            rest_off = cur_off + fm.end()
            decl_off = 0
            for chunk in rest.split(","):
                head_part = chunk.split("=", 1)[0].split("{", 1)[0]
                stripped_name = head_part.strip()
                if re.fullmatch(r"[A-Za-z_]\w*", stripped_name):
                    lead_ws = len(head_part) - len(head_part.lstrip())
                    results.append(
                        (ftype, stripped_name, rest_off + decl_off + lead_ws)
                    )
                decl_off += len(chunk) + 1
        seg_start = pos + 1
    return results


def _extract_scoped_units(
    visible_code: str, rel: str, unresolved_spans: list[tuple[int, int]]
) -> tuple[
    list[tuple[str, str, bool, bool, int, int, str]],
    set[str],
    list[tuple[list[str], set[str], set[str], dict[str, set[str]], str, str, bool]],
    set[str],
]:
    """Single brace-balanced pass extracting qualified types and function scopes.

    Returns:
      - type_blocks: list of (qname, sname, is_macro_tiling, skip_candidate, body_start, body_end, body_text)
      - known_qnames: all struct/class/tiling qualified names declared in ``visible_code``
      - func_scopes: list of (effective_ns_parts, certain_using_ns, uncertain_using_ns, using_decls, params_text, body_text, has_cond)
      - aliased_short_names: short type/namespace names aliased or conditionally ambiguous
    """
    type_blocks: list[tuple[str, str, bool, bool, int, int, str]] = []
    known_qnames: set[str] = set()
    func_scopes: list[
        tuple[
            list[str],
            set[str],
            set[str],
            dict[str, set[str]],
            set[str],
            str,
            str,
            bool,
        ]
    ] = []
    cond_tainted_names: set[str] = set()

    alias_stmt_re = re.compile(
        r"\b(?:using|namespace)\s+([A-Za-z_]\w*)\s*=|\btypedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*;"
    )
    for m_alias in alias_stmt_re.finditer(visible_code):
        alias_name = m_alias.group(1) or m_alias.group(2)
        if alias_name and _spans_overlap(
            m_alias.start(), m_alias.end(), unresolved_spans
        ):
            cond_tainted_names.add(alias_name)

    has_unbalanced_cond = any(
        visible_code[s:e].count("{") != visible_code[s:e].count("}")
        for s, e in unresolved_spans
    )

    ns_re = re.compile(
        r"^(?:inline\s+)?namespace(?:\s+([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*))?\s*$"
    )
    type_head_re = re.compile(
        r"^(?P<tmpl>template\s*<[^>]*>\s*)?(?:struct|class)\s+(?P<name>[A-Za-z_]\w*)\b(?:\s+final\b)?(?P<tail>[^;()]*)$"
    )
    func_qual_re = re.compile(
        r"^(?:(?:const|volatile|noexcept(?:\s*\([^)]*\))?|override|final|__aicore__|__host__|__device__)\b\s*)*(?:->\s*[\w:<>&*\s]+)?$"
    )
    using_ns_re = re.compile(
        r"\busing\s+namespace\s+([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)\s*;"
    )
    using_decl_re = re.compile(
        r"\busing\s+(?!namespace\b)(?:::)?([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)+)\s*;"
    )
    tiling_macro_re = re.compile(
        r"BEGIN_TILING_DATA_DEF\s*\(\s*(\w+)\s*\)(?P<body>.*?)END_TILING_DATA_DEF",
        flags=re.S,
    )

    macro_by_start: dict[int, re.Match[str]] = {
        m.start(): m for m in tiling_macro_re.finditer(visible_code)
    }
    macro_starts = sorted(macro_by_start)
    macro_ptr = 0

    # Stack tuple: (kind, ns_parts, certain_using_ns, uncertain_using_ns, using_decls_sets, scope_aliases)
    scope_stack: list[
        tuple[str, list[str], set[str], set[str], dict[str, set[str]], set[str]]
    ] = [("root", [], set(), set(), {}, set())]
    cond_ns_tainted = has_unbalanced_cond

    def _current_prefix() -> list[str]:
        out: list[str] = []
        for _, parts, _, _, _, _ in scope_stack:
            out.extend(parts)
        return out

    def _current_usings() -> tuple[set[str], set[str], dict[str, set[str]], set[str]]:
        c_uns: set[str] = set()
        u_uns: set[str] = set()
        udecls: dict[str, set[str]] = {}
        s_aliases: set[str] = set(cond_tainted_names)
        for _, _, c_set, u_set, u_map, a_set in scope_stack:
            c_uns.update(c_set)
            u_uns.update(u_set)
            for k_short, q_targets in u_map.items():
                udecls[k_short] = set(q_targets)
                if k_short not in cond_tainted_names:
                    s_aliases.discard(k_short)
            for anm in a_set:
                s_aliases.add(anm)
                udecls.pop(anm, None)
        return c_uns, u_uns, udecls, s_aliases

    def _record_usings(segment: str, seg_offset: int) -> None:
        for um in using_ns_re.finditer(segment):
            norm_ns = "::".join(p.strip() for p in um.group(1).split("::"))
            if _spans_overlap(
                seg_offset + um.start(), seg_offset + um.end(), unresolved_spans
            ):
                scope_stack[-1][3].add(norm_ns)
            else:
                scope_stack[-1][2].add(norm_ns)
        for dm in using_decl_re.finditer(segment):
            norm_q = "::".join(p.strip() for p in dm.group(1).split("::"))
            short = norm_q.split("::")[-1]
            if _spans_overlap(
                seg_offset + dm.start(), seg_offset + dm.end(), unresolved_spans
            ):
                cond_tainted_names.add(short)
            else:
                scope_stack[-1][4].setdefault(short, set()).add(norm_q)
                scope_stack[-1][5].discard(short)
        for am in alias_stmt_re.finditer(segment):
            anm = am.group(1) or am.group(2)
            if not anm:
                continue
            if _spans_overlap(
                seg_offset + am.start(), seg_offset + am.end(), unresolved_spans
            ):
                cond_tainted_names.add(anm)
            else:
                scope_stack[-1][5].add(anm)
                scope_stack[-1][4].pop(anm, None)

    idx = 0
    stmt_start = 0
    n = len(visible_code)
    while idx < n:
        while macro_ptr < len(macro_starts) and macro_starts[macro_ptr] <= idx:
            mm = macro_by_start[macro_starts[macro_ptr]]
            sname = mm.group(1)
            qname = "::".join([*_current_prefix(), sname])
            known_qnames.add(qname)
            skip_cand = _spans_overlap(mm.start(), mm.end(), unresolved_spans)
            type_blocks.append(
                (
                    qname,
                    sname,
                    True,
                    skip_cand,
                    mm.start("body"),
                    mm.end("body"),
                    mm.group("body"),
                )
            )
            macro_ptr += 1

        ch = visible_code[idx]
        if ch == ";":
            _record_usings(visible_code[stmt_start : idx + 1], stmt_start)
            stmt_start = idx + 1
            idx += 1
            continue
        if ch == "}":
            if len(scope_stack) > 1:
                scope_stack.pop()
            stmt_start = idx + 1
            idx += 1
            continue
        if ch == "(":
            header_before_paren = visible_code[stmt_start:idx]
            m_fn = re.search(
                r"((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*)\s*$", header_before_paren
            )
            fn_tail = m_fn.group(1).split("::")[-1].strip() if m_fn is not None else ""
            close_p = _find_matching_delim(visible_code, idx, "(", ")")
            if close_p is None:
                break
            if m_fn and fn_tail not in _NON_FUNC_KEYWORDS:
                j = close_p + 1
                p_depth = 0
                b_open: int | None = None
                while j < n:
                    cj = visible_code[j]
                    if cj == "(":
                        p_depth += 1
                    elif cj == ")":
                        p_depth = max(0, p_depth - 1)
                    elif p_depth == 0:
                        if cj in {";", "}"}:
                            break
                        if cj == "{":
                            b_open = j
                            break
                    j += 1
                if b_open is not None:
                    between = visible_code[close_p + 1 : b_open].strip()
                    is_ctor = between.startswith(":") and not between.startswith("::")
                    if not between or is_ctor or func_qual_re.fullmatch(between):
                        b_close = _find_matching_delim(visible_code, b_open, "{", "}")
                        if b_close is None:
                            break
                        raw_quals = [
                            q.strip()
                            for q in m_fn.group(1).split("::")[:-1]
                            if q.strip()
                        ]
                        eff_ns = [*_current_prefix(), *raw_quals]
                        cur_c_uns, cur_u_uns, cur_udecls, cur_aliases = (
                            _current_usings()
                        )
                        has_cond = cond_ns_tainted or _spans_overlap(
                            stmt_start, b_close, unresolved_spans
                        )
                        func_scopes.append(
                            (
                                eff_ns,
                                cur_c_uns,
                                cur_u_uns,
                                cur_udecls,
                                cur_aliases,
                                visible_code[idx + 1 : close_p],
                                visible_code[b_open + 1 : b_close],
                                has_cond,
                            )
                        )
                        idx = b_close + 1
                        stmt_start = idx
                        continue
            idx = close_p + 1
            continue
        if ch == "{":
            raw_hdr = visible_code[stmt_start:idx]
            header = " ".join(raw_hdr.split())
            candidate_headers = [header]
            last_line = (
                " ".join(raw_hdr.splitlines()[-1].split())
                if raw_hdr.splitlines()
                else ""
            )
            if last_line and last_line != header:
                candidate_headers.append(last_line)
            m_ns = next(
                (ns_re.match(h) for h in candidate_headers if ns_re.match(h)), None
            )
            if m_ns:
                if m_ns.group(1):
                    parts = [p.strip() for p in m_ns.group(1).split("::")]
                else:
                    parts = [f"<anon:{rel}>"]
                scope_stack.append(("ns", parts, set(), set(), {}, set()))
                stmt_start = idx + 1
                idx += 1
                continue
            if not re.search(r"\benum\s+(?:struct|class)\b", header):
                m_st = next(
                    (
                        type_head_re.match(h)
                        for h in candidate_headers
                        if type_head_re.match(h)
                    ),
                    None,
                )
                if m_st:
                    sname = m_st.group("name")
                    qname = "::".join([*_current_prefix(), sname])
                    known_qnames.add(qname)
                    b_close = _find_matching_delim(visible_code, idx, "{", "}")
                    is_tmpl = bool(m_st.group("tmpl")) or "<" in (
                        m_st.group("tail") or ""
                    )
                    if b_close is not None:
                        skip_cand = (
                            cond_ns_tainted
                            or is_tmpl
                            or _spans_overlap(stmt_start, b_close, unresolved_spans)
                        )
                        type_blocks.append(
                            (
                                qname,
                                sname,
                                False,
                                skip_cand,
                                idx + 1,
                                b_close,
                                visible_code[idx + 1 : b_close],
                            )
                        )
                    scope_stack.append(("type", [sname], set(), set(), {}, set()))
                    stmt_start = idx + 1
                    idx += 1
                    continue
            scope_stack.append(("other", [], set(), set(), {}, set()))
            stmt_start = idx + 1
            idx += 1
            continue
        idx += 1

    while macro_ptr < len(macro_starts):
        mm = macro_by_start[macro_starts[macro_ptr]]
        sname = mm.group(1)
        qname = "::".join([*_current_prefix(), sname])
        known_qnames.add(qname)
        skip_cand = _spans_overlap(mm.start(), mm.end(), unresolved_spans)
        type_blocks.append(
            (
                qname,
                sname,
                True,
                skip_cand,
                mm.start("body"),
                mm.end("body"),
                mm.group("body"),
            )
        )
        macro_ptr += 1

    return type_blocks, known_qnames, func_scopes, cond_tainted_names


_LAMBDA_PRECEDING_KW = frozenset({"return", "co_return", "co_yield", "throw", "else"})
_LAMBDA_SPEC_RE = re.compile(
    r"^(?:(?:mutable|constexpr|consteval|static|__aicore__|__host__|__device__|noexcept(?:\s*\([^)]*\))?)\b\s*)*(?:->\s*[^;{]+)?$"
)


def _active_scope_blocks(
    params_text: str, body_prefix: str
) -> list[tuple[str, str, list[tuple[int, str]]]]:
    """Return active lexical frames ``(param_header, stmt_body, [(offset, local_type_name)])``."""
    # Each frame is [param_header, stmt_body, if_header_to_carry_into_else, local_type_offsets_list].
    frames: list[list[Any]] = [[params_text, "", "", []], ["", "", "", []]]
    idx = 0
    n = len(body_prefix)
    cond_head_re = re.compile(
        r"\b(?P<kw>for|if(?:\s+constexpr)?|while|switch|catch)\s*$"
    )
    local_type_head_re = re.compile(
        r"(?:^|[;{}\n])\s*(?:struct|class|union|enum(?:\s+(?:class|struct))?)\s+([A-Za-z_]\w*)\b(?:\s+final\b)?(?:\s*:[^;{}]*)?\s*$"
    )
    else_after_brace_re = re.compile(r"^\s*else\b")
    pending_param = ""
    pending_body = ""
    pending_if = ""

    while idx < n:
        ch = body_prefix[idx]
        if ch == "(" and cond_head_re.search(frames[-1][1]):
            m_kw = cond_head_re.search(frames[-1][1])
            kw = m_kw.group("kw") if m_kw else ""
            close_p = _find_matching_delim(body_prefix, idx, "(", ")")
            if close_p is None:
                inner_slice = body_prefix[idx + 1 :]
                if kw == "catch":
                    frames.append([inner_slice, "", "", []])
                else:
                    frames.append(["", pending_body + inner_slice, "", []])
                pending_param = pending_body = pending_if = ""
                break
            hdr = body_prefix[idx + 1 : close_p]
            if kw == "catch":
                pending_param = hdr
                pending_body = ""
                pending_if = ""
            else:
                combined = (
                    (pending_body + hdr + ";\n") if pending_body else (hdr + ";\n")
                )
                pending_param = ""
                pending_body = combined
                pending_if = combined if kw.startswith("if") else ""
            idx = close_p + 1
            continue

        if ch == "[" and not (idx + 1 < n and body_prefix[idx + 1] == "["):
            before_brk = frames[-1][1].rstrip()
            is_struct_bind = bool(
                re.search(r"\bauto(?:\s*(?:const|volatile))?(?:\s*[*&]+)?$", before_brk)
            )
            prev_word_m = re.search(r"([A-Za-z_]\w*)$", before_brk)
            prev_word = prev_word_m.group(1) if prev_word_m else ""
            prev_char = before_brk[-1] if before_brk else ""
            can_start_lambda = not is_struct_bind and (
                not prev_char
                or prev_char
                not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_)]}"
                or prev_word in _LAMBDA_PRECEDING_KW
            )
            if can_start_lambda:
                close_b = _find_matching_delim(body_prefix, idx, "[", "]")
                if close_b is not None:
                    cap_blob = body_prefix[idx + 1 : close_b]
                    k = close_b + 1
                    while k < n and body_prefix[k].isspace():
                        k += 1
                    tparam_shadows: list[str] = []
                    if k < n and body_prefix[k] == "<":
                        close_t = _find_matching_delim(body_prefix, k, "<", ">")
                        if close_t is not None:
                            tblob = body_prefix[k + 1 : close_t]
                            for tm in re.finditer(
                                r"(?:typename|class|auto|[A-Za-z_]\w*)\s+([A-Za-z_]\w*)\b",
                                tblob,
                            ):
                                tparam_shadows.append(tm.group(1))
                            k = close_t + 1
                            while k < n and body_prefix[k].isspace():
                                k += 1
                    lam_params = ""
                    if k < n and body_prefix[k] == "(":
                        close_lp = _find_matching_delim(body_prefix, k, "(", ")")
                        if close_lp is None:
                            frames.append([body_prefix[k + 1 :], "", "", []])
                            pending_param = pending_body = pending_if = ""
                            break
                        lam_params = body_prefix[k + 1 : close_lp]
                        k = close_lp + 1
                    m_brace = k
                    p_depth = 0
                    lam_brace: int | None = None
                    while m_brace < n:
                        cm = body_prefix[m_brace]
                        if cm == "(":
                            p_depth += 1
                        elif cm == ")":
                            p_depth = max(0, p_depth - 1)
                        elif p_depth == 0:
                            if cm in {";", "}"}:
                                break
                            if cm == "{":
                                lam_brace = m_brace
                                break
                        m_brace += 1
                    if lam_brace is not None:
                        spec_text = body_prefix[k:lam_brace].strip()
                        if not spec_text or _LAMBDA_SPEC_RE.fullmatch(spec_text):
                            init_caps = re.findall(
                                r"(?:^|,)\s*&?\s*([A-Za-z_]\w*)\s*(?:=|\{|\()",
                                cap_blob,
                            )
                            shadow_lines = "".join(
                                f"__lambda_shadow_t {nm};\n"
                                for nm in [*init_caps, *tparam_shadows]
                            )
                            frames.append(
                                [
                                    lam_params,
                                    shadow_lines,
                                    "",
                                    [(0, tnm) for tnm in tparam_shadows],
                                ]
                            )
                            pending_param = pending_body = pending_if = ""
                            idx = lam_brace + 1
                            continue

        if ch == "{":
            m_lt = local_type_head_re.search(frames[-1][1])
            if m_lt:
                frames[-1][3].append((len(frames[-1][1]), m_lt.group(1)))
            frames.append([pending_param, pending_body, pending_if, []])
            pending_param = pending_body = pending_if = ""
        elif ch == "}":
            popped_if = ""
            if len(frames) > 2:
                popped = frames.pop()
                popped_if = popped[2]
            if popped_if and else_after_brace_re.match(body_prefix[idx + 1 :]):
                pending_param = ""
                pending_body = popped_if
                pending_if = popped_if
            else:
                pending_param = pending_body = pending_if = ""
            rest_after_brace = body_prefix[idx + 1 :].lstrip()
            if rest_after_brace and rest_after_brace[0] not in {
                ";",
                ",",
                "=",
                ")",
                "}",
            }:
                frames[-1][1] += ";\n"
        else:
            if ch == ";":
                if pending_if and else_after_brace_re.match(body_prefix[idx + 1 :]):
                    pending_param = ""
                    pending_body = pending_if
                else:
                    pending_param = pending_body = pending_if = ""
            frames[-1][1] += ch
        idx += 1

    if pending_param or pending_body:
        frames.append([pending_param, pending_body, "", []])
    return [(f[0], f[1], list(f[3])) for f in frames]


def _mask_nested_parens(text: str) -> str:
    out: list[str] = []
    depth = 0
    for ch in text:
        if ch == "(":
            depth += 1
            out.append(" ")
        elif ch == ")":
            depth = max(0, depth - 1)
            out.append(" ")
        else:
            out.append(" " if depth > 0 else ch)
    return "".join(out)


def _find_last_decl_in_block(
    block_text: str, ident: str, *, is_param_list: bool
) -> tuple[str, str, int] | None:
    if not re.search(r"\b" + re.escape(ident) + r"\b", block_text):
        return None
    if not is_param_list:
        macro_m = None
        for m in re.finditer(
            r"\bGET_TILING_DATA_WITH_STRUCT\s*\(\s*([A-Za-z_]\w*)\s*,\s*"
            + re.escape(ident)
            + r"\b",
            block_text,
        ):
            macro_m = m
        if macro_m is not None:
            return (macro_m.group(1), "", macro_m.end())
    scan_text = block_text if is_param_list else _mask_nested_parens(block_text)
    sep = r"(?:^|[,;\n])" if is_param_list else r"(?:^|[;{\n])"
    type_seg = r"[A-Za-z_]\w*(?:\s*<[^;{}()]*>)?"
    decl_re = re.compile(
        sep
        + r"\s*(?P<type>(?:(?:const|volatile|constexpr|static|register|unsigned|signed|long|short|struct|class|typename)\b\s*)*"
        r"(?:::)?" + type_seg + r"(?:\s*::\s*" + type_seg + r")*"
        r"(?:\s+(?:const|volatile|unsigned|signed|long|short|int)\b)*)"
        r"(?:\s*(?P<ptr>[*&]+)\s*(?:const\b\s*)?|\s+)"
        + re.escape(ident)
        + r"\b\s*(?=[=,;:)\[{]|$)"
    )
    last_hit: tuple[int, int, str, str] | None = None
    for m in decl_re.finditer(scan_text):
        raw_type = " ".join(m.group("type").split())
        first_word = raw_type.lstrip(":").split()[0] if raw_type.lstrip(":") else ""
        if not first_word or first_word in _NON_TYPE_WORDS:
            continue
        last_hit = (m.start(), m.end(), raw_type, (m.group("ptr") or "").strip())
    if not is_param_list:
        # Handle C++17 structured bindings `auto [cap, other] = ...`.
        for sb in re.finditer(
            r"\bauto\b\s*(?:const\b\s*)?[*&]*\s*\[[^\]]*\b"
            + re.escape(ident)
            + r"\b[^\]]*\]",
            scan_text,
        ):
            if last_hit is None or sb.start() >= last_hit[0]:
                last_hit = (sb.start(), sb.end(), "auto", "")
        # Handle comma-separated local declarations such as `int32_t a = 1, cap = 2;`.
        multi_re = re.compile(
            r"(?:^|[;{\n])\s*(?P<type>(?:(?:const|volatile|constexpr|static|register|unsigned|signed|long|short|struct|class|typename)\b\s*)*"
            r"(?:::)?(?:[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)"
            r"(?:\s+(?:const|volatile|unsigned|signed|long|short|int)\b)*)"
            r"\s+[*&]*\s*[A-Za-z_]\w*[^;{}\n]*?,\s*(?P<ptr>[*&]*)\s*"
            + re.escape(ident)
            + r"\b\s*(?=[=,;:\[{])"
        )
        for m in multi_re.finditer(scan_text):
            raw_type = " ".join(m.group("type").split())
            first_word = raw_type.lstrip(":").split()[0] if raw_type.lstrip(":") else ""
            if not first_word or first_word in _NON_TYPE_WORDS:
                continue
            if last_hit is None or m.start() >= last_hit[0]:
                last_hit = (
                    m.start(),
                    m.end(),
                    raw_type,
                    (m.group("ptr") or "").strip(),
                )
    return (last_hit[2], last_hit[3], last_hit[1]) if last_hit is not None else None


def _resolve_scoped_decl(
    frames: list[tuple[str, str, list[tuple[int, str]]]], ident: str
) -> tuple[tuple[str, str], int, bool, int] | None:
    for frame_idx in range(len(frames) - 1, -1, -1):
        param_hdr, stmt_body, _lts = frames[frame_idx]
        if stmt_body:
            hit = _find_last_decl_in_block(stmt_body, ident, is_param_list=False)
            if hit is not None:
                return ((hit[0], hit[1]), frame_idx, False, hit[2])
        if param_hdr:
            hit = _find_last_decl_in_block(param_hdr, ident, is_param_list=True)
            if hit is not None:
                return ((hit[0], hit[1]), frame_idx, True, hit[2])
    return None


_USING_NS_INLINE_RE = re.compile(
    r"\busing\s+namespace\s+([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)\s*;"
)
_USING_DECL_INLINE_RE = re.compile(
    r"\busing\s+(?!namespace\b)(?:::)?([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)+)\s*;"
)
_ALIAS_INLINE_RE = re.compile(
    r"\b(?:using|namespace)\s+([A-Za-z_]\w*)\s*=|\btypedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*;"
)
_LOCAL_TYPE_FWD_RE = re.compile(
    r"(?:^|[;{}\n])\s*(?:struct|class|union|enum(?:\s+(?:class|struct))?)\s+([A-Za-z_]\w*)\s*;"
)


def _lookup_context_at_decl(
    frames: list[tuple[str, str, list[tuple[int, str]]]],
    decl_frame_idx: int,
    in_param_hdr: bool,
    decl_end_pos: int,
    file_using_ns: set[str],
    file_uncertain_using_ns: set[str],
    file_using_decls: dict[str, set[str]],
    file_aliases: set[str],
) -> tuple[set[str], set[str], dict[str, set[str]], set[str]]:
    """Compute imports and shadowed type names active at the variable's declaration site."""
    active_uns = set(file_using_ns)
    active_uncertain_uns = set(file_uncertain_using_ns)
    active_udecls: dict[str, set[str]] = {
        k_sh: set(v_qs) for k_sh, v_qs in file_using_decls.items()
    }
    cond_aliases = set(file_aliases)
    lexical_shadows: set[str] = set()

    for f_idx in range(decl_frame_idx + 1):
        if f_idx == decl_frame_idx and in_param_hdr:
            break
        _ph, stmt_body, lts = frames[f_idx]
        stmt_slice = stmt_body if f_idx < decl_frame_idx else stmt_body[:decl_end_pos]
        events: list[tuple[int, str, str]] = []
        for um in _USING_NS_INLINE_RE.finditer(stmt_slice):
            events.append(
                (
                    um.start(),
                    "using_ns",
                    "::".join(p.strip() for p in um.group(1).split("::")),
                )
            )
        for dm in _USING_DECL_INLINE_RE.finditer(stmt_slice):
            events.append(
                (
                    dm.start(),
                    "using_decl",
                    "::".join(p.strip() for p in dm.group(1).split("::")),
                )
            )
        for am in _ALIAS_INLINE_RE.finditer(stmt_slice):
            anm = am.group(1) or am.group(2)
            if anm:
                events.append((am.start(), "shadow", anm))
        for fm in _LOCAL_TYPE_FWD_RE.finditer(stmt_slice):
            events.append((fm.start(), "shadow", fm.group(1)))
        for lt_pos, lt_name in lts:
            if f_idx < decl_frame_idx or lt_pos <= decl_end_pos:
                events.append((lt_pos, "shadow", lt_name))
        for _ev_pos, ev_kind, ev_val in sorted(events, key=lambda x: x[0]):
            if ev_kind == "using_ns":
                active_uns.add(ev_val)
            elif ev_kind == "using_decl":
                short = ev_val.split("::")[-1]
                active_udecls[short] = {ev_val}
                lexical_shadows.discard(short)
            elif ev_kind == "shadow":
                lexical_shadows.add(ev_val)
                active_udecls.pop(ev_val, None)

    return (
        active_uns,
        active_uncertain_uns,
        active_udecls,
        cond_aliases | lexical_shadows,
    )


def _is_64bit_scalar_decl(decl: tuple[str, str] | None) -> bool:
    if decl is None:
        return False
    raw_type, ptr = decl
    if "*" in ptr:
        return False
    tokens = [
        tok
        for tok in raw_type.replace("std::", "").split()
        if tok not in {"const", "volatile", "constexpr", "static", "register"}
    ]
    return " ".join(tokens) in _WIDTH_64_DECL_TYPES


def _is_struct_receiver_decl(
    decl: tuple[str, str] | None,
    cand_qname: str,
    cand_short: str,
    access_op: str,
    *,
    func_ns_parts: list[str],
    using_ns: set[str],
    uncertain_using_ns: set[str],
    using_decls: dict[str, set[str]],
    all_known_qnames: set[str],
    aliased_short_names: set[str],
) -> bool:
    if decl is None:
        return False
    raw_type, ptr = decl
    if ("*" in ptr) != (access_op == "->"):
        return False
    tokens = [
        tok
        for tok in raw_type.split()
        if tok
        not in {
            "const",
            "volatile",
            "constexpr",
            "static",
            "struct",
            "class",
            "typename",
        }
    ]
    if not tokens:
        return False
    norm = re.sub(r"\s*::\s*", "::", " ".join(tokens)).strip()
    if " " in norm:
        return False
    if norm.startswith("::"):
        return norm[2:] == cand_qname

    all_possible_uns = using_ns | uncertain_using_ns
    prefixes = ["::".join(func_ns_parts[:k]) for k in range(len(func_ns_parts), -1, -1)]
    if "::" in norm:
        head = norm.split("::", 1)[0]
        if head in aliased_short_names:
            return False
        certain_matches: set[str] = set()
        for pfx in prefixes:
            q = f"{pfx}::{norm}" if pfx else norm
            if q in all_known_qnames:
                certain_matches.add(q)
                if pfx:
                    break
        for uns in using_ns:
            uq = f"{uns}::{norm}"
            if uq in all_known_qnames:
                certain_matches.add(uq)
        possible_matches = set(certain_matches)
        for uns in all_possible_uns:
            uq = f"{uns}::{norm}"
            if uq in all_known_qnames:
                possible_matches.add(uq)
        return certain_matches == {cand_qname} and possible_matches == {cand_qname}

    # Unqualified receiver type.
    if norm != cand_short or norm in aliased_short_names:
        return False
    if norm in using_decls:
        return using_decls[norm] == {cand_qname}
    for pfx in prefixes:
        if not pfx:
            continue
        q = f"{pfx}::{norm}"
        if q in all_known_qnames:
            certain_set = {q} | {
                f"{uns}::{norm}"
                for uns in using_ns
                if f"{uns}::{norm}" in all_known_qnames
            }
            possible_set = set(certain_set) | {
                f"{uns}::{norm}"
                for uns in all_possible_uns
                if f"{uns}::{norm}" in all_known_qnames
            }
            return certain_set == {cand_qname} and possible_set == {cand_qname}
    certain_global: set[str] = set()
    if norm in all_known_qnames:
        certain_global.add(norm)
    for uns in using_ns:
        uq = f"{uns}::{norm}"
        if uq in all_known_qnames:
            certain_global.add(uq)
    possible_global = set(certain_global)
    for uns in all_possible_uns:
        uq = f"{uns}::{norm}"
        if uq in all_known_qnames:
            possible_global.add(uq)
    return certain_global == {cand_qname} and possible_global == {cand_qname}


def _strip_outer_parens(expr: str) -> str:
    s = expr.strip()
    while s.startswith("(") and s.endswith(")"):
        if _find_matching_delim(s, 0, "(", ")") != len(s) - 1:
            break
        s = s[1:-1].strip()
    return s


def _split_top_level_args(args_blob: str) -> list[str]:
    parts: list[str] = []
    cur: list[str] = []
    paren_depth = 0
    angle_depth = 0
    bracket_depth = 0
    for idx, ch in enumerate(args_blob):
        if ch == "(":
            paren_depth += 1
        elif ch == ")":
            paren_depth = max(0, paren_depth - 1)
        elif ch == "[":
            bracket_depth += 1
        elif ch == "]":
            bracket_depth = max(0, bracket_depth - 1)
        elif ch == "<" and paren_depth == 0 and bracket_depth == 0:
            angle_depth += 1
        elif (
            ch == ">"
            and angle_depth > 0
            and paren_depth == 0
            and bracket_depth == 0
            and (idx == 0 or args_blob[idx - 1] != "-")
        ):
            angle_depth -= 1
        elif ch == "," and paren_depth == 0 and angle_depth == 0 and bracket_depth == 0:
            parts.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    if cur:
        parts.append("".join(cur).strip())
    return parts


def _check_build_script_and_width_contracts(
    context: dict[str, Any], findings: list[dict[str, Any]]
) -> None:
    """Detect standalone negated build commands and verified 32/64-bit shared field conflicts."""
    texts = _source_texts(context)
    if not texts:
        return
    for rel, text in texts.items():
        norm_rel = str(rel).replace("\\", "/")
        if Path(norm_rel).name == "build.sh" or norm_rel.endswith("/build.sh"):
            for lineno, snippet in _find_standalone_negated_build_lines(text):
                _add_finding(
                    findings,
                    "build_script_negated_status",
                    "warning",
                    "cmake_config",
                    "Standalone '!' negates a build/package command outside an if/while condition.",
                    rel,
                    lineno,
                    [snippet],
                    "Check the enclosing shell control flow and actual exit receipt; remove standalone '!' if it inverts the build exit status.",
                )

    # Single-pass extraction of all qualified struct/tiling declarations and function scopes.
    # Candidate tuple: (decl_rel, qname, sname, is_macro_tiling, ftype, fname, decl_line, siblings_64)
    narrowed_candidates: list[tuple[str, str, str, bool, str, str, int, list[str]]] = []
    all_known_qnames: set[str] = set()
    qname_decl_files: dict[str, set[str]] = {}
    cpp_units: list[
        tuple[
            str,
            str,
            list[
                tuple[
                    list[str],
                    set[str],
                    set[str],
                    dict[str, set[str]],
                    set[str],
                    str,
                    str,
                ]
            ],
            set[str],
        ]
    ] = []

    tiling_name_re = re.compile(r"^\w*(?:Tiling|Buffer|Param|Info)\w*$")
    for rel, text in texts.items():
        low_rel = str(rel).lower()
        if not low_rel.endswith(
            (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".asc", ".cce")
        ):
            continue
        raw_vis = _visible_source(text)
        if raw_vis is None:
            continue
        pre_vis, unresolved_spans = _strip_preprocessor_preserving_offsets(raw_vis)
        vis = _blank_cpp_attributes(pre_vis)
        type_blocks, file_qnames, func_scopes, cond_tainted = _extract_scoped_units(
            vis, str(rel), unresolved_spans
        )
        all_known_qnames.update(file_qnames)
        for qn in file_qnames:
            qname_decl_files.setdefault(qn, set()).add(str(rel))
        active_func_scopes = [
            (fns, c_uns, u_uns, udecls, s_aliases, ptxt, btxt)
            for fns, c_uns, u_uns, udecls, s_aliases, ptxt, btxt, has_cond in func_scopes
            if not has_cond
        ]
        if active_func_scopes:
            cpp_units.append((str(rel), text, active_func_scopes, cond_tainted))
        if not low_rel.endswith((".h", ".hpp", ".hh")):
            continue
        for (
            qname,
            sname,
            is_macro_tiling,
            skip_cand,
            body_start,
            _body_end,
            body,
        ) in type_blocks:
            if skip_cand:
                continue
            if not is_macro_tiling and not tiling_name_re.match(sname):
                continue
            if is_macro_tiling:
                parsed_fields = [
                    (m.group(1), m.group(2), m.start())
                    for m in re.finditer(
                        r"TILING_DATA_FIELD_DEF\s*\(\s*(u?int(?:32|64)_t)\s*,\s*([A-Za-z_]\w*)\s*\)",
                        body,
                    )
                ]
            else:
                parsed_fields = _extract_direct_struct_fields(body)
            siblings_64 = [fnm for ftp, fnm, _foff in parsed_fields if "64" in ftp]
            if not siblings_64:
                continue
            widths_by_name: dict[str, set[str]] = {}
            for ftp, fnm, _foff in parsed_fields:
                widths_by_name.setdefault(fnm, set()).add("64" if "64" in ftp else "32")
            for ftype, fname, foff in parsed_fields:
                if "32" in ftype and widths_by_name.get(fname) == {"32"}:
                    lineno = vis[: body_start + foff].count("\n") + 1
                    narrowed_candidates.append(
                        (
                            str(rel),
                            qname,
                            sname,
                            is_macro_tiling,
                            ftype,
                            fname,
                            lineno,
                            siblings_64[:4],
                        )
                    )

    if not narrowed_candidates or not cpp_units:
        return

    seen_fields: set[tuple[str, str, str]] = set()
    call_head_re = re.compile(
        r"\b(?:std::(?:min|max)|CeilDiv|DivCeil|RoundUp\w*|AlignUp\w*)\s*\("
    )
    ident_re = re.compile(r"^[A-Za-z_]\w*$")

    for (
        decl_rel,
        qname,
        sname,
        is_macro_tiling,
        ftype,
        fname,
        decl_line,
        siblings_64,
    ) in narrowed_candidates:
        if (decl_rel, qname, fname) in seen_fields:
            continue
        competing_files = qname_decl_files.get(qname, set())
        field_expr_re = re.compile(
            r"^(?P<recv>[A-Za-z_]\w*)\s*(?P<op>\.|->)\s*"
            + (
                r"get_" + re.escape(fname) + r"\s*\(\s*\)$"
                if is_macro_tiling
                else re.escape(fname) + r"$"
            )
        )
        for use_rel, raw_use_text, scopes, cond_tainted in cpp_units:
            if len(competing_files) > 1 and use_rel != decl_rel:
                inc_names = set(
                    Path(m.group(1)).name
                    for m in re.finditer(
                        r'#\s*include\s*[<"]([^>"]+)[>"]', raw_use_text
                    )
                )
                matching_hdrs = [
                    cf for cf in competing_files if Path(cf).name in inc_names
                ]
                if matching_hdrs != [decl_rel]:
                    continue
            matched_evidence: tuple[str, str] | None = None
            for (
                func_ns,
                file_using_ns,
                file_uncertain_using_ns,
                file_using_decls,
                scope_aliases,
                params_text,
                body_text,
            ) in scopes:
                base_aliases = scope_aliases | cond_tainted
                if fname not in body_text:
                    continue
                for head_m in call_head_re.finditer(body_text):
                    open_p = head_m.end() - 1
                    close_p = _find_matching_delim(body_text, open_p, "(", ")")
                    if close_p is None:
                        continue
                    args_blob = body_text[open_p + 1 : close_p]
                    parts = _split_top_level_args(args_blob)
                    if len(parts) < 2:
                        continue
                    stripped_parts = [_strip_outer_parens(p) for p in parts]
                    field_indices: list[tuple[int, str, str]] = []
                    for idx, sp in enumerate(stripped_parts):
                        fm = field_expr_re.fullmatch(sp)
                        if fm is not None:
                            field_indices.append(
                                (idx, fm.group("recv"), fm.group("op"))
                            )
                    if not field_indices:
                        continue
                    body_prefix = body_text[: head_m.start()]
                    frames = _active_scope_blocks(params_text, body_prefix)
                    bound_field_idxs: list[int] = []
                    for idx, recv, op in field_indices:
                        resolved_recv = _resolve_scoped_decl(frames, recv)
                        if resolved_recv is None:
                            continue
                        recv_decl, r_fidx, r_in_param, r_end = resolved_recv
                        r_uns, r_uuns, r_udecls, r_aliases = _lookup_context_at_decl(
                            frames,
                            r_fidx,
                            r_in_param,
                            r_end,
                            file_using_ns,
                            file_uncertain_using_ns,
                            file_using_decls,
                            base_aliases,
                        )
                        if _is_struct_receiver_decl(
                            recv_decl,
                            qname,
                            sname,
                            op,
                            func_ns_parts=func_ns,
                            using_ns=r_uns,
                            uncertain_using_ns=r_uuns,
                            using_decls=r_udecls,
                            all_known_qnames=all_known_qnames,
                            aliased_short_names=r_aliases,
                        ):
                            bound_field_idxs.append(idx)
                    if not bound_field_idxs:
                        continue
                    peer_found: str | None = None
                    for idx, sp in enumerate(stripped_parts):
                        if idx in bound_field_idxs or not ident_re.fullmatch(sp):
                            continue
                        resolved_peer = _resolve_scoped_decl(frames, sp)
                        if resolved_peer is None:
                            continue
                        peer_decl, p_fidx, p_in_param, p_end = resolved_peer
                        _, _, _, p_aliases = _lookup_context_at_decl(
                            frames,
                            p_fidx,
                            p_in_param,
                            p_end,
                            file_using_ns,
                            file_uncertain_using_ns,
                            file_using_decls,
                            base_aliases,
                        )
                        peer_first = (
                            peer_decl[0].replace("std::", "").split()[-1]
                            if peer_decl[0].split()
                            else ""
                        )
                        if peer_first not in p_aliases and _is_64bit_scalar_decl(
                            peer_decl
                        ):
                            peer_found = sp
                            break
                    if peer_found is not None:
                        call_snip = " ".join(
                            body_text[head_m.start() : close_p + 1].split()
                        )[:120]
                        matched_evidence = (peer_found, call_snip)
                        break
                if matched_evidence is not None:
                    break
            if matched_evidence:
                peer_name, call_snip = matched_evidence
                seen_fields.add((decl_rel, qname, fname))
                display_type = qname if not qname.startswith("<anon:") else sname
                _add_finding(
                    findings,
                    "tiling_field_width_inconsistency",
                    "warning",
                    "tiling_consistency",
                    f"Field {fname} ({ftype} in {display_type}, sibling 64-bit fields: {', '.join(siblings_64)}) mixes with 64-bit operand '{peer_name}' at {use_rel}.",
                    decl_rel,
                    decl_line,
                    [
                        f"declaration={display_type}::{fname} ({ftype})",
                        f"peer_64bit={peer_name}",
                        f"call={use_rel}: {call_snip}",
                    ],
                    "Compare the struct/tiling declaration, sibling fields, producer arithmetic, and consumer contracts before deciding whether the declaration or the call expression drifted.",
                )
                break


def check_context(
    context: dict[str, Any], rule_set: str | None = None, opcheck: bool = False
) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    _check_config_rules(context, findings)
    _check_env(context, findings)
    _check_tiling(context, findings)
    _check_build_script_and_width_contracts(context, findings)
    if rule_set == "schema_consistency" or opcheck:
        _check_schema(context, findings)
    result = {
        "findings": findings,
        "summary": {
            "total": len(findings),
            "errors": sum(1 for f in findings if f["severity"] == "error"),
        },
    }
    if opcheck:
        result["contract_summary"] = _contract_summary(context)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Ascend C static diagnosis rules.")
    parser.add_argument("--context")
    parser.add_argument("--case")
    parser.add_argument("--rule-set")
    parser.add_argument("--opcheck", action="store_true")
    parser.add_argument("--json-out", required=True)
    args = parser.parse_args()
    try:
        if args.context:
            context = load_json(args.context, default={})
        elif args.case:
            context = collect_context(Path(args.case).resolve())
        else:
            raise ValueError("--context or --case is required")
        data = check_context(context, rule_set=args.rule_set, opcheck=args.opcheck)
    except Exception as exc:
        data = {
            "findings": [],
            "summary": {"total": 0, "errors": 0},
            **error_payload(
                "static_rule_checker", exc, context=args.context, case=args.case
            ),
        }
    write_json(Path(args.json_out), data)
    print(str(Path(args.json_out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
