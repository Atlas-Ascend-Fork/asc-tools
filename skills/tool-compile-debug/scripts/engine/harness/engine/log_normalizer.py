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
import hashlib
import re
import shlex
from pathlib import Path
from typing import Any

from harness.engine.common import PROJECT_ROOT
from harness.engine.common import error_payload
from harness.engine.common import guess_phase_from_file
from harness.engine.common import load_yaml
from harness.engine.common import write_json
from harness.engine.diagnostic_facts import _failed_target
from harness.engine.diagnostic_facts import _same_target
from harness.engine.stage_evidence import _visible_source
from harness.engine.stage_evidence import stage_observation
from harness.engine.version_gate import extract_log_version

COMPILER_RE = re.compile(
    r"^(?P<file>[^:\n][^:\n]*?):(?P<line>\d+)(?::(?P<column>\d+))?:\s*(?P<kind>fatal error|error|warning):\s*(?P<message>.*)$"
)
# Operator builds can wrap compiler diagnostics in TBE/plog prefixes.
# Expose the inner file:line diagnostic while excluding bracketed wrapper
# locations, which are logging sites rather than the compiler's source path.
_TBE_ENVELOPE_INNER_RE = re.compile(
    r"(?P<inner>[^:\s\[\]]+:\d+(?::\d+)?:\s*(?:fatal error|error|warning):\s*.*)$"
)


def _strip_opbuild_envelope(stripped: str) -> str:
    """剥 TBE/opbuild/plog 信封前缀，暴露内层编译器诊断行；非信封行原样返回。"""
    # 仅当行以信封标记开头时才剥（避免误伤普通诊断行的路径里恰含 ':error:' 等）
    if (
        stripped.startswith(("[ERROR]", "[WARNING]", "[INFO]"))
        or "[plog_print]" in stripped
        or "TBE(" in stripped
    ):
        m = _TBE_ENVELOPE_INNER_RE.search(stripped)
        if m:
            return m.group("inner").strip()
    return stripped


# CMake message blocks can split a GCC diagnostic into location, severity and
# message lines. Reflow these into one compiler diagnostic and normalize Unicode
# quotes; ordinary single-line diagnostics retain their existing structure.
_QUOTE_NORMALIZE = {0x2018: "'", 0x2019: "'", 0x201C: '"', 0x201D: '"'}
# 行尾即 `<path>:<line>[:<col>]:`（无后续 error:/warning: 文本）——GCC 把诊断位置单独成行的标志。
_BARE_FILELINE_RE = re.compile(
    r"^(?P<file>[^:\s][^:\n]*?):(?P<line>\d+)(?::(?P<col>\d+))?:\s*$"
)
# 续行的诊断关键字（裸 error:/warning:，无 file 前缀）。
_BARE_DIAG_RE = re.compile(r"^(?:fatal error|error|warning|note):\s*(?P<msg>.*)$")


def _reflow_cmake_message_block(text: str, spans: dict | None = None) -> str:
    """重组 CMake (message): 块内 GCC 多行式诊断为 COMPILER_RE 可解析的单行；归一 Unicode 引号。

    非块内文本原样保留；块内非诊断行（traceback、源码引用、`make: ***`）原样保留。
    """
    text = text.translate(_QUOTE_NORMALIZE)
    if "(message):" not in text and "opbuild run failed" not in text:
        return text  # 没有 opbuild/message 信封块，零开销跳过

    raw_lines = text.splitlines()
    out: list[str] = []
    i = 0
    n = len(raw_lines)
    while i < n:
        line = raw_lines[i]
        s = line.strip()
        fm = _BARE_FILELINE_RE.match(s)
        if fm:
            # 向下找最近的非空续行，若为裸 error:/warning: 则把位置+诊断+消息续行拼成单行。
            j = i + 1
            while j < n and not raw_lines[j].strip():
                j += 1
            if j < n:
                dm = _BARE_DIAG_RE.match(raw_lines[j].strip())
                if dm:
                    # 判定诊断 kind（COMPILER_RE 需 fatal error|error|warning）。
                    kind = raw_lines[j].strip().split(":", 1)[0]
                    msg_parts = [dm.group("msg").strip()]
                    k = j + 1
                    # 收集消息续行：缩进的、非空的、不是新 file:line:/新 error:/源码引用(`NN |`)/make 行。
                    while k < n:
                        cs = raw_lines[k].strip()
                        if not cs:
                            break
                        if (
                            _BARE_FILELINE_RE.match(cs)
                            or _BARE_DIAG_RE.match(cs)
                            or COMPILER_RE.match(cs)
                            or CMAKE_RE.search(cs)
                        ):
                            break
                        if re.match(r"^\d+\s*\|", cs) or cs.startswith(
                            ("|", "make", "In function", "In file")
                        ):
                            break
                        msg_parts.append(cs)
                        k += 1
                    col = f":{fm.group('col')}" if fm.group("col") else ""
                    merged = f"{fm.group('file')}:{fm.group('line')}{col}: {kind}: {' '.join(msg_parts).strip()}"
                    out.append(merged)
                    if spans is not None:
                        spans[i + 1] = [i + 1, k]
                        # Retain original line coordinates for notes and source echoes.
                        out.extend([""] * (k - i - 1))
                    i = k
                    continue
        out.append(line)
        i += 1
    return "\n".join(out)


CMAKE_RE = re.compile(r"(CMake Error at|CMake Error:)\s*(?P<message>.*)")
# Capture the CMake script and line separately from the message text.
CMAKE_AT_RE = re.compile(r"CMake Error at\s+(?P<file>[^:\n]+?):(?P<line>\d+)\b")
LINK_UNDEF_RE = re.compile(r"undefined reference to [`'\"“]?([^`'\"”]+)")
# LLD uses 'undefined symbol' where other linkers use 'undefined reference'.
# Retain the symbol itself for later localization.
LINK_UNDEF_LLD_RE = re.compile(
    r"\bld\.lld:\s*error:\s*undefined symbol:\s*(?P<sym>.+)", re.IGNORECASE
)
# Capture explicit target SoC/architecture evidence without treating the banner
# as a failure diagnostic.
SOC_VERSION_TOKEN_RE = re.compile(
    r"(?:SOC_VERSION\s*=\s*|--npu-arch=|npu[_-]?arch[^\w]+)(Ascend\w+|\d{3,4})",
    re.IGNORECASE,
)
# A build-target banner may encode a SoC token when no explicit option is logged.
# Restrict this heuristic to banners; arbitrary installation paths are not targets.
_CATLASS_TARGET_BANNER_RE = re.compile(
    r"Building target:\s*[\w./-]*?(?P<soc>ascend[0-9][a-z0-9]*?|kirin[0-9][a-z0-9]*?)(?=_|\b)",
    re.IGNORECASE,
)


def _canonical_soc_token(token: str) -> str:
    """Banner SoC token -> canonical SKU casing (ascend910b1 -> Ascend910B1).

    str.capitalize() lowercases the tail (Ascend910b1), which then misses the
    case-sensitive SoC alias table downstream and can resolve the wrong
    architecture family. Keep the family prefix, uppercase the numeric suffix
    tail; non-Ascend families use capitalize().
    """
    lowered = token.lower()
    if lowered.startswith("ascend"):
        return "Ascend" + lowered[len("ascend") :].upper()
    return token.capitalize()


# Recognize object and source prefixes on linker diagnostics.
LINK_FILE_RE = re.compile(
    r"^(?P<file>[^:\s][^:\n]*?\.(?:o|obj|a|so|asc|cpp|cc|cce))(?::(?P<line>\d+))?:"
)
CANNOT_FIND_LIB_RE = re.compile(
    r"cannot find -l(\S+)|unable to find library -l(\S+)|linker command failed",
    re.IGNORECASE,
)
NO_RULE_RE = re.compile(r"No rule to make target [`']?([^`',]+)")
# The missing make target provides an artifact path.
NO_RULE_TARGET_RE = re.compile(r"No rule to make target [`']?(?P<file>[^`',\s]+)")
CANN_CODE_RE = re.compile(r"\b[EWIZ1-9A-Z][0-9A-Z][0-9]{4}\b")
DOWNSTREAM_RE = re.compile(
    r"g?make(\[[0-9]+\])?: \*\*\* .* Error [0-9]+|ninja: build stopped"
)
JSON_NOT_GENERATED_RE = re.compile(
    r"\.json.*not generated|not generated.*\.json|failed to generate.*json",
    re.IGNORECASE,
)
# Retain a JSON artifact path mentioned by a packaging diagnostic.
JSON_FILE_RE = re.compile(r"(?P<file>[\w./-]+\.json)")
SCHEMA_RE = re.compile(
    r"generated code is stale|prototype .*schema|opcheck|host[_ -]?kernel.*arg|input_desc missing|required dtype|schema_consistency",
    re.IGNORECASE,
)
PACKAGE_RE = re.compile(
    r"msobjdump:.*no .*kernel|kernel metadata missing|package failed", re.IGNORECASE
)
# Declaration notes and include chains provide upstream source locations.
# Candidate functions, templates and constructors all supply declaration leads.
NOTE_DECL_RE = re.compile(
    r"^(?P<file>[^:\n]+?):(?P<line>\d+)(?::\d+)?:\s*note:\s*(?:'(?P<sym>[\w:]+)'\s+)?"
    r"(?P<msg>.*(?:declared here|defined here|previous declaration"
    r"|candidate (?:function|template|constructor)|candidate .*(?:ignored|not viable)"
    r"|conflicting types).*)$"
)
# An overload failure may depend on argument declarations as well as the called
# function. Retain argument identifiers for optional source localization.
NO_MATCH_FN_RE = re.compile(r"no matching function for call to '(?P<fn>[\w:]+)'")
NO_MATCH_ARGS_RE = re.compile(r"\b(?P<fn>[A-Za-z_]\w*)\s*\((?P<args>[^)]*)\)")
INCLUDE_FROM_RE = re.compile(
    r"^\s*In file included from\s+(?P<file>[^:\n]+?):(?P<line>\d+)"
)
_CARET_LINE_RE = re.compile(r"\s*(?:\|\s*)?[~ ]*\^[~^ ]*\s*")


def _source_echo(
    lines: list[str], diagnostic_line: int, source_line: int
) -> str | None:
    """Accept an adjacent compiler source excerpt, never the next diagnostic.

    Log positions are one-based in the normalized/reflowed text. A numbered GCC
    gutter or a following caret establishes excerpt layout; neither proves that
    the current checkout has the same contents as the logged source.
    """
    if diagnostic_line >= len(lines):
        return None
    candidate = lines[diagnostic_line].strip()
    if not candidate:
        return None
    gutter = re.match(r"^(\d+)\s*\|\s?(.*)$", candidate)
    if gutter:
        if int(gutter.group(1)) != source_line:
            return None
        candidate = gutter.group(2).strip()
    elif diagnostic_line + 1 >= len(lines) or not _CARET_LINE_RE.fullmatch(
        lines[diagnostic_line + 1]
    ):
        return None
    unwrapped = _strip_opbuild_envelope(candidate)
    if (
        not candidate
        or not (set(candidate) - set("^~ |"))
        or COMPILER_RE.match(unwrapped)
        or NOTE_DECL_RE.match(unwrapped)
        or re.match(r"^.*:\d+(?::\d+)?:\s*(?:note|remark):", unwrapped)
        or _BARE_DIAG_RE.match(unwrapped)
        or CMAKE_RE.search(unwrapped)
        or DOWNSTREAM_RE.search(unwrapped)
        or stage_observation(unwrapped, diagnostic_line + 1)
        or re.match(
            r"^(?:\[\s*\d+[%/]|--\s|In file included from\b|In function\b)", unwrapped
        )
        or re.match(
            r"^(?:(?:\S*/)?(?:cmake|ninja|g?make|bisheng|clang(?:\+\+)?|g\+\+|gcc|cc|c\+\+)(?:-\d+)?)\s+",
            unwrapped,
        )
    ):
        return None
    return candidate


def _simple_declaration_symbol(code_line: str | None) -> str | None:
    """Name in a simple logged declaration/head, not a complete signature.

    Compiler candidate excerpts often stop mid-parameter-list or show a definition
    head without its body. Accept a narrow builtin-return form for those excerpts;
    keep expressions, macros, bodies and complex declarators unresolved.
    """
    if not code_line:
        return None
    visible = _visible_source(code_line)
    if visible is None or any(c in visible for c in "#={}[]"):
        return None
    if re.match(
        r"\s*(?:return|co_return|throw|new|delete|using|typedef|if|else|while|for|switch|case|goto)\b",
        visible,
    ):
        return None
    match = re.fullmatch(
        r"\s*[\w:*&\s]+?\s+(?P<symbol>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)"
        r"\s*\([^();{}]*\)\s*(?:const\s*)?;\s*",
        visible,
    )
    if match:
        return match.group("symbol")
    # A truncated excerpt proves the declaration name only. Do not infer its
    # parameter count, complete overload or SDK origin from this partial line.
    head = re.fullmatch(
        r"\s*(?:template\s*<[\w:*&,\s]+>\s*)?"
        r"(?:(?:__aicore__|__host__|__device__|__global__|inline|__forceinline__|constexpr|static|extern|const|volatile|signed|unsigned|short|long)\s+)*"
        r"(?:void|bool|char|int|float|double|long)\s*[*&]*\s+"
        r"(?P<symbol>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\("
        r"[^();{}=\[\]#]*(?:\)\s*(?:const\s*)?;?)?\s*",
        visible,
    )
    return head.group("symbol") if head else None


def _category_from_message(message: str, kind: str = "") -> str:
    lower = message.lower()

    # Match compiler message forms here; linker diagnostics without file/line
    # prefixes are handled separately by the configured signature rules.
    if "calling a __host__ function" in lower and "from a __aicore__ function" in lower:
        return "kernel_compile_error"
    if re.search(
        r"cannot initialize a variable of type '__gm__ \w+ ?\*' with an lvalue of type 'gm_addr'",
        lower,
    ):
        return "kernel_compile_error"
    if "unknown type name 'half'" in lower and "did you mean 'half2'" in lower:
        return "kernel_compile_error"
    if "induction variable must have a type uint16_t" in lower:
        return "kernel_compile_error"
    if "static assertion failed" in lower and "tque's depth is zero" in lower:
        return "kernel_compile_error"

    # Use the legacy host-category fallback for unrecognized device qualifiers.
    if re.search(r"use of undeclared identifier 'event_id\d+'", lower):
        return "host_compile_error"
    # An undeclared local tiling variable is not sufficient evidence of a tiling
    # contract failure. Leave it unknown for source-based localization.
    if "soc_version" in lower and (
        "unsupported" in lower or "does not support" in lower
    ):
        return "environment"
    if any(
        token in lower
        for token in [
            "asc_dir",
            "ascend_home_path",
            "ascend_cann_package_path",
            "cann package",
        ]
    ) and any(
        marker in lower
        for marker in ["not set", "does not exist", "not found", "no such"]
    ):
        return "environment"
    if ("no such file or directory" in lower or "file not found" in lower) and (
        "fatal" in kind or "fatal error" in lower
    ):
        # The requested header path does not establish a CMake failure. A missing
        # generated file, include search path or dependency remains to be checked.
        return "header_missing"
    if any(
        token in lower
        for token in [
            "unknown type name '__aicore__'",
            "unknown type name 'gm_addr'",
            "undeclared identifier 'ascendc'",
            "use of undeclared identifier 'ascendc'",
        ]
    ):
        # A compiler spelling suggestion supplies a namespace/API-name lead before
        # the generic missing-header fallback. Verify declarations before applying it.
        if re.search(r"did you mean ['\"]ascendc['\"]", lower):
            return "api_incompatible"
        return "header_missing"
    # ACL-prefixed undeclared symbols provide a host-header visibility lead.
    # Keep exact identifier casing and leave more specific AscendC/tiling checks first.
    if re.search(
        r"(unknown type name|undeclared identifier) '(acl[A-Za-z]\w*|ACL_[A-Z0-9_]+)'",
        message,
    ):
        return "header_missing"
    # Registration namespaces and types can be unavailable when framework headers
    # are absent. A spelling suggestion instead takes the API-name fallback; neither
    # message alone establishes the required repair.
    if (
        re.search(
            r"'(?:ge|gert)(?:::[\w:]+)?' (?:does not name a type|was not declared)",
            lower,
        )
        or re.search(r"'[\w:]*' in namespace '(?:ge|gert)' does not name a type", lower)
        or re.search(r"'(?:ge|gert)::\w+'", message)
        or re.search(r"(?:unknown type name|undeclared identifier) 'OpDef\w*'", lower)
    ):
        if re.search(r"did you mean", lower):
            return "api_incompatible"
        return "header_missing"
    # Apply the legacy tiling-category heuristic to unknown tiling-named types.
    if "unknown type name" in lower and re.search(
        r"unknown type name '\w*tiling\w*'", lower
    ):
        return "tiling_consistency"
    # GCC's 'was not declared' also describes local values. A tiling-like name
    # alone does not establish a type or a Host/Kernel data-contract failure.
    # An explicit argument-count mismatch supplies a signature-compatibility lead.
    if (
        "too few arguments to function call" in lower
        or "too many arguments to function call" in lower
    ):
        return "api_incompatible"
    if "did you mean 'ascendc'" in lower or 'did you mean "ascendc"' in lower:
        return "api_incompatible"
    if "no member named" in lower and "tiling" in lower:
        return "tiling_consistency"
    # A missing member in the AscendC namespace supplies an API-compatibility lead.
    # Tiling-specific messages retain their earlier category.
    if "no member named" in lower and (
        "namespace 'ascendc'" in lower or "in namespace 'ascendc'" in lower
    ):
        return "api_incompatible"
    if "get_tiling_data" in lower:
        return "tiling_consistency"
    # After exact missing-header tokens, retain a namespace/API-name category for
    # undeclared identifiers that contain the AscendC naming stem.
    if ("undeclared identifier" in lower) and "ascendc" in lower:
        return "api_incompatible"
    if "excess elements in struct initializer" in lower:
        return "tiling_consistency"
    # Generic C++ syntax, type and constant-expression diagnostics use the legacy
    # host-category fallback only after the more specific CANN message checks.

    # Recognize specialized static-assert messages before the generic C++ fallback.

    # Specific assertion wording supplies a more precise category than the
    # common 'static assertion failed' prefix.
    if "only support" in lower and "in fp8_e8m0_t dtype" in lower:
        return "kernel_compile_error"
    if "l1_tile_m == l0_tile_m" in lower:
        return "compiler_limit"
    if "dependent_false<" in lower:
        return "api_incompatible"
    if any(
        p in lower
        for p in [
            "expected ';'",
            "expected unqualified-id",
            "redefinition of",
            "static assertion failed",
            "cannot be narrowed",
            "must be initialized by a constant expression",
            # GCC can report both comma and semicolon as expected delimiters.
            "expected ',' or ';'",
            "expected ',' or ';' before",
            # Other malformed declaration tokens belong to the same syntax-error family.
            "expected identifier",
        ]
    ):
        return "host_compile_error"
    # Device functions reject some C++ constructs. Match the compiler's general
    # 'cannot use ... in [aicore]' wording rather than one prohibited keyword.
    if "cannot use '" in lower and ("[aicore]" in lower or "aicore function" in lower):
        return "kernel_compile_error"
    if "cannot use 'throw'" in lower or 'cannot use "throw"' in lower:
        return "kernel_compile_error"
    if "generated code is stale" in lower or "prototype json" in lower:
        return "schema_consistency"
    if "no viable conversion" in lower and "localtensor" in lower:
        return "template_error"
    if "no matching member function" in lower:
        return "template_error"
    if "undefined reference" in lower:
        return "link_error"
    if (
        "cannot find -l" in lower
        or "unable to find library -l" in lower
        or "linker command failed" in lower
    ):
        return "link_error"
    if (
        "internal compiler error" in lower
        or "segmentation fault" in lower
        or "crash" in lower
    ):
        return "compiler_internal"
    if "jump" in lower and "range" in lower:
        return "compiler_limit"
    if "template instantiation" in lower and (
        "exceeded maximum depth" in lower or "depth exceeds" in lower
    ):
        # Compiler depth/resource-limit wording takes precedence over generic
        # instantiation and template errors.
        return "compiler_limit"
    if "datacopy" in lower and "no matching function" in lower:
        return "api_incompatible"
    if any(
        x in lower
        for x in [
            "template",
            "instantiation",
            "no matching function",
            "ambiguous",
            "invalid conversion",
        ]
    ):
        return "template_error"
    if "cmake" in lower:
        return "cmake_config"
    if CANN_CODE_RE.search(message):
        return "unfriendly_error_code"
    return "unknown"


def _signature(message: str) -> str:
    signature = message
    signature = re.sub(r"(/[^/\s:]+)+", "<path>", signature)
    signature = re.sub(r"line \d+|\b\d+:\d+\b|\b\d+\b", "<num>", signature)
    signature = re.sub(r"'[^']+'", "'<symbol>'", signature)
    signature = re.sub(r'"[^"]+"', '"<symbol>"', signature)
    signature = re.sub(
        r"fatal error: .*: No such file or directory",
        "fatal error: <header>: No such file or directory",
        signature,
    )
    signature = re.sub(
        r"fatal error: .* file not found",
        "fatal error: <header> file not found",
        signature,
    )
    return signature.strip()


def _is_build_command_echo(line: str) -> bool:
    """Recognize command syntax before applying unlocated diagnostic signatures.

    Paths and arguments can contain words such as ``template``. They are not
    diagnostic messages. Located errors and explicit failure formats are parsed
    first; an unrecognized or malformed command remains available to the parser.
    """
    if re.search(
        r"\b(?:error|failed|failure):|"
        r"\b(?:failed|exited|returned)\b.*\b(?:status|code)\b",
        line,
        re.IGNORECASE,
    ):
        return False
    command = re.sub(r"^(?:\+|\$)\s+", "", line)
    command = re.sub(r"^\[\d+/\d+\]\s+", "", command)
    try:
        tokens = shlex.split(command)
    except ValueError:
        return False
    # Make/CMake often prefix a printed command with its working directory.
    if len(tokens) >= 5 and tokens[0] == "cd" and tokens[2] == "&&":
        tokens = tokens[3:]
    elif len(tokens) >= 6 and tokens[:2] == ["cd", "--"] and tokens[3] == "&&":
        tokens = tokens[4:]
    if len(tokens) < 2:
        return False
    executable = tokens[0].replace("\\", "/").rsplit("/", 1)[-1]
    if re.fullmatch(r"python(?:\d+(?:\.\d+)*)?(?:\.exe)?", executable):
        # A script invocation, including compiler launchers, has an explicit
        # script argument. Python diagnostics use a colon after the executable.
        script = next((token for token in tokens[1:] if not token.startswith("-")), "")
        return script.endswith(".py")
    if re.fullmatch(
        r"(?:bisheng|ccec|clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+|cmake|make|ninja)"
        r"(?:-\d+(?:\.\d+)*)?(?:\.exe)?",
        executable,
    ):
        return any(
            token.startswith(("-", "@"))
            or re.search(r"\.(?:c|cc|cpp|cxx|asc|o|obj)$", token)
            for token in tokens[1:]
        )
    return False


def normalize_log_text(text: str) -> dict[str, Any]:
    all_errors: list[dict[str, Any]] = []
    downstream_errors: list[str] = []
    noise: list[str] = []
    error_codes: list[str] = []
    notes: list[dict[str, Any]] = []  #: note 'declared here' 回指
    diagnostic_block: int | None = None
    excerpt_lines: set[int] = set()
    include_chain: list[dict[str, Any]] = []  #: In-file-included-from 链
    stage_observations: list[dict[str, Any]] = []
    stage_observations_omitted = 0
    first_driver_observation: dict[str, Any] | None = None
    target_soc: str | None = None  # 收尾-1(b): build.log 里的目标 SoC/arch token
    signature_rules = (
        load_yaml(PROJECT_ROOT / "harness/knowledge/error_signatures.yaml", default=[])
        or []
    )

    # Reflow wrapped multiline GCC diagnostics and normalize quotes before the
    # line parser selects the actual compiler errors.
    original_text = text
    spans: dict[int, list[int]] = {}
    text = _reflow_cmake_message_block(text, spans)

    cann_version = extract_log_version(text)
    lines = text.splitlines()
    for idx, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped:
            continue
        # Preserve the first target token found in explicit options or target banners.
        if target_soc is None:
            ms = SOC_VERSION_TOKEN_RE.search(stripped)
            if ms:
                target_soc = ms.group(1)
            else:
                mb = _CATLASS_TARGET_BANNER_RE.search(stripped)
                if mb:
                    target_soc = _canonical_soc_token(mb.group("soc"))
        # Unwrap recognized TBE/plog prefixes before matching compiler diagnostics.
        stripped = _strip_opbuild_envelope(stripped)
        # A declaration note only belongs to a contiguous diagnostic block.
        # Preserve recognized excerpts/carets and declaration notes; any other
        # nonempty line ends the association, including warnings and commands
        # whose executable or argument order this parser does not recognize.
        # Unknown lines are not evidence that two nearby notes share a cause.
        if (
            diagnostic_block is not None
            and idx not in excerpt_lines
            and not NOTE_DECL_RE.match(stripped)
        ):
            diagnostic_block = None
        excerpt_lines.discard(idx)
        observation = stage_observation(stripped, idx)
        if observation:
            if observation["value"] == "driver" and first_driver_observation is None:
                first_driver_observation = observation
            if len(stage_observations) < 100:
                stage_observations.append(observation)
            else:
                stage_observations_omitted += 1
        codes = CANN_CODE_RE.findall(stripped)
        for code in codes:
            if code not in error_codes:
                error_codes.append(code)

        if DOWNSTREAM_RE.search(stripped):
            downstream_errors.append(stripped)
            continue

        # Capture declaration notes and include chains separately from error rows.
        nm = NOTE_DECL_RE.match(stripped)
        if nm:
            note = {
                "file": nm.group("file"),
                "line": int(nm.group("line")),
                "sym": nm.group("sym"),
                "message": nm.group("msg"),
                "log_line": idx,
                "diagnostic_block": diagnostic_block,
            }
            echo = _source_echo(lines, idx, int(nm.group("line")))
            if echo is not None:
                note.update(
                    code_line=echo,
                    code_line_log_line=idx + 1,
                    declaration_symbol=_simple_declaration_symbol(echo),
                )
                excerpt_lines.add(idx + 1)
                if idx + 1 < len(lines) and _CARET_LINE_RE.fullmatch(lines[idx + 1]):
                    excerpt_lines.add(idx + 2)
            notes.append(note)
            continue
        im = INCLUDE_FROM_RE.match(stripped)
        if im:
            include_chain.append(
                {
                    "file": im.group("file"),
                    "line": int(im.group("line")),
                    "log_line": idx,
                }
            )
            continue

        m = COMPILER_RE.match(stripped)
        if m and m.group("kind") != "warning":
            file_name = m.group("file")
            message = m.group("message")
            category = _category_from_message(message, m.group("kind"))

            # Only rules explicitly marked claims_compiler_diagnostic may refine a
            # broad compiler category such as unknown or template_error.
            if category in ("unknown", "unknown_compile", "template_error"):
                claim = next(
                    (
                        r
                        for r in signature_rules
                        if r.get("claims_compiler_diagnostic")
                        and r.get("regex")
                        and r.get("category") not in {"downstream_error"}
                        and re.search(r["regex"], message, flags=re.IGNORECASE)
                    ),
                    None,
                )
                if claim:
                    category = claim.get("category", category)
            phase = guess_phase_from_file(file_name)

            # Retain an adjacent source excerpt for localization; the caret line alone
            # contains no consumed identifiers.
            code_line = _source_echo(lines, idx, int(m.group("line")))
            diagnostic_block = idx
            if code_line is not None:
                excerpt_lines.add(idx + 1)
                if idx + 1 < len(lines) and _CARET_LINE_RE.fullmatch(lines[idx + 1]):
                    excerpt_lines.add(idx + 2)
            all_errors.append(
                {
                    "raw": stripped,
                    "file": file_name,
                    "line": int(m.group("line")),
                    "column": int(m.group("column")) if m.group("column") else None,
                    "kind": m.group("kind").replace(" ", "_"),
                    "message": message,
                    "code_line": code_line,
                    "phase": phase,
                    "category": category,
                    "log_line": idx,
                    "diagnostic_block": diagnostic_block,
                }
            )
            continue

        cm = CMAKE_RE.search(stripped)
        if cm:
            # Only rules marked claims_cmake_envelope may refine a CMake wrapper category.
            # Match against its indented body as well as its heading, preserving genuine
            # configuration diagnostics when no more specific rule claims the block.
            block_text = stripped
            if idx < len(lines):
                body = [stripped]
                for follow in lines[idx : idx + 39]:
                    text_follow = follow.strip()
                    if not text_follow:
                        continue  # 段内空行：块体可跨段，不终止
                    if follow.startswith((" ", "\t")) or text_follow.startswith("--"):
                        body.append(text_follow)
                        continue
                    break  # 非缩进非空行（下一命令/文件行）才终止
                block_text = "\n".join(body)
            claim = next(
                (
                    r
                    for r in signature_rules
                    if r.get("claims_cmake_envelope")
                    and r.get("regex")
                    and re.search(r["regex"], block_text, flags=re.IGNORECASE)
                ),
                None,
            )
            if claim:
                at = CMAKE_AT_RE.search(stripped)
                all_errors.append(
                    {
                        "raw": block_text,
                        "file": at.group("file") if at else None,
                        "line": int(at.group("line")) if at else None,
                        "column": None,
                        "kind": claim.get("id", "claimed_cmake_error"),
                        "message": block_text,
                        "phase": claim.get("phase", "cmake_configure"),
                        "category": claim.get("category", "cmake_config"),
                        "log_line": idx,
                    }
                )
                continue
            category = _category_from_message(stripped)
            if category == "unknown":
                category = "cmake_config"
            at = CMAKE_AT_RE.search(stripped)  #: 派生 file:line
            all_errors.append(
                {
                    "raw": stripped,
                    "file": at.group("file") if at else None,
                    "line": int(at.group("line")) if at else None,
                    "column": None,
                    "kind": "cmake_error",
                    "message": stripped,
                    "phase": "cmake_configure",
                    "category": category,
                    "log_line": idx,
                }
            )
            continue

        if (
            LINK_UNDEF_RE.search(stripped)
            or LINK_UNDEF_LLD_RE.search(stripped)
            or CANNOT_FIND_LIB_RE.search(stripped)
        ):
            lf = LINK_FILE_RE.match(stripped)  #: 链接错误归属 obj/源文件
            lld = LINK_UNDEF_LLD_RE.search(stripped)
            err = {
                "raw": stripped,
                "file": lf.group("file") if lf else None,
                "line": int(lf.group("line")) if (lf and lf.group("line")) else None,
                "column": None,
                "kind": "link_error",
                "message": stripped,
                "phase": "link",
                "category": "link_error",
                "log_line": idx,
            }
            if lld:
                err["symbol"] = lld.group("sym").strip()
            all_errors.append(err)
            continue

        if NO_RULE_RE.search(stripped):
            nr = NO_RULE_TARGET_RE.search(stripped)  #: 缺失目标即归属文件
            all_errors.append(
                {
                    "raw": stripped,
                    "file": nr.group("file") if nr else None,
                    "line": None,
                    "column": None,
                    "kind": "build_rule_error",
                    "message": stripped,
                    "phase": "cmake_generate",
                    "category": "cmake_config",
                    "log_line": idx,
                }
            )
            continue

        if JSON_NOT_GENERATED_RE.search(stripped):
            jf = JSON_FILE_RE.search(stripped)  #: 缺失 .json 即归属文件
            all_errors.append(
                {
                    "raw": stripped,
                    "file": jf.group("file") if jf else None,
                    "line": None,
                    "column": None,
                    "kind": "package_error",
                    "message": stripped,
                    "phase": "package",
                    "category": "package_error",
                    "log_line": idx,
                }
            )
            continue

        if SCHEMA_RE.search(stripped):
            all_errors.append(
                {
                    "raw": stripped,
                    "file": None,
                    "line": None,
                    "column": None,
                    "kind": "schema_error",
                    "message": stripped,
                    "phase": "host_compile",
                    "category": "schema_consistency",
                    "log_line": idx,
                }
            )
            continue

        if PACKAGE_RE.search(stripped):
            all_errors.append(
                {
                    "raw": stripped,
                    "file": None,
                    "line": None,
                    "column": None,
                    "kind": "package_error",
                    "message": stripped,
                    "phase": "package",
                    "category": "package_error",
                    "log_line": idx,
                }
            )
            continue

        lower = stripped.lower()

        # Keep backend failure details as compiler-internal evidence. The accompanying
        # frontend/backend command-failed wrapper is a fallback when no detail is present.
        if (
            "internal compiler error" in lower
            or "segmentation fault" in lower
            or "out of jump/jumpc imm range" in lower
            or "error in backend:" in lower
            or "cannot select" in lower
            or re.search(
                r"(clang )?(frontend|backend) command failed with exit code", lower
            )
        ):
            all_errors.append(
                {
                    "raw": stripped,
                    "file": None,
                    "line": None,
                    "column": None,
                    "kind": "compiler_internal",
                    "message": stripped,
                    "phase": "kernel_compile",
                    "category": "compiler_internal",
                    "log_line": idx,
                }
            )
            continue

        # Verbose builds print commands containing arbitrary project paths and
        # option values. Do not classify those words as unlocated diagnostics.
        if _is_build_command_echo(stripped):
            continue
        if re.match(
            r"^(?:\[\s*\d+\s*%\]\s+(?:Building|Linking|Built|Generating)|"
            r"\[\d+/\d+\]\s+(?:Building|Linking|Generating)|--\s)",
            stripped,
        ) and not re.search(r"\b(?:error|failed|failure)\b", stripped, re.I):
            continue
        if re.match(r"^(?:warning|note|remark):", stripped, re.I):
            continue

        matched_rule = None
        for rule in signature_rules:
            regex = rule.get("regex")
            if regex and re.search(regex, stripped, flags=re.IGNORECASE):
                matched_rule = rule
                break
        if matched_rule and matched_rule.get("category") not in {"downstream_error"}:
            category = matched_rule.get("category", "unknown")
            if category == "cann_error_code":
                category = (
                    "compiler_internal_or_cann_internal"
                    if any(
                        re.match(r"\b[EWIZ1-9A-Z][0-9A-Z]9[0-9]{3}\b", c) for c in codes
                    )
                    else "unfriendly_error_code"
                )
            all_errors.append(
                {
                    "raw": stripped,
                    "file": None,
                    "line": None,
                    "column": None,
                    "kind": matched_rule.get("id", "matched_error"),
                    "message": stripped,
                    "phase": matched_rule.get("phase", "unknown"),
                    "category": category,
                    "log_line": idx,
                }
            )
        elif re.search(r"\berror\b", lower) and "warning" not in lower:
            noise.append(stripped)
            if re.search(
                r"(?:^|:\s*|\[)error(?:\s+[A-Z0-9_-]+)?\s*[:\]]", stripped, re.I
            ):
                all_errors.append(
                    {
                        "raw": stripped,
                        "message": stripped,
                        "file": None,
                        "line": None,
                        "column": None,
                        "kind": "driver_error"
                        if observation and observation["value"] == "driver"
                        else "unrecognized_error",
                        "phase": "unknown",
                        "category": "unknown",
                        "log_line": idx,
                    }
                )

    primary = choose_primary_error(all_errors)
    # An explicit driver failure remains a failure even without a parsed
    # source diagnostic.
    if primary is None:
        driver = first_driver_observation
        if driver:
            raw_driver = _strip_opbuild_envelope(lines[driver["log_line"] - 1].strip())
            primary = {
                "raw": raw_driver,
                "message": raw_driver,
                "file": None,
                "line": None,
                "column": None,
                "kind": "driver_error",
                "phase": "unknown",
                "category": "unknown",
                "log_line": driver["log_line"],
            }
            all_errors.append(primary)
    status = "failed" if all_errors or downstream_errors or error_codes else "unknown"
    if primary is None and error_codes:
        primary = {
            "message": f"CANN error code(s): {', '.join(error_codes)}",
            "file": None,
            "line": None,
            "column": None,
            "phase": "unknown",
            "category": "unfriendly_error_code",
            "confidence": 0.55,
            "kind": "cann_error_code",
            "raw": ", ".join(error_codes),
            "log_line": None,
        }

    if primary:
        primary = dict(primary)
        primary.setdefault("confidence", confidence_for(primary))
        # Attach a matched declaration note without changing the selected diagnostic.
        hint = _root_hint_from_notes(primary, notes)
        if hint:
            primary["root_hint"] = hint

        fix = extract_did_you_mean(primary)
        if fix:
            primary["did_you_mean"] = fix

    for entry in [
        *all_errors,
        *notes,
        *include_chain,
        *stage_observations,
        *([primary] if primary else []),
    ]:
        position = entry.get("log_line")
        if isinstance(position, int):
            entry["original_log_span"] = spans.get(position, [position, position])

    return {
        "schema_version": "2.0",
        "status": status,
        "execution_status": "unknown",
        "input_evidence": {
            "sha256": hashlib.sha256(original_text.encode("utf-8")).hexdigest(),
            "line_count": len(original_text.splitlines()),
            "presence": "present" if original_text.strip() else "empty",
            "completeness": "not_asserted",
        },
        "primary_error": primary,
        "error_codes": error_codes,
        "all_errors": all_errors,
        "downstream_errors": downstream_errors,
        "notes": notes,
        "include_chain": include_chain,
        "stage_observations": stage_observations,
        "stage_observations_omitted": stage_observations_omitted,
        "noise": noise[:100],
        "normalized_signature": _signature(
            primary["message"] if primary else "no_error"
        ),
        "target_soc": target_soc,
        "cann_version": cann_version,
    }


def _consumed_arg_symbols(primary: dict[str, Any]) -> list[str]:
    """Extract argument identifiers from the call site of a no-matching-function diagnostic for source localization."""
    text = " ".join(str(primary.get(k) or "") for k in ("code_line", "raw", "message"))
    fnm = NO_MATCH_FN_RE.search(text)
    if not fnm:
        return []
    fn = fnm.group("fn").split("::")[-1]
    # 找同名调用 fn(args) 的实参串
    for am in NO_MATCH_ARGS_RE.finditer(text):
        if am.group("fn").split("::")[-1] == fn and am.group("fn") != "call":
            args = am.group("args")
            syms = re.findall(r"[A-Za-z_]\w*", args)
            # 保留可能是 constexpr/类型（大写或含大写的标识符更可能是常量/别名），去重保序
            seen: list[str] = []
            for s in syms:
                if s not in seen and not s.isdigit():
                    seen.append(s)
            return seen
    return []


def _root_hint_from_notes(
    primary: dict[str, Any], notes: list[dict[str, Any]]
) -> dict[str, Any] | None:
    if not notes:
        return None
    msg = primary.get("message", "")
    pll = primary.get("log_line") or 0
    if (
        not isinstance(pll, int)
        or isinstance(pll, bool)
        or pll < 1
        or primary.get("diagnostic_block") != pll
    ):
        return None
    target = _failed_target(msg)
    bound_notes = []
    for note in notes:
        npos, source_line = note.get("log_line"), note.get("line")
        if (
            note.get("diagnostic_block") != pll
            or not isinstance(npos, int)
            or isinstance(npos, bool)
            or not 0 < npos - pll <= 6
            or not isinstance(source_line, int)
            or isinstance(source_line, bool)
            or source_line < 1
        ):
            continue
        identities = [
            note[key] for key in ("sym", "declaration_symbol") if note.get(key)
        ]
        if target and any(not _same_target(name, target) for name in identities):
            continue
        bound_notes.append(note)
    # 优先：note 的符号在 primary 消息里出现（最强证据）。
    for n in bound_notes:
        sym = n.get("sym")
        if sym and re.search(rf"\b{re.escape(sym)}\b", msg):
            return {
                "file": n["file"],
                "line": n.get("line"),
                "sym": sym,
                "source": "note_declared_here",
            }

    # A nearby candidate declaration can carry argument identifiers for further
    # localization; proximity alone does not prove the repair location.
    for n in bound_notes:
        if 0 <= (n.get("log_line", 0) - pll) <= 6:
            hint = {
                "file": n["file"],
                "line": n.get("line"),
                "sym": n.get("sym"),
                "source": "note_proximity",
            }
            if "no matching function" in msg.lower():
                hint["consumed_symbols"] = _consumed_arg_symbols(primary)
                hint["source"] = "no_matching_fn_candidate_note"
            return hint
    return None


def confidence_for(error: dict[str, Any]) -> float:
    category = error.get("category")
    kind = error.get("kind")
    if category in {
        "header_missing",
        "link_error",
        "cmake_config",
        "compiler_internal",
        "compiler_limit",
        "api_incompatible",
        "tiling_consistency",
        "host_compile_error",
        "template_error",
    }:
        return 0.9
    if kind in {"fatal_error", "cmake_error"}:
        return 0.86
    if category == "unfriendly_error_code":
        return 0.55
    return 0.72


# did-you-mean 的引号形态：clang 用 ASCII 'X'，GCC 用弯引号 ‘X’ 且常把建议折到下一行。
_Q = r"['\"‘’“”`]"
_SYM = r"[A-Za-z_][\w:]*"
# Recognize both Clang's leading diagnostic keyword and GCC's trailing
# 'was not declared' wording around the quoted identifier.
_DYM_HEAD_RE = re.compile(
    rf"""(?:(?:unknown\ type\ name|use\ of\ undeclared\ identifier|undeclared\ identifier
        |no\ member\ named|no\ template\ named|unknown\ type)
        \s*{_Q}(?P<wrong>{_SYM}){_Q}
      |{_Q}(?P<wrong2>{_SYM}){_Q}\s*(?:was\ not\ declared|is\ not\ a\ member|does\ not\ name))""",
    re.IGNORECASE | re.VERBOSE,
)
# GCC may say 'did you mean simply ...'. Skip the adverb so it cannot become
# the suggested identifier. Equal names are intentionally not emitted as renames.
_DYM_SUGG_RE = re.compile(
    rf"did\ you\ mean\s*(?:simply\s+)?{_Q}?(?P<right>{_SYM}){_Q}?",
    re.IGNORECASE | re.VERBOSE | re.DOTALL,
)

# A spelling suggestion is a candidate, not a proof of the intended contract.
# A suffixed alternative may reflect a renamed declaration; retain a caution
# rather than mechanically propagating that name into use sites.
_SUSPECT_SUFFIX = (
    "typo",
    "renamed",
    "old",
    "v2",
    "v3",
    "bak",
    "backup",
    "deprecated",
    "wrong",
    "bad",
    "tmp",
    "temp",
    "copy",
    "new",
)


def _looks_like_injected_name(wrong: str, right: str) -> bool:
    """Flag an extended-name suggestion for declaration/contract review."""
    lw, lr = wrong.lower(), right.lower()
    if lr.startswith(lw) and len(lr) > len(lw):
        tail = lr[len(lw) :].strip("_")
        if tail in _SUSPECT_SUFFIX or any(tail.startswith(s) for s in _SUSPECT_SUFFIX):
            return True
        return True  # Any suffix extension warrants checking the intended declaration.
    return False


def extract_did_you_mean(primary: dict[str, Any]) -> dict[str, Any] | None:
    """Extract the compiler's did-you-mean suggestion from the first diagnostic."""
    for text in (str(primary.get("message") or ""), str(primary.get("raw") or "")):
        if "did you mean" not in text.lower():
            continue
        sugg = _DYM_SUGG_RE.search(text)
        if not sugg:
            continue
        head = _DYM_HEAD_RE.search(text)
        wrong = (head.group("wrong") or head.group("wrong2")) if head else None
        right = sugg.group("right")
        if not wrong or not right or wrong == right:
            continue
        return {
            "wrong_symbol": wrong,
            "suggested_symbol": right,
            "file": primary.get("file"),
            "line": primary.get("line"),
            "source": "compiler_suggestion",
            "safe_to_apply": not _looks_like_injected_name(wrong, right),
        }
    return None


def choose_primary_error(errors: list[dict[str, Any]]) -> dict[str, Any] | None:
    if not errors:
        return None
    specific = [e for e in errors if e.get("kind") != "unrecognized_error"]
    if specific:
        errors = specific
    # If an explicit unsupported-option diagnostic accompanies a compiler crash,
    # prefer the option failure. A crash without that diagnostic remains primary.
    has_ice = any(
        e.get("kind") == "internal_compiler_error"
        or e.get("category") == "compiler_internal"
        for e in errors
    )
    if has_ice:
        rejection = [
            e
            for e in errors
            if e.get("category") == "unfriendly_error_code" and e.get("message")
        ]
        if rejection:
            return rejection[0]
    cmake_errors = [e for e in errors if e.get("phase") == "cmake_configure"]
    if cmake_errors:
        # An opbuild CMake message can forward a compiler failure. Prefer a concrete
        # non-CMake diagnostic when present; otherwise retain the CMake diagnostic.
        compile_errors = [
            e
            for e in errors
            if e.get("phase") != "cmake_configure"
            and e.get("kind") in {"fatal_error", "error"}
            and e.get("message")
        ]
        forwarder = cmake_errors[0]
        raw_lower = (forwarder.get("raw") or "").lower()
        is_forwarder = "func.cmake" in raw_lower and (
            "(message)" in raw_lower or "opbuild" in raw_lower
        )
        # A project CMake message may forward captured compiler/linker output.
        # Use a concrete diagnostic from its nearby body when available; otherwise
        # retain the configuration error.
        block_errors: list[dict[str, Any]] = []
        if not is_forwarder and "(message)" in raw_lower:
            head_line = forwarder.get("log_line")
            if isinstance(head_line, int):
                block_errors = [
                    e
                    for e in errors
                    if e.get("phase") != "cmake_configure"
                    and e.get("kind") in {"fatal_error", "error", "link_error"}
                    and e.get("message")
                    and isinstance(e.get("log_line"), int)
                    and 0 <= e["log_line"] - head_line <= 40
                ]
                if block_errors:
                    is_forwarder = True
        if not (is_forwarder and (compile_errors or block_errors)):
            return cmake_errors[0]

    real_errors = [
        e
        for e in errors
        if e.get("category") not in {"package_error"}
        and e.get("kind") not in {"package_error"}
        and e.get("message")
        and e.get("phase") != "cmake_configure"
    ]

    def is_tool_verdict(error: dict[str, Any]) -> bool:
        raw = str(error.get("raw") or error.get("message") or "").strip()
        # Signature-rule IDs describe message matches, not their producer. GCC
        # candidate notes and source echoes can match template rules too.
        if re.search(
            r":\d+(?::\d+)?:\s*(?:fatal error|error|warning|note|remark):", raw
        ) or re.match(r"^(?:\d+\s*\||\|)", raw):
            return False
        tool = re.match(r"^(?P<name>[A-Za-z_][\w.+-]*)(?:\[\d+\])?:\s+\S", raw)
        if tool:
            name = tool.group("name").lower()
            return not re.fullmatch(
                r"(?:error|warning|note|remark|candidate|template|bisheng|cceld|"
                r"ld(?:\.lld)?|clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+|cc1(?:plus)?)"
                r"(?:-\d+(?:\.\d+)*)?",
                name,
            )
        # Built-in schema checkers also emit standalone verdict messages without
        # a tool prefix. A keyword mentioned inside another line is insufficient.
        return error.get("kind") == "schema_error" and bool(SCHEMA_RE.match(raw))

    # A tool-verdict row with producer/message evidence can refine an unknown
    # primary. Ordinary compiler errors, notes and echoes retain their ordering.
    first_generic = next(
        (e for e in real_errors if e.get("kind") in {"fatal_error", "error"}), None
    )
    if first_generic is not None and first_generic.get("category") in {
        "unknown",
        "unknown_compile",
    }:
        verdict = [
            e
            for e in real_errors
            if e.get("kind")
            not in {"fatal_error", "error", "link_error", "build_rule_error"}
            and e.get("category")
            not in {
                None,
                "unknown",
                "unknown_compile",
                "package_error",
                "downstream_error",
            }
            and is_tool_verdict(e)
        ]
        if verdict:
            return verdict[0]
    for e in real_errors:
        if e.get("kind") in {"fatal_error", "error", "link_error", "build_rule_error"}:
            return e
    for e in real_errors:
        if e.get("category") in {
            "compiler_internal",
            "compiler_limit",
            "template_error",
            "unfriendly_error_code",
            "api_incompatible",
            "tiling_consistency",
            "host_compile_error",
        }:
            return e
    return errors[0]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Normalize compiler/build logs into JSON."
    )
    parser.add_argument("--log", required=True)
    parser.add_argument("--json-out", required=True)
    args = parser.parse_args()

    log_path = Path(args.log)
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        data = normalize_log_text(text)
        data["log_path"] = str(log_path)
    except Exception as exc:
        data = {
            "status": "error",
            "primary_error": None,
            "error_codes": [],
            "all_errors": [],
            "downstream_errors": [],
            "noise": [],
            "normalized_signature": "normalizer_error",
            "log_path": str(log_path),
            **error_payload("log_normalizer", exc, log_path=str(log_path)),
        }
    write_json(Path(args.json_out), data)
    print(str(Path(args.json_out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
