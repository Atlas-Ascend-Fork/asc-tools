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
import posixpath
import re
from pathlib import Path
from typing import Any

from harness.engine.cause_tree import build_cause_tree
from harness.engine.common import error_payload
from harness.engine.common import load_json
from harness.engine.common import resolve_project_source
from harness.engine.common import write_json
from harness.engine.diagnostic_facts import _failed_line
from harness.engine.diagnostic_facts import build_diagnostic_facts
from harness.engine.stage_evidence import build_stage_evidence

FINDING_PRIORITY = {
    # Cross-file evidence outranks environment and configuration hints when the
    # primary diagnostic is otherwise unknown.
    "crossfile_missing_tiling_type": 98,
    "crossfile_shared_const_renamed": 97,
    "crossfile_missing_entry_symbol": 97,
    "tiling_field_mismatch": 96,
    "cmake_missing_find_package_asc": 94,
    "cmake_missing_language_asc": 92,
    "cann_path_missing": 90,
    "cann_or_soc_config_invalid": 90,
    "kernel_operator_header_missing": 88,
    "prototype_host_kernel_arg_count_mismatch": 80,
}


def _root_in_project(root_file: str | None) -> bool:
    """Apply the legacy path heuristic for a likely project-owned candidate.

    This does not establish filesystem ownership or authorization to edit a file.
    """
    if not root_file:
        return False
    rf = str(root_file).replace("\\", "/")
    out_markers = (
        "/usr/",
        "/opt/",
        "$cann",
        "cann-",
        "/ascend/",
        "ascend-toolkit",
        "asc/include",
    )
    low = rf.lower()
    if any(m in low for m in out_markers):
        return False
    # 绝对路径但不含项目工程标志（op_host/op_kernel/CMakeLists）也视为项目外
    return not (
        rf.startswith("/")
        and not any(k in rf for k in ("op_host", "op_kernel", "CMakeLists"))
    )


def _detect_self_explanatory(
    primary: dict[str, Any] | None,
    root_localization: dict[str, Any] | None,
) -> bool:
    """保留 self_explanatory 兼容字段：有 file+line，且未定位到项目内根文件。

    True 只表示存在局部排查起点，不证明根因恰在报错行，也不排除跨文件原因。
    是否可直接修改，仍取决于已有源码、声明和需求证据。
    """
    if not primary:
        return False
    has_locus = bool(primary.get("file")) and primary.get("line") is not None
    if not has_locus:
        return False
    root_file = (root_localization or {}).get("root_file")
    # 有项目内有效跨文件根 → 不是单文件自解释（该用 diagnose 的定位）
    return not _root_in_project(root_file)


def _cascade_forewarning(
    primary: dict[str, Any] | None,
    context: dict[str, Any] | None,
) -> str | None:
    """给缺失生成头的分离工程提示后续取证，不预判新错误的因果。

    路径只提示可能存在生成步骤；缺头本身不能证明删 include 是合法修法，
    也不能证明修复后的新首错必属同一根因或补丁没有引入回归。
    """
    if not primary:
        return None
    file_text = (context or {}).get("file_text") or {}
    if not isinstance(file_text, dict):
        return None
    # 条件①：完整算子工程（host 侧实现与 kernel 侧分列在各自目录）。
    keys = [str(k).replace("\\", "/") for k in file_text]
    has_host = any(k.startswith("op_host/") or "/op_host/" in k for k in keys)
    has_kernel = any(k.startswith("op_kernel/") or "/op_kernel/" in k for k in keys)
    if not (has_host and has_kernel):
        return None  # This advisory requires both host and kernel source groups.
    # Limit the advisory to a missing generated-looking header in a split project.
    # The layout alone does not establish a missing generation step.
    message = str(primary.get("message") or "").lower()
    missing_file = "no such file" in message or "file not found" in message
    generated = any(
        tok in message for tok in ("build_out/", "autogen/", "generated/", "_stub")
    )
    if not (missing_file and generated):
        return None
    return (
        "级联预警：工程含 op_host/ 与 op_kernel/，首错引用疑似构建期生成头。"
        "先核该头的生成步骤、输入和 include 路径，不凭缺头直接删除 include。"
        "完成有依据的修改集后按原配方验证；若出现新首错，比较修改前后证据，"
        "分别判断原错后续、独立错误或补丁回归。需要 API 契约时先查相关知识页；"
        "声明或适用条件仍不明时，定向查当前所选 SDK 的对应声明，避免无目标地全库搜索。"
    )


# Keep these message families aligned with verify_runner._ESCAPE_SYMBOL_RES.
# A suggestion or missing member follows a separate evidence path.
_DELETED_SYMBOL_RES = [
    re.compile(r"(?:use\ of\ )?undeclared\ identifier\ '(?P<sym>[A-Za-z_]\w*)'"),
    re.compile(r"no\ template\ named\ '(?P<sym>[A-Za-z_]\w*)'"),
    re.compile(r"no\ type\ named\ '(?P<sym>[A-Za-z_]\w*)'"),
    re.compile(r"unknown\ type\ name\ '(?P<sym>[A-Za-z_]\w*)'"),
]
_USAGE_SRC_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".cce", ".cxx", ".c"}


def _deleted_symbol_usages(
    primary: dict[str, Any],
    context: dict[str, Any] | None,
    all_errors: list[dict[str, Any]] | None = None,
    *,
    neutral: bool = False,
) -> str | None:
    """Project bounded source uses of symbols named by missing-declaration errors.

    Return one next-action string or None without loaded source/use evidence.
    At most four lines of 110 characters are included. A missing declaration
    diagnostic alone does not establish that a definition was deleted.
    """
    msg = str((primary or {}).get("message") or "")
    if "did you mean" in msg.lower():
        return None  # Compiler spelling suggestions use a separate evidence path.
    syms: list[str] = []
    for line in msg.splitlines():
        for rx in _DELETED_SYMBOL_RES:
            m = rx.search(line)
            if m and m.group("sym") not in syms and len(m.group("sym")) > 2:
                syms.append(m.group("sym"))
    if not syms:
        return None
    file_text = (context or {}).get("file_text") or {}
    if not isinstance(file_text, dict) or not file_text:
        return None
    hits: list[str] = []
    seen_files: set[str] = set()
    for rel, text in sorted(file_text.items(), key=lambda kv: str(kv[0])):
        rel_s = str(rel).replace("\\", "/")
        if Path(rel_s).suffix not in (
            _USAGE_SRC_SUFFIXES | ({".asc"} if neutral else set())
        ):
            continue
        visible_lines = (
            _cpp_visible_text(str(text), mask_literals=True).splitlines()
            if neutral
            else []
        )
        for i, ln in enumerate(str(text).splitlines(), 1):
            has_usage = (
                any(
                    re.search(r"\b" + re.escape(s) + r"\b", visible_lines[i - 1])
                    for s in syms
                )
                if neutral
                else any(s in ln for s in syms)
            )
            if has_usage and ln.strip():
                tag = f"{rel_s}:{i}"
                hits.append(f"{tag}: `{ln.strip()[:110]}`")
                if rel_s not in seen_files:
                    seen_files.add(rel_s)
                if len(hits) >= 4:
                    break
        if len(hits) >= 4:
            break
    if not hits:
        return None

    # Project additional missing project symbols so callers can inspect related
    # contract failures together instead of stopping at the first occurrence.
    all_missing: list[str] = []
    for e in all_errors or []:
        emsg = str((e or {}).get("message") or "")
        if "did you mean" in emsg.lower():
            continue
        for rx in _DELETED_SYMBOL_RES:
            m = rx.search(emsg)
            if m and m.group("sym") not in all_missing and len(m.group("sym")) > 2:
                all_missing.append(m.group("sym"))
    cascade = ""
    if len(set(syms) | set(all_missing)) >= 2:
        cascade = (
            f"（日志还点名其它未声明符号 {sorted(set(syms) | set(all_missing))[:4]}；分别核对其用法与声明）"
            if neutral
            else (
                f"（日志共点名 {len(set(syms) | set(all_missing))} 个缺失符号"
                f" {sorted(set(syms) | set(all_missing))[:4]}；逐项核对声明与需求，"
                "完成有依据的修改集后按原配方验证）"
            )
        )
    return (
        (
            f"使用点投影（{'/'.join(syms[:2])} 的当前源码用法线索，缺失原因尚未确定）："
            if neutral
            else f"使用点投影（{'/'.join(syms[:2])} 的当前源码用法，供核对声明契约）："
        )
        + "；".join(hits)
        + cascade
    )


def _unknown_usage_context(
    primary: dict[str, Any], context: dict[str, Any]
) -> dict[str, Any] | None:
    """Bind the failed line to directly owned, loaded source before projecting it."""
    if primary.get("file_resolved") is False or primary.get("file_reanchored") is True:
        return None
    file_text = context.get("file_text") or {}
    raw, line = primary.get("file"), primary.get("line")
    if (
        not isinstance(file_text, dict)
        or not isinstance(raw, str)
        or not isinstance(line, int)
    ):
        return None
    case_dir = context.get("case_dir")
    if case_dir:
        project = Path(case_dir).resolve()
        resolved = resolve_project_source(raw, project, reanchor=False)
        if resolved is None:
            return None
        key = resolved.relative_to(project).as_posix()
    else:
        # Direct callers may supply an in-memory relative source map. Without
        # an owning root, absolute/escaping/historical paths supply no identity.
        key = raw.replace("\\", "/")
        if key.startswith("/") or ":" in key or "$" in key or ".." in key.split("/"):
            return None
        key = posixpath.normpath(key)
    text = file_text.get(key)
    if not isinstance(text, str) or not 0 < line <= len(text.splitlines()):
        return None
    visible = _cpp_visible_text(text, mask_literals=True).splitlines()[line - 1]
    message = str(primary.get("message") or "")
    if not any(
        (match := rx.search(message))
        and re.search(r"\b" + re.escape(match.group("sym")) + r"\b", visible)
        for rx in _DELETED_SYMBOL_RES
    ):
        return None
    return {"file_text": {key: text}}


def _build_unknown_fallback(
    primary: dict[str, Any] | None,
    all_errors: list[dict[str, Any]],
    root_localization: dict[str, Any] | None,
) -> tuple[str, list[str]] | None:
    """Expose useful secondary clues when the primary category is unknown without guessing."""
    from collections import Counter

    hints: list[str] = []
    next_actions: list[str] = []

    # Preserve classified secondary diagnostics as leads when the first is unknown.
    secondary = Counter(
        e.get("category")
        for e in (all_errors or [])
        if e.get("category") not in (None, "unknown", "unknown_compile")
    )
    if secondary:
        top = "、".join(f"{cat}×{n}" for cat, n in secondary.most_common(3))
        hints.append(f"首错未能归类，但日志中检测到已分类的次级信号：{top}")
        next_actions.append(
            f"优先核查次级信号最强的类别（{secondary.most_common(1)[0][0]}）对应的错误行"
        )

    # An unclassified diagnostic can still provide a useful source location.
    if primary and primary.get("file") and primary.get("line") is not None:
        hints.append(f"首个编译错误位于 {primary.get('file')}:{primary.get('line')}")
        next_actions.append(
            "从 primary_error 的 file:line 开始读源码，据编译器原始消息判断根因"
        )

    # An upstream declaration candidate can refine the symptom location.
    if root_localization and root_localization.get("root_file"):
        hints.append(f"跨文件定位候选根文件：{root_localization.get('root_file')}")
        next_actions.append(
            f"检查候选根文件 {root_localization.get('root_file')} 是否为真正需修改处"
        )

    if not hints:
        return None

    summary = (
        "未能给出自信的错误类别（category=unknown）——以下是可用于定位的线索，请据此自行判断根因："
        + "；".join(hints)
        + "。"
    )
    next_actions.append("若线索不足，可调 run_verify 在隔离副本重编以获取更完整日志")
    return summary, next_actions


def _category_to_summary(category: str, phase: str) -> str:
    mapping = {
        "cmake_config": "CMake 配置阶段失败，主因很可能是 Ascend C CMake 配置不完整。",
        "header_missing": "编译阶段失败，主因是头文件或 include 路径缺失。",
        "link_error": "链接阶段失败，主因是符号或库依赖未正确链接。",
        "template_error": "C++/Ascend C 模板或重载解析失败。",
        "api_incompatible": "Ascend C API、命名空间或调用签名不兼容。",
        "host_compile_error": "Host 侧 C++ 编译失败，主因可能是声明/定义、语法或接口不一致。",
        "kernel_compile_error": "Kernel 侧 Ascend C 编译失败。",
        "tiling_consistency": "Kernel/Host/Tiling 契约不一致。",
        "compiler_limit": "疑似触发编译器限制。",
        "compiler_internal": "疑似编译器内部错误或崩溃，需要最小复现和规避方案。",
        "environment": "环境探测发现 CANN 或 SoC 配置问题。",
        "unfriendly_error_code": "日志只提供不友好的错误码，需要进一步探针定位。",
        "no_compile_error": "未识别到编译失败信号；空、缺失或不完整日志不能证明构建通过。请核实际执行收据或提供完整失败日志。",
    }
    return mapping.get(category, f"{phase} 阶段失败，类别为 {category}。")


def _observed_mechanism_summary(facts: dict[str, Any]) -> str | None:
    """Describe the observed failure without promoting a legacy root-cause guess."""
    return {
        "undeclared_identifier": "名字查找失败：报错标识符在该使用点未声明，需核声明、作用域和当前用法。",
        "unknown_type": "类型名字在当前使用点不可见，需核类型声明、作用域及包含/宏条件。",
        "member_lookup_failure": "成员查找失败，需核实际接收者类型或命名空间及其声明。",
        "overload_resolution_failure": "当前调用未匹配可用重载，需核实参类型/数量与候选声明。",
        "argument_count_mismatch": "诊断指出实参数量不匹配；API归属及正确调用形式需另核声明。",
        "syntax_error": "编译器报告语法错误；先核报错附近语法及必要的宏展开上下文。",
        "header_lookup_failure": "头文件查找失败；需核所请求的头、实际include搜索路径及生成输入。",
        "link_unresolved_symbol": "链接器报告未解析符号；需核定义、目标输入和原链接参数。",
        "link_library_lookup_failure": "链接器未找到请求的库；需核库文件、搜索目录和原链接参数。",
        "driver_option_rejection": "编译器驱动拒绝参数或缺少输入；先核实际命令与工具链，不从源码目录推断失败域。",
        "compiler_crash": "日志记录编译器内部错误或中止；这不证明输入源码合法，需结合前序诊断定位。",
        "static_assertion": "编译期断言失败；需核触发条件与调用契约，不以删除断言作为修复依据。",
        "conversion_failure": "诊断指出类型转换或初始化失败；需核源/目标类型及允许的转换。",
    }.get((facts.get("mechanism") or {}).get("value"))


def _phase_from_category(category: str, primary: dict[str, Any] | None) -> str:
    mapped = {
        "cmake_config": "cmake_configure",
        "environment": "env_check",
        "link_error": "link",
        "header_missing": "kernel_compile",
        "api_incompatible": "kernel_compile",
        "host_compile_error": "host_compile",
        "kernel_compile_error": "kernel_compile",
        "tiling_consistency": "kernel_compile",
        "schema_consistency": "host_compile",
        "compiler_limit": "kernel_compile",
        "compiler_internal": "kernel_compile",
    }
    primary_category = primary.get("category") if primary else None
    if category in mapped and primary_category != category:
        return mapped[category]
    if (
        primary
        and primary.get("phase")
        and primary.get("phase") not in {"unknown_compile", "package"}
    ):
        return primary["phase"]
    return mapped.get(
        category, primary.get("phase", "unknown") if primary else "unknown"
    )


_CPP_COMMENTS_AND_LITERALS = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
    re.DOTALL,
)


def _cpp_visible_text(text: str, *, mask_literals: bool = False) -> str:
    """Keep line offsets; comments and string contents are never declarations."""

    def replace(match: re.Match) -> str:
        token = match.group(0)
        if mask_literals or token.startswith(("//", "/*")):
            return "".join("\n" if ch == "\n" else " " for ch in token)
        return token

    return _CPP_COMMENTS_AND_LITERALS.sub(replace, text)


def _local_type_visibility_evidence(
    primary: dict[str, Any], context: dict[str, Any]
) -> dict[str, Any] | None:
    """A missing type with a unique, unincluded project definition is an include lead.

    This checks only loaded project sources. Macro includes, omitted files,
    duplicate definitions and conditional/namespace declarations stay unknown.
    It does not infer SDK declarations, generated dependencies or a patch.
    """
    message = str(primary.get("message") or "")
    match = re.search(
        r"(?:unknown type name|(?:use of )?undeclared identifier) ['‘]([A-Za-z_]\w*)['’]"
        r"|['‘]([A-Za-z_]\w*)['’] (?:was not declared in this scope|does not name a type)",
        message,
    )
    if not match or "did you mean" in message.lower():
        return None
    symbol = next(group for group in match.groups() if group)
    file_text = context.get("file_text") or {}
    if not isinstance(file_text, dict) or (context.get("source_coverage") or {}).get(
        "omitted_sources"
    ):
        return None
    sources = {
        posixpath.normpath(str(path).replace("\\", "/")): str(value)
        for path, value in file_text.items()
        if Path(str(path)).suffix.lower()
        in {".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx", ".asc", ".cce"}
    }
    failed_path = str(primary.get("file") or "").replace("\\", "/")
    matches = [
        path
        for path in sources
        if failed_path == path or failed_path.endswith("/" + path)
    ]
    if len(matches) != 1:
        return None
    failed_file = matches[0]
    declarations = []
    declaration_re = re.compile(r"\b(?:struct|class)\s+" + re.escape(symbol) + r"\s*\{")
    for path, original in sources.items():
        if Path(path).suffix.lower() not in {".h", ".hpp", ".hh"}:
            continue
        visible = _cpp_visible_text(original, mask_literals=True)
        # Ordinary #ifndef header guards are allowed; target/feature branches are not evaluated.
        if re.search(r"^\s*#\s*(?:if|ifdef|elif|else)\b", visible, re.MULTILINE):
            continue
        guards = re.findall(r"^\s*#\s*ifndef\s+([A-Za-z_]\w*)", visible, re.MULTILINE)
        if any(
            not re.search(
                r"#\s*ifndef\s+"
                + re.escape(guard)
                + r"\s*\n\s*#\s*define\s+"
                + re.escape(guard)
                + r"\b",
                visible,
            )
            for guard in guards
        ):
            continue
        visible = re.sub(
            r"^\s*#(?:[^\n]*\\\n)*[^\n]*",
            lambda m: "\n" * m.group(0).count("\n"),
            visible,
            flags=re.MULTILINE,
        )
        for found in declaration_re.finditer(visible):
            before = visible[: found.start()]
            if before.count("{") == before.count("}"):
                declarations.append((path, before.count("\n") + 1))
    if len(declarations) != 1:
        return None
    declaration_file, declaration_line = declarations[0]
    visited: set[str] = set()
    pending = [failed_file]
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        for include in re.finditer(
            r"^\s*#\s*include\s+([^\n]+)",
            _cpp_visible_text(sources[current]),
            re.MULTILINE,
        ):
            literal = re.fullmatch(r'[<"]([^>"\n]+)[>"]\s*', include.group(1))
            if not literal:
                return (
                    None  # A computed include can hide the declaration's actual route.
                )
            name = posixpath.normpath(literal.group(1).replace("\\", "/"))
            relative = posixpath.normpath(
                posixpath.join(posixpath.dirname(current), name)
            )
            resolved = [
                path
                for path in sources
                if path in {relative, name} or path.endswith("/" + name)
            ]
            # Include search order is not known here: every plausible match is reachable.
            pending.extend(path for path in resolved if path not in visited)
    if declaration_file in visited:
        return None
    return {
        "kind": "local_type_definition_not_in_include_graph",
        "symbol": symbol,
        "failed_file": failed_file,
        "declaration_file": declaration_file,
        "declaration_line": declaration_line,
        "include_graph_files": sorted(visited),
        "scope": "loaded_project_sources",
        "requires_include_configuration_check": True,
    }


def _tiling_type_use_evidence(
    normalized: dict[str, Any], facts: dict[str, Any]
) -> dict[str, Any] | None:
    """Identify a failed tiling type argument from bound compiler diagnostics.

    A type name alone says nothing about a tiling contract. Require a qualified
    companion diagnostic in the same local source-diagnostic group.
    This identifies the affected API family, not a registration or schema cause.
    """
    primary = normalized.get("primary_error") or {}
    if facts["mechanism"]["value"] != "undeclared_identifier":
        return None
    if normalized.get("stage_observations_omitted"):
        return None
    message = str(primary.get("message") or "")
    missing = re.search(
        r"(?:undeclared identifier '([A-Za-z_]\w*)'|'([A-Za-z_]\w*)' was not declared)",
        message,
    )
    if not missing or not primary.get("file") or not primary.get("line"):
        return None
    position = primary.get("log_line")
    if not isinstance(position, int) or isinstance(position, bool) or position < 1:
        return None
    symbol = missing.group(1) or missing.group(2)
    argument = r"\bGetTilingData\s*<\s*" + re.escape(symbol) + r"\s*>\s*\("
    echo = primary.get("code_line")
    visible = (
        _cpp_visible_text(echo, mask_literals=True) if isinstance(echo, str) else None
    )
    if visible is not None:
        if not re.search(argument, visible):
            return None
        # A repeated name on the same line may refer to another failed use.
        if len(re.findall(r"\b" + re.escape(symbol) + r"\b", visible)) != 1:
            return None
    original_file = primary.get("file_original") or primary["file"]
    for companion in normalized.get("all_errors") or []:
        other_position = companion.get("log_line")
        if (
            companion.get("kind") != "error"
            or (companion.get("file_original") or companion.get("file"))
            != original_file
            or companion.get("line") != primary["line"]
            or not isinstance(other_position, int)
            or isinstance(other_position, bool)
            or not 0 < other_position - position <= 40
        ):
            continue
        diagnostic = str(companion.get("message") or "")
        failed_call = re.match(
            r"no matching function for call to (['\"])([^'\"\n]+)\1", diagnostic
        )
        if not failed_call or not re.fullmatch(
            r"gert::TilingContext::GetTilingData\s*<\s*"
            + re.escape(symbol)
            + r"\s*>\s*\([^)]*\)",
            failed_call.group(2),
        ):
            continue
        companion_echo = companion.get("code_line")
        if isinstance(companion_echo, str):
            companion_visible = _cpp_visible_text(companion_echo, mask_literals=True)
            if not re.search(argument, companion_visible):
                continue
            if (
                len(re.findall(r"\b" + re.escape(symbol) + r"\b", companion_visible))
                != 1
            ):
                continue
            if visible is not None and re.sub(r"\s+", "", visible) != re.sub(
                r"\s+", "", companion_visible
            ):
                continue
        # An intervening source error elsewhere or a new build/command breaks
        # this local association, even if another compile later repeats the line.
        interrupted = any(
            isinstance(error.get("log_line"), int)
            and position < error["log_line"] < other_position
            and (
                (error.get("file_original") or error.get("file")) != original_file
                or error.get("line") != primary["line"]
            )
            for error in normalized.get("all_errors") or []
        )
        if interrupted or any(
            observation.get("value") == "boundary"
            and isinstance(observation.get("log_line"), int)
            and position < observation["log_line"] <= other_position
            and not re.search(
                r":\d+(?::\d+)?:\s*(?:fatal error|error|note):",
                str(observation.get("detail") or ""),
            )
            for observation in normalized.get("stage_observations") or []
        ):
            continue
        return {
            "kind": "unavailable_tiling_type_argument",
            "symbol": symbol,
            "file": primary["file"],
            "line": primary["line"],
            "call": echo if isinstance(echo, str) else diagnostic,
            "companion_log_line": companion.get("log_line"),
            "scope": "observed_compiler_diagnostics",
            "root_cause_verified": False,
        }
    return None


def _ascend_api_overload_evidence(
    normalized: dict[str, Any], facts: dict[str, Any]
) -> dict[str, Any] | None:
    """Project the common SDK declaration evidence into the legacy category."""
    primary = normalized.get("primary_error") or {}
    association = facts["sdk_association"]
    if (
        facts["mechanism"]["value"] != "overload_resolution_failure"
        or association["status"] != "log_bound_sdk_declaration"
        or association.get("namespace") != "AscendC"
    ):
        return None
    ref = next(r for r in association["evidence_refs"] if r["kind"] == "note")
    for note in normalized.get("notes") or []:
        if note.get("log_line") != ref["log_line"] or not re.search(
            r"\bcandidate\b.*(?:not viable|ignored)", str(note.get("message") or "")
        ):
            continue
        return {
            "kind": "ascend_api_candidate_rejected",
            "symbol": association["symbol"],
            "call": str(primary.get("code_line") or ""),
            "candidate_file": ref["file_original"],
            "candidate_line": ref["line"],
            "candidate_diagnostic": note.get("message"),
            "scope": "observed_compiler_diagnostic",
            "replacement_verified": False,
        }
    return None


def _undeclared_call_evidence(
    primary: dict[str, Any],
    context: dict[str, Any],
    facts: dict[str, Any],
    symbol: str,
) -> dict[str, Any] | None:
    """Require a call at the failed use; a variable name is not SDK evidence."""
    visible = _failed_line(primary, context) or ""
    original = str(primary.get("code_line") or visible)
    escaped = re.escape(symbol)
    if len(re.findall(r"\b" + escaped + r"\b", visible)) != 1:
        return None  # Multiple uses on one line do not identify the failed use.
    if not re.search(r"\b" + escaped + r"\s*(?:<[^;<>]*>)?\s*(?:\(|<<<)", visible):
        return None  # Includes member receivers, scalar arguments and missing types.
    evidence = {
        "kind": "undeclared_call",
        "symbol": symbol,
        "call": original,
        "scope": "failed_use",
        "sdk_association": False,
    }
    association = facts["sdk_association"]
    if association.get("symbol") != symbol:
        return evidence
    if association["status"] == "explicit_namespace_use":
        evidence["sdk_association"] = True
        evidence["sdk_evidence"] = "explicit_qualified_call"
        return evidence
    if association["status"] == "log_bound_sdk_declaration":
        ref = next(r for r in association["evidence_refs"] if r["kind"] == "note")
        evidence.update(
            sdk_association=True,
            sdk_evidence="compiler_declaration_note",
            declaration_file=ref["file_original"],
            declaration_line=ref["line"],
        )
    return evidence


def diagnose(
    normalized: dict[str, Any],
    context: dict[str, Any] | None = None,
    static_findings: dict[str, Any] | None = None,
    probes: dict[str, Any] | None = None,
) -> dict[str, Any]:
    context = context or {}
    static_findings = static_findings or {}
    probes = probes or {}
    primary = normalized.get("primary_error")
    diagnostic_facts = build_diagnostic_facts(normalized, context)
    category = primary.get("category", "unknown") if primary else "unknown"
    phase = primary.get("phase", "unknown") if primary else "unknown"

    # An empty normalized log is not a compile failure. Keep this state explicit
    # so static project hints cannot manufacture a configuration diagnosis.
    all_errors = normalized.get("all_errors") or []
    no_failure_signal = (
        primary is None and not all_errors and normalized.get("status") != "failed"
    )

    findings = static_findings.get("findings") or []
    ranked_findings = sorted(
        findings,
        key=lambda f: (
            FINDING_PRIORITY.get(f.get("rule_id"), 50),
            1 if f.get("severity") == "error" else 0,
        ),
        reverse=True,
    )

    # Environment findings describe diagnostic readiness and must not replace
    # concrete compiler or linker errors already present in the normalized log.
    # A concrete error has a non-placeholder category, a file and line, or an
    # explicit driver-error kind.
    primary_has_concrete_error = bool(primary) and (
        category not in {"unknown", "unknown_compile", "package_error", "environment"}
        or (bool(primary.get("file")) and primary.get("line") is not None)
        or primary.get("kind") == "driver_error"
    )

    # A specific packaging failure may have no source location. It still takes
    # precedence over diagnostic-machine environment findings, while allowing
    # structural evidence to refine its category.
    _pkg_concrete_vs_env = (
        category == "package_error"
        and normalized.get("status") == "failed"
        and len((primary or {}).get("message") or "") > 12
    )
    if (
        ranked_findings
        and ranked_findings[0].get("severity") == "error"
        and not no_failure_signal
    ):
        best = ranked_findings[0]
        best_cat = best.get("category")
        promote = category in {"unknown", "unknown_compile", "package_error"}

        # Concrete compiler/linker evidence takes precedence over static guesses.
        if primary_has_concrete_error:
            # Preserve the observed compiler or linker diagnostic.
            promote = False
        elif best_cat == "environment" and _pkg_concrete_vs_env:
            # Local environment readiness does not explain the recorded packaging failure.
            promote = False
        elif best_cat == "environment":
            # With no concrete failure evidence, retain the existing fallback decision.
            pass
        elif not promote and best_cat in {
            "tiling_consistency",
            "cmake_config",
            "environment",
        }:
            promote = True
        if promote:
            category = best.get("category", category)
            phase = _phase_from_category(category, primary)

    if category == "cann_error_code":
        category = "unfriendly_error_code"
    if category == "compiler_internal_or_cann_internal":
        category = "compiler_internal"

    classification_evidence = []
    if primary and category in {"unknown", "unknown_compile", "tiling_consistency"}:
        visibility = _local_type_visibility_evidence(primary, context)
        if visibility:
            category = "header_missing"
            classification_evidence.append(visibility)
    if category in {"unknown", "unknown_compile"}:
        tiling_use = _tiling_type_use_evidence(normalized, diagnostic_facts)
        if tiling_use:
            category = "tiling_consistency"
            classification_evidence.append(tiling_use)
    if category == "template_error":
        overload = _ascend_api_overload_evidence(normalized, diagnostic_facts)
        if overload:
            category = "api_incompatible"
            classification_evidence.append(overload)

    # Classify from observed diagnostics and source structure.
    # Dataset metadata (including mutation descriptions) is not build evidence
    # and must not override category, phase or the absence of a failure.
    phase = _phase_from_category(category, primary)
    if classification_evidence and (primary or {}).get("phase") in {
        "host_compile",
        "kernel_compile",
    }:
        phase = primary["phase"]

    # Preserve supplied probe interpretations for inspection, not classification.
    # Even a same-input optimization split or successful preprocessing does not
    # establish a compiler limit, a backend failure or the cause of this build.
    probe_interp = probes.get("interpretation") or {}

    # Undeclared variables and project helpers do not establish an SDK mismatch.
    # Preserve unknown unless the failed use supplies call/domain evidence.
    if category in {"unknown", "unknown_compile"} and primary:
        msg = primary.get("message") or ""
        lower = msg.lower()
        if "undeclared identifier" in lower:
            m = re.search(r"undeclared identifier '([^']+)'", msg)
            sym = m.group(1) if m else ""
            also_unknown_type = bool(sym) and any(
                f"unknown type name '{sym}'" in (e.get("message") or "")
                for e in normalized.get("all_errors", [])
            )
            call_evidence = (
                _undeclared_call_evidence(primary, context, diagnostic_facts, sym)
                if sym and not also_unknown_type
                else None
            )
            if call_evidence and call_evidence["sdk_association"]:
                category = "api_incompatible"
                phase = _phase_from_category(category, primary)
                classification_evidence.append(call_evidence)

    candidates: list[dict[str, Any]] = []
    rank = 1
    candidate_findings = [
        f
        for f in ranked_findings
        if f.get("rule_id") != "cmake_missing_compile_commands"
        and not (
            f.get("category") == "environment"
            and (primary_has_concrete_error or _pkg_concrete_vs_env)
        )
    ]
    # Environment readiness belongs to the diagnosis machine; it does not explain
    # a concrete compiler/tool failure from the supplied log. Observability advice
    # is retained in static_findings.json, without consuming a root-cause slot.
    for f in candidate_findings[:5]:
        confidence = 0.88 if f.get("severity") == "error" else 0.62
        if primary and primary.get("category") == f.get("category"):
            confidence += 0.05
        candidates.append(
            {
                "rank": rank,
                "cause": f.get("message") or f.get("rule_id"),
                "confidence": round(min(confidence, 0.95), 2),
                "evidence": (f.get("evidence") or [])
                + (
                    [f"static_rule_checker: {f.get('rule_id')}"]
                    if f.get("rule_id")
                    else []
                ),
                "files": [{"path": f.get("file"), "line": f.get("line")}]
                if f.get("file")
                else [],
            }
        )
        rank += 1

    if primary:
        candidates.insert(
            0,
            {
                "rank": 1,
                "cause": primary.get("message", "Primary compiler error"),
                "confidence": primary.get("confidence", 0.72),
                "evidence": [primary.get("raw") or primary.get("message", "")],
                "files": [{"path": primary.get("file"), "line": primary.get("line")}]
                if primary.get("file")
                else [],
            },
        )
        for i, c in enumerate(candidates, 1):
            c["rank"] = i

    if not candidates:
        candidates.append(
            {
                "rank": 1,
                "cause": "No concrete primary error was found in the provided log.",
                "confidence": 0.35,
                "evidence": [],
                "files": [],
            }
        )

    # Locate upstream declaration candidates without changing the selected
    # primary diagnostic, category, or phase.
    root_localization = None
    try:
        from harness.engine.root_localizer import localize_root_cause

        loc = localize_root_cause(normalized, context, static_findings)
        if loc and loc.get("root_file"):
            root_localization = loc
            head = candidates[0]
            head["root_file"] = loc["root_file"]
            head["root_line"] = loc.get("root_line")
            head["localized_by"] = loc.get("method")
            cc = loc.get("collapsed_count") or 0
            collapse_msg = (
                f"collapsed {cc} same-symbol errors across {loc.get('collapsed_files')}"
                if cc >= 2
                else "1 matching error (no cascade to collapse)"
            )
            head.setdefault("evidence", []).append(
                f"cross-file root: {loc['root_file']} (symptom {loc.get('symptom_file')}); "
                + collapse_msg
            )
    except Exception:  # pragma: no cover - 定位失败不应阻断诊断
        root_localization = None

    is_internal = not no_failure_signal and category in {
        "compiler_internal",
        "compiler_limit",
    }
    # Callers decide changes from source and requirement evidence. Compilation
    # success alone does not validate the complete task contract.
    next_actions = [
        (
            "top_suggestion 的 action 与 compiler_notes 是取证和修法线索，不生成代码补丁；"
            "结合 primary_error、源码声明与需求提出局部修改，反证出现时撤回失去依据的建议"
        ),
        (
            "若现有证据已足够，直接在源码上局部修改（不需要先写 patch 文件）；"
            "证据不足或冲突时，只补充判断所需的最小声明、调用或构建上下文"
        ),
        (
            "改完沿原构建配方与目标真编验证，可用 run_verify(case_dir=<project_dir>)；"
            "compile_passed / compile_failure_fixed 仅确认编译层结果，随后核对本任务契约；"
            "仅在具体补丁风险尚未排除时补对应回归"
        ),
        "若旧错误仍在或出现新错误，以本次完整日志继续诊断；不沿用过期首错推断，也不将新错误自动归因于补丁",
        "patch_file / --apply-patch 是可选项，只在你确实要验证一份独立补丁时才用",
    ]
    if is_internal and (
        (diagnostic_facts.get("mechanism") or {}).get("value")
        in {"compiler_crash", "unknown"}
    ):
        next_actions = [
            "先核更早的直接诊断、原命令和同一翻译单元输入；内部异常或弱探针不证明源码合法",
            "复用已有同状态证据，只在尚需区分具体假设时补一次受控探针或缩减；不固定遍历优化级别",
            "优化级别变化另记为配方变化，缩减物不作交付补丁；修复仍须按原任务目标验证",
        ]

    # Static findings cannot manufacture a compile failure when none was observed.
    if no_failure_signal and category in {"unknown", "unknown_compile"}:
        category = "no_compile_error"
        phase = "no_build_failure"
        # Echo the inspected log path so callers can verify the input used.
        _log_hint = (
            f"（本次检查的是 {normalized.get('log_path')}）"
            if normalized.get("log_path")
            else ""
        )
        next_actions = [
            f"未在 build log 中发现任何编译错误——请确认 build_log_path 指向真正失败的构建日志{_log_hint}",
            "若尚未编译，请先编译产生失败日志，再用 build_log_path 传入",
            "若构建其实已通过，则无需修复",
        ]
        candidates = [
            {
                "rank": 1,
                "cause": "No recognized compile error; this log alone does not establish the build outcome.",
                "confidence": 0.0,
                "evidence": [
                    f"normalized.status={normalized.get('status')!r}, primary_error=None, all_errors=0"
                ],
                "files": [],
            }
        ]

    final_candidates = candidates[:3]
    cause_tree = build_cause_tree(
        final_candidates,
        category=category,
        phase=phase,
        normalized=normalized,
        probes=probes,
        include_category_advice=False,
    )

    summary = _observed_mechanism_summary(diagnostic_facts) or _category_to_summary(
        category, phase
    )
    # Preserve available clues when the category is unknown.
    # The no_compile_error state retains its own summary and next actions.
    unknown_fallback_applied = False
    if category in {"unknown", "unknown_compile"}:
        fb = _build_unknown_fallback(primary, all_errors, root_localization)
        if fb is not None:
            summary, next_actions = fb
            unknown_fallback_applied = True
            usage_context = _unknown_usage_context(primary or {}, context)
            if usage_context:
                usages = _deleted_symbol_usages(
                    primary or {}, usage_context, all_errors, neutral=True
                )
                if usages:
                    next_actions.append(usages)

    # A file and line provide a local starting point, not proof of a local cause.
    # Keep richer secondary or upstream evidence when the unknown fallback found it.

    self_explanatory = (
        category != "no_compile_error"
        and not no_failure_signal
        and not unknown_fallback_applied
        and _detect_self_explanatory(primary, root_localization)
    )
    if self_explanatory:
        pe = primary or {}
        locus = f"{pe.get('file')}:{pe.get('line')}"
        # Use the local lead when evidence is sufficient; collect only the
        # minimum additional context needed to distinguish compile status from
        # task completion.
        next_actions = [
            (
                f"编译器在 {locus} 提供局部线索：结合报错附近的声明、语法上下文与当前完整日志判断。"
                "本次未定位到项目内跨文件根因，不代表已排除跨文件原因，也不证明根因恰在报错行"
            ),
            (
                "top_suggestion 的 action 与 compiler_notes 提供局部修复线索；若现有源码与需求证据已足够，直接在源码上局部修改"
                "（不需要先写 patch 文件）；证据不足或冲突时，只补充判断所需的最小上下文"
            ),
            (
                "改完沿原构建配方与目标真编验证，可用 run_verify(case_dir=<project_dir>)；"
                "compile_passed / compile_failure_fixed 仅确认编译层结果，随后核对本任务契约；"
                "仅在具体补丁风险尚未排除时补对应回归"
            ),
            "若旧错误仍在或出现新错误，以本次完整日志继续诊断；不沿用过期首错推断，也不将新错误自动归因于补丁",
        ]

        # A missing symbol may require restoring a declaration or correcting a name.
        # A use-site location does not establish which change preserves the contract.
        if re.search(
            r"undeclared identifier|no template named|no type named",
            str(pe.get("message") or ""),
            re.IGNORECASE,
        ):
            next_actions.append(
                "未声明名字的原因仍需确认：先核当前使用点、作用域、声明可见性和生成输入。"
                "只有工程契约或定义记录支持时才恢复/改名；KB未命中不证明定义被删。"
                "有 SDK 归属证据时再核当前声明与实参，近名不证明可直接替换。"
            )

            # Include bounded source-use excerpts to constrain investigation when the
            # current source contains the missing symbol.
            usage_context = _unknown_usage_context(pe, context)
            usages = (
                _deleted_symbol_usages(pe, usage_context, all_errors, neutral=True)
                if usage_context
                else None
            )
            if usages:
                next_actions.append(usages)

        # A split host/kernel project with a missing generated header may need
        # additional generation and API-contract evidence after the first repair.
        cascade = _cascade_forewarning(primary, context)
        if cascade:
            next_actions.append(cascade)

    return {
        "summary": summary,
        "phase": phase,
        "category": category,
        "classification_evidence": classification_evidence,
        "diagnostic_facts": diagnostic_facts,
        **build_stage_evidence(normalized, context, legacy_phase=phase),
        "primary_error": primary,
        "root_cause_candidates": final_candidates,
        "root_cause_localization": root_localization,
        "self_explanatory": self_explanatory,
        "cause_tree": cause_tree,
        "is_compiler_internal_suspected": is_internal,
        "next_actions": next_actions,
        "probes_interpretation": probe_interp,
        "probes_interpretation_role": "observation_only",
        "status": normalized.get("status", "unknown"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Diagnose Ascend C compile failures from normalized inputs."
    )
    parser.add_argument("--normalized")
    parser.add_argument("--context")
    parser.add_argument("--static-findings")
    parser.add_argument("--probes")
    parser.add_argument("--case")
    parser.add_argument("--json-out", required=True)
    args = parser.parse_args()
    try:
        if args.case and not args.normalized:
            base = Path(args.case) / "logs"
            args.normalized = str(base / "normalized.json")
            args.context = str(base / "context.json")
            args.static_findings = str(base / "static_findings.json")
            args.probes = str(base / "probes.json")
        data = diagnose(
            load_json(args.normalized, default={}),
            load_json(args.context, default={}),
            load_json(args.static_findings, default={}),
            load_json(args.probes, default={}),
        )
    except Exception as exc:
        data = {
            "summary": "诊断引擎执行失败。",
            "phase": "unknown",
            "category": "unknown",
            "root_cause_candidates": [],
            "is_compiler_internal_suspected": False,
            "next_actions": [],
            **error_payload("diagnosis_engine", exc),
        }
    write_json(Path(args.json_out), data)
    print(str(Path(args.json_out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
