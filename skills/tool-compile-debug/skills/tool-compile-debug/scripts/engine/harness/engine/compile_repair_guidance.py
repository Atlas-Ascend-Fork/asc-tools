# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Evidence-led compilation guidance, without source scans or generated edits.

Only consume supplied diagnosis and normalized compiler notes. The host owns the
repair; a compiler note is not proof of SDK availability or a valid replacement.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

_GUIDANCE = {
    "overload_resolution_failure": (
        "对照调用实参与编译器候选重载",
        "按候选note核实参数量、类型、地址空间及模板推导条件。区分调用与声明哪一侧偏离原任务；"
        "只有声明和用途支持时才修参数或显式模板实参，不因无匹配就补某个dtype、强制转换或删除参数。",
    ),
    "argument_count_mismatch": (
        "核对调用与声明的参数契约",
        "沿当前调用与对应声明核每个参数的用途，再选择修调用或声明。数量不符不证明SDK版本问题；"
        "不能只为匹配数量删除仍被实现或接口需要的参数。",
    ),
    "undeclared_identifier": (
        "定位报错名字的可见声明与使用",
        "核报错限定名、当前作用域、相关声明及更早的include/宏/生成错误。"
        "按可见定义与原接口决定改名方向；短名、路径或注释不证明SDK归属、版本移除或可删功能。",
    ),
    "unknown_type": (
        "核对类型声明的可见性",
        "核报错类型的限定名、声明入口、有效include/宏条件和当前使用。"
        "区分缺声明、错误名字与类型契约变化；不要仅凭名称猜一个替代类型。",
    ),
    "member_lookup_failure": (
        "对照实际对象类型与成员声明",
        "核当前对象/模板实例和所用成员，沿定义与消费方找契约差异。"
        "成员不可见不自动证明SDK移除或tiling生成错误，也不决定应改定义还是使用。",
    ),
    "syntax_error": (
        "检查首个语法错误附近的完整结构",
        "查看报错前后的声明、函数头与函数体、括号及条件宏；诊断位置可能晚于真正缺口。"
        "保留有效声明和实现，不能只按expected提示删掉报错行。",
    ),
    "header_lookup_failure": (
        "沿原构建配方核头文件来源与查找",
        "核该include所需声明、原搜索路径、生成真源及实际生成步骤。"
        "工程目录找不到头不证明它不存在或可删除；修复合法路径、入口或生成依赖后验证原目标。",
    ),
    "link_unresolved_symbol": (
        "对照未解析符号的声明、定义与链接输入",
        "核原目标实际链接的对象/库、符号限定名、签名和ABI；确认所需定义参与本次构建。"
        "不要通过移除调用或换目标绕过缺失符号。",
    ),
    "link_library_lookup_failure": (
        "核原目标的库名、搜索路径与依赖产物",
        "按失败链接命令核库的来源、架构、实际路径及依赖生成步骤。"
        "库尚未找到不支持直接删掉链接依赖。",
    ),
    "driver_option_rejection": (
        "核实际编译器与被拒绝的目标选项",
        "优先处理明确被拒绝的选项，核实际编译器版本、原target与参数来源。"
        "后续abort不抵消更早的driver诊断；修配方时保留已确定的目标，不先盲试优化级别。",
    ),
    "compiler_crash": (
        "确认崩溃前态与原翻译单元复现条件",
        "先查更早的有效诊断、完整原命令与源码输入；崩溃不证明源码合法。"
        "已有同状态复现证据时复用；只有具体假设仍需区分时才做受控探针或缩减，"
        "优化变化单列为配方变化，缩减物不作为交付补丁。",
    ),
    "static_assertion": (
        "沿实例化链核断言约束与配置来源",
        "读取失败断言及相关类型/配置生产者，按当前声明和任务核违反的条件。"
        "不要删除断言、猜隐藏配置值或更换算法来制造编译通过。",
    ),
    "conversion_failure": (
        "核转换两侧的类型与值/地址用途",
        "沿源类型、目标类型和调用声明定位不一致，保留dtype、布局、值或地址语义。"
        "不能仅靠cast或解引用消除报错；若修改影响行为，再按具体风险核补丁有效性。",
    ),
}


def _compiler_notes(
    normalized: dict[str, Any], primary: dict[str, Any]
) -> list[dict[str, Any]]:
    """Bounded excerpts from the same diagnostic block, without owner inference."""
    position = primary.get("log_line")
    if not isinstance(position, int) or isinstance(position, bool) or position < 1:
        return []
    if primary.get("diagnostic_block") != position:
        return []
    records = []
    for note in normalized.get("notes") or []:
        at = note.get("log_line")
        if (
            note.get("diagnostic_block") != position
            or not isinstance(at, int)
            or isinstance(at, bool)
            or at <= position
        ):
            continue
        message = str(note.get("message") or "")
        if not re.search(
            r"candidate|declared here|in instantiation|substitution|deduced conflicting|no (?:known|viable) conversion",
            message,
            re.I,
        ):
            continue
        record = {
            "kind": "compiler_note",
            "log_line": at,
            "log_line_space": "normalized",
            "file_original": note.get("file_original") or note.get("file"),
            "line": note.get("line"),
            "diagnostic": message[:360],
            "binding": "same_diagnostic_block",
        }
        if note.get("code_line"):
            record["source_excerpt"] = str(note["code_line"])[:240]
        if note.get("declaration_symbol"):
            record["declaration_symbol"] = note["declaration_symbol"]
        records.append(record)
        if len(records) == 3:
            break
    return records


_LP64_WIDTH_32_TYPES = frozenset(
    {
        "int32_t",
        "uint32_t",
        "std::int32_t",
        "std::uint32_t",
        "int",
        "signed",
        "signed int",
        "int signed",
        "unsigned",
        "unsigned int",
        "int unsigned",
    }
)
_LP64_WIDTH_64_TYPES = frozenset(
    {
        "int64_t",
        "uint64_t",
        "std::int64_t",
        "std::uint64_t",
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
        "size_t",
        "std::size_t",
        "ssize_t",
        "ptrdiff_t",
        "std::ptrdiff_t",
        "intptr_t",
        "uintptr_t",
        "std::intptr_t",
        "std::uintptr_t",
    }
)
_QUOTED_TYPE = r"['‘\"]([^'’\"\n]{1,120})['’\"]"
_AKA_SUFFIX_RE = re.compile(r"\s*\(aka\s+" + _QUOTED_TYPE + r"\)\s*$", re.I)
_TENSOR_WRAPPER_RE = re.compile(
    r"^(?:AscendC::)?(?:GlobalTensor|LocalTensor)\s*<\s*(.+?)\s*>$"
)
_DEDUCTION_CONFLICT_RE = re.compile(
    r"deduced conflicting types for parameter\s+(?:['‘\"][^'’\"\n]+['’\"]\s*)?"
    r"\(\s*"
    + _QUOTED_TYPE
    + r"(?:\s*\(aka\s+"
    + _QUOTED_TYPE
    + r"\))?\s+(?:vs\.?|and)\s+"
    + _QUOTED_TYPE
    + r"(?:\s*\(aka\s+"
    + _QUOTED_TYPE
    + r"\))?\s*\)",
    re.I,
)
_CONVERSION_PAIR_RE = re.compile(
    r"(?:(?:no known conversion(?:\s+for\s+[^\n'‘\"]+?)?|no viable conversion|invalid conversion)\s+from\s+|cannot convert\s+(?:from\s+)?)"
    + _QUOTED_TYPE
    + r"(?:\s*\(aka\s+"
    + _QUOTED_TYPE
    + r"\))?\s+to\s+"
    + _QUOTED_TYPE
    + r"(?:\s*\(aka\s+"
    + _QUOTED_TYPE
    + r"\))?"
    r"|cannot initialize[^\n:;]*?of type\s+"
    + _QUOTED_TYPE
    + r"(?:\s*\(aka\s+"
    + _QUOTED_TYPE
    + r"\))?\s+with an? [lr]value of type\s+"
    + _QUOTED_TYPE
    + r"(?:\s*\(aka\s+"
    + _QUOTED_TYPE
    + r"\))?",
    re.I,
)
_QUALIFIER_TOKENS = frozenset(
    {
        "const",
        "volatile",
        "restrict",
        "__restrict",
        "__restrict__",
        "__gm__",
        "__ubuf__",
        "__cbuf__",
        "__cc__",
        "__ca__",
        "__cb__",
        "__aicore__",
    }
)


def _strip_outer_qualifiers_and_refs(text: str) -> str:
    cur = text.strip()
    while cur:
        prev = cur
        cur = re.sub(r"[\s*&]+$", "", cur)
        lead = re.match(r"^([A-Za-z_]\w*)\b\s*", cur)
        if lead and lead.group(1) in _QUALIFIER_TOKENS:
            cur = cur[lead.end() :].strip()
        trail = re.search(r"\s*\b([A-Za-z_]\w*)$", cur)
        if trail and trail.group(1) in _QUALIFIER_TOKENS:
            cur = cur[: trail.start()].strip()
        if cur == prev:
            break
    return cur


def _type_integer_width(spelling: str | None) -> int | None:
    """Classify a complete C++ integer/pointer/tensor element type as 32- or 64-bit on LP64."""
    if not spelling:
        return None
    cleaned = _AKA_SUFFIX_RE.sub("", spelling).strip()
    while True:
        cleaned = _strip_outer_qualifiers_and_refs(cleaned)
        wrapped = _TENSOR_WRAPPER_RE.match(cleaned)
        if not wrapped:
            break
        cleaned = wrapped.group(1).strip()
    cleaned = re.sub(r"[*&]+", " ", cleaned)
    tokens = [tok for tok in cleaned.split() if tok not in _QUALIFIER_TOKENS]
    norm = " ".join(tokens)
    if norm in _LP64_WIDTH_32_TYPES:
        return 32
    if norm in _LP64_WIDTH_64_TYPES:
        return 64
    return None


def _pair_is_32_vs_64(
    left: str | None, left_aka: str | None, right: str | None, right_aka: str | None
) -> bool:
    w1 = _type_integer_width(left_aka) or _type_integer_width(left)
    w2 = _type_integer_width(right_aka) or _type_integer_width(right)
    return {w1, w2} == {32, 64}


def _has_integer_or_element_width_conflict(
    mechanism: str,
    primary: dict[str, Any],
    notes: list[dict[str, Any]],
    same_block_notes: list[dict[str, Any]] | None = None,
) -> bool:
    """Detect 32-bit vs 64-bit deduction or conversion conflicts from paired diagnostic types."""
    if mechanism not in {"overload_resolution_failure", "conversion_failure"}:
        return False
    diagnostics = [str(primary.get("message") or "")]
    for note in notes:
        diagnostics.append(str(note.get("diagnostic") or ""))
    for note in same_block_notes or []:
        diagnostics.append(str(note.get("message") or ""))
    for text in diagnostics:
        for match in _DEDUCTION_CONFLICT_RE.finditer(text):
            if _pair_is_32_vs_64(*match.groups()):
                return True
        for match in _CONVERSION_PAIR_RE.finditer(text):
            groups = match.groups()
            pair = groups[:4] if groups[0] is not None else groups[4:]
            if _pair_is_32_vs_64(*pair):
                return True
    return False


def suggest(
    diagnosis: dict[str, Any],
    normalized: dict[str, Any],
    case_dir: Path | None = None,
    soc: str | None = None,
) -> dict[str, Any]:
    """Return conditions and compiler evidence; never a generated code patch.

    case_dir/soc are echoed into the verification hint only (run_verify primitive);
    every other field stays a pure function of the supplied evidence with no
    project IO.
    """
    primary = diagnosis.get("primary_error") or {}
    if diagnosis.get("category") == "no_compile_error" or not primary:
        return {"schema_version": "compile-repair-guidance-v1", "suggestions": []}
    mechanism = ((diagnosis.get("diagnostic_facts") or {}).get("mechanism") or {}).get(
        "value", "unknown"
    )
    title, action = _GUIDANCE.get(
        mechanism,
        (
            "沿首个失败命令与原工程证据定位",
            "先核首错原文、实际失败命令和相关源码/构建依赖，区分直接错误与后续包装失败。"
            "已有证据足够就局部修复；不足时只补能区分当前假设的声明、调用或配方证据。",
        ),
    )
    # Keep an observed packaging failure useful without deriving a repair from
    # the broad package_error label. This is a conditional check, not a rename.
    if (
        mechanism == "unknown"
        and primary.get("kind") == "filename_too_long"
        and any(
            re.search(
                r"file\s*name (?:is )?too long", str(error.get("message") or ""), re.I
            )
            for error in normalized.get("all_errors") or []
        )
    ):
        title = "核原打包命令的文件名长度约束"
        action = (
            "日志包含文件名过长诊断；核失败产物路径、原打包格式及名称生成规则。"
            "只有确认当前格式的长度约束后才调整生成真源或兼容打包方式，"
            "并同步消费者引用、验证原打包目标；不要直接删除必需产物或只改生成文件。"
        )
    note_primary = normalized.get("primary_error") or {}
    same_primary = all(
        primary.get(key) == note_primary.get(key)
        for key in ("message", "log_line", "diagnostic_block")
    )
    notes = _compiler_notes(normalized, note_primary) if same_primary else []
    position = note_primary.get("log_line")
    same_block_notes = (
        [
            n
            for n in (normalized.get("notes") or [])
            if isinstance(position, int)
            and not isinstance(position, bool)
            and position >= 1
            and note_primary.get("diagnostic_block") == position
            and n.get("diagnostic_block") == position
            and isinstance(n.get("log_line"), int)
            and not isinstance(n.get("log_line"), bool)
            and n["log_line"] > position
        ]
        if same_primary
        else []
    )
    if _has_integer_or_element_width_conflict(
        mechanism, primary, notes, same_block_notes
    ):
        action = (
            action
            + " 检测到 32/64 位整数或元素位宽冲突：严禁未经核对在调用点盲目套 static_cast 窄化截断或强转指针；"
            "须先核对结构体/Tiling 同组相邻字段定义、上游生产计算表达式、多架构/Kernel 消费侧类型、"
            "格式化占位符及算子原型 dtype，按已证实的契约决定恢复 64 位声明还是调整调用侧。"
        )
    suggestion = {
        "rank": 1,
        "type": "diagnostic_guidance",
        "title": title,
        "risk": "unknown",
        "patch": "",
        "action": action,
        "mechanism": mechanism,
        "applicability": "conditional",
        "compiler_notes": notes,
        # 验证原语化：case_dir/soc 只回显进 verification 调用形状
        # （未传时用占位符，soc 仅在传入时追加），
        # 指导本体仍是所供证据的纯函数，不做任何工程 IO。
        "verification": (
            f"run_verify(case_dir={case_dir or '<case_dir>'}"
            f"{f', soc={soc}' if soc else ''}) 真编确认（一次性副本，不碰真实工程）；"
            "核实际编译与补丁契约。只有具体补丁风险尚未排除时才补对应回归。"
        ),
    }
    return {"schema_version": "compile-repair-guidance-v1", "suggestions": [suggestion]}
