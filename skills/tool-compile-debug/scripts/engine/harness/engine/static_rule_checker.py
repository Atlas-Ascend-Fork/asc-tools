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
        PROJECT_ROOT / "harness/knowledge/static_rules.yaml", default={"rules": []}
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


def check_context(
    context: dict[str, Any], rule_set: str | None = None, opcheck: bool = False
) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    _check_config_rules(context, findings)
    _check_env(context, findings)
    _check_tiling(context, findings)
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
