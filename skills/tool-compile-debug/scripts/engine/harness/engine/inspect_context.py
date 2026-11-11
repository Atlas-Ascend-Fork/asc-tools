# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Explicit bounded source/build/environment queries for the compile-repair host."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from harness.engine.build_context_collector import (
    build_facts,
    collect_context,
    select_compile_commands,
    build_metadata_reader,
    selected_build_directory,
)
from harness.engine.cause_tree import query_api_signatures
from harness.engine.common import (
    PROJECT_ROOT,
    error_payload,
    force_utf8_stdio,
    load_yaml,
    write_json,
)
from harness.engine.env_probe import detect_cann
from harness.engine.log_normalizer import normalize_log_text
from harness.engine.root_localizer import build_define_use_graph
from harness.engine.source_facts import SourceReader
from harness.engine import repair_memory
from harness.engine.common import load_json


def _evidence(
    result: dict,
    *,
    gaps=(),
    binding="not_requested",
    semantics: str,
    freshness="unknown",
    no_match=False,
) -> dict:
    gaps = [dict(x) for x in gaps]
    if binding in {"unbound", "ambiguous"}:
        gaps.append({"reason": "configuration_" + binding})
    # Deduplicate the shared metadata reader's gaps without losing path identity.
    gaps = list({json.dumps(x, sort_keys=True): x for x in gaps}.values())
    coverage = "no_match" if no_match else "partial" if gaps else "available"
    return {
        **result,
        "status": "completed" if coverage == "available" else coverage,
        "operation_status": "completed",
        "evidence": {
            "binding": binding,
            "coverage": coverage,
            "freshness": freshness,
            "semantics": semantics,
            "acceptance": "not_assessed",
            "gaps": gaps,
        },
    }


def _binding(commands: list, requested=True) -> str:
    if not requested:
        return "not_requested"
    return "selected" if len(commands) == 1 else "ambiguous" if commands else "unbound"


def _command_gaps(commands: list) -> list:
    gaps = [
        {
            "reason": "command_not_replayable",
            "configuration_id": row["configuration_id"],
            "unresolved": row["unresolved"],
        }
        for row in commands
        if not row["replayable"]
    ]
    for row in commands:
        gaps.extend(row.get("response_expansion", {}).get("gaps", []))
    return gaps


def inspect(
    project: Path,
    *,
    question: str,
    source: str | None = None,
    build_dir: Path | None = None,
    log: Path | None = None,
    max_files: int = 16,
    max_bytes: int = 262144,
    mechanism: str | None = None,
    target: str | None = None,
    configuration_id: str | None = None,
    max_metadata_files: int = 128,
    max_metadata_bytes: int = 4194304,
    symbol: str | None = None,
    namespace: str | None = None,
    api_level: str | None = None,
    cann_version: str | None = None,
    cann_build: str | None = None,
    target_arch: str | None = None,
    limit: int = 5,
    memory: Path | None = None,
    allow_roots=(),
) -> dict:
    if min(max_files, max_bytes, max_metadata_files, max_metadata_bytes) < 1:
        raise ValueError("read budgets must be positive")
    if question not in {"source", "build", "environment", "knowledge"}:
        raise ValueError("unknown question")
    if memory and (question != "knowledge" or symbol):
        raise ValueError("--memory is for mechanism questions only")
    if question != "knowledge" and any(
        (symbol, namespace, api_level, cann_version, cann_build, target_arch, mechanism)
    ):
        raise ValueError("knowledge filters require --question knowledge")
    result = {
        "schema_version": "2.0",
        "question": question,
        "project": str(project),
        "status": "completed",
        "build_invoked": False,
    }
    if question == "knowledge":
        if mechanism and symbol:
            raise ValueError("choose a mechanism or an API symbol, not both")
        if symbol:
            api = query_api_signatures(
                symbol,
                namespace=namespace,
                api_level=api_level,
                cann_version=cann_version,
                cann_build=cann_build,
                target_arch=target_arch,
                limit=limit,
            )
            return _evidence(
                {**result, "api": api},
                semantics="knowledge_candidates",
                no_match=not api["candidate_count"],
                gaps=[{"reason": r} for r in api["applicability_reasons"]],
            )
        if any((namespace, api_level, cann_version, cann_build, target_arch)):
            raise ValueError("API filters require --symbol")
        data = load_yaml(PROJECT_ROOT / "harness/knowledge/compile_mechanisms.yaml")
        cards = [
            x for x in data["mechanisms"] if mechanism is None or x["id"] == mechanism
        ]
        state = (
            repair_memory.advance(project, load_json(memory), allow_roots=allow_roots)
            if memory
            else None
        )
        return _evidence(
            {
                **result,
                "mechanisms": cards,
                "investigation": repair_memory.investigation(cards, state),
                "memory": repair_memory.summary(state) if state else None,
                "applicability": "requires_current_source_command_sdk_evidence",
            },
            semantics="knowledge_candidates",
            freshness="not_applicable",
            no_match=not cards,
        )
    if question == "environment":
        cann = detect_cann()
        return _evidence(
            {**result, "cann": cann, "device": "not_requested"},
            semantics="detected_environment",
            gaps=[] if cann.get("found") else [{"reason": "sdk_not_detected"}],
        )
    if question == "build":
        reader = build_metadata_reader(
            project,
            build_dir,
            max_files=max_metadata_files,
            max_bytes=max_metadata_bytes,
        )
        facts = build_facts(project, build_dir, metadata_reader=reader)
        selected = select_compile_commands(
            facts, target=target, configuration_id=configuration_id
        )
        return _evidence(
            {
                **result,
                "build": facts,
                "selected_commands": selected,
                "interpretation": "existing records; freshness and actual failure binding must be checked",
            },
            binding=_binding(selected, bool(target or configuration_id)),
            semantics="recorded_commands",
            gaps=facts["gaps"] + _command_gaps(selected),
        )
    primary = (
        normalize_log_text(log.read_text(encoding="utf-8", errors="replace")).get(
            "primary_error"
        )
        if log
        else {}
    )
    primary = primary or {}
    if source:
        scope = SourceReader(
            project,
            additional_roots=[selected_build_directory(project, build_dir)]
            if build_dir is not None
            else [],
        )
        resolved = scope.resolve(source)
        if resolved is None:
            return _evidence(
                {
                    **result,
                    "context": {
                        "primary_error": {
                            **primary,
                            "file": source,
                            "file_resolved": False,
                        },
                        "file_text": {},
                        "source_coverage": {"failed_source_loaded": False},
                        "read_evidence": {
                            "files_read": 0,
                            "bytes_read": 0,
                            "gaps": [
                                {"path": source, "reason": "missing_or_outside_scope"}
                            ],
                        },
                    },
                },
                semantics="lexical_candidates",
                binding="unbound",
                gaps=[{"path": source, "reason": "missing_or_outside_scope"}],
            )
        primary = {**primary, "file": str(resolved), "file_resolved": True}
    if not primary.get("file"):
        raise ValueError(
            "source query needs --file or a source diagnostic in --build-log"
        )
    context = collect_context(
        project,
        primary_error=primary,
        cann={"status": "not_requested"},
        build_dir=build_dir,
        focused=True,
        max_files=max_files,
        max_bytes=max_bytes,
        target=target,
        configuration_id=configuration_id,
        max_metadata_files=max_metadata_files,
        max_metadata_bytes=max_metadata_bytes,
    )
    graph = build_define_use_graph(context["file_text"])
    return _evidence(
        {
            **result,
            "context": context,
            "definitions": graph,
            "interpretation": "literal include/declaration candidates; not preprocessor visibility proof",
        },
        binding=context["configuration_status"],
        semantics="lexical_candidates",
        gaps=(
            context["read_evidence"]["gaps"]
            + context["build_facts"]["gaps"]
            + [
                g
                for g in context["metadata_read_evidence"]["gaps"]
                if g.get("purpose") != "response_file"
            ]
            + _command_gaps(context["selected_commands"])
        ),
    )


def summarize(result: dict) -> dict:
    """Short stdout protocol; exact hashes, sources and command records stay in --out."""
    summary = {
        k: result[k]
        for k in (
            "schema_version",
            "status",
            "operation_status",
            "question",
            "build_invoked",
            "error",
        )
        if k in result
    }
    if "evidence" in result:
        evidence = result["evidence"]
        summary["evidence"] = {k: v for k, v in evidence.items() if k != "gaps"}
        summary["evidence"]["gap_count"] = len(evidence["gaps"])
        summary["evidence"]["gap_reasons"] = sorted(
            {x["reason"] for x in evidence["gaps"]}
        )
    context = result.get("context", {})
    for name, full in (
        ("source_reads", context.get("read_evidence")),
        (
            "metadata_reads",
            context.get("metadata_read_evidence")
            or result.get("build", {}).get("metadata_read_evidence"),
        ),
    ):
        if full is not None:
            summary[name] = {
                k: full[k]
                for k in ("files_read", "bytes_read", "cache_hits")
                if k in full
            }
    if "api" in result:
        summary["api"] = {
            k: result["api"][k]
            for k in (
                "matching_symbol_count",
                "candidate_count",
                "filtered_count",
                "omitted_count",
                "version_resolution",
                "applicability",
                "absence_proven",
            )
        }
    if "investigation" in result:
        summary["investigation"] = [
            {
                "mechanism": x["mechanism"],
                "next_question": x["next_question"],
                "missing_roles": sorted(
                    {r for q in x["questions"] for r in q["missing_roles"]}
                ),
            }
            for x in result["investigation"]
        ]
    if result.get("memory"):
        summary["memory"] = result["memory"]
    return summary


def main() -> int:
    force_utf8_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument(
        "--question",
        choices=("source", "build", "environment", "knowledge"),
        default="source",
    )
    parser.add_argument("--mechanism")
    parser.add_argument(
        "--memory",
        type=Path,
        help="Optional evidence snapshot; revalidate recorded dependencies only",
    )
    parser.add_argument("--allow-root", type=Path, action="append", default=[])
    parser.add_argument("--target")
    parser.add_argument("--configuration-id")
    parser.add_argument("--symbol")
    parser.add_argument("--namespace")
    parser.add_argument("--api-level")
    parser.add_argument("--cann-version")
    parser.add_argument("--cann-build")
    parser.add_argument(
        "--target-arch", help="observed compiler architecture, not a guessed SoC family"
    )
    parser.add_argument("--limit", type=int, default=5)
    parser.add_argument("--file")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--build-log", type=Path)
    parser.add_argument("--max-files", type=int, default=16)
    parser.add_argument("--max-bytes", type=int, default=262144)
    parser.add_argument("--max-metadata-files", type=int, default=128)
    parser.add_argument("--max-metadata-bytes", type=int, default=4194304)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = inspect(
            args.project.resolve(),
            question=args.question,
            source=args.file,
            build_dir=args.build_dir,
            log=args.build_log,
            max_files=args.max_files,
            max_bytes=args.max_bytes,
            mechanism=args.mechanism,
            target=args.target,
            configuration_id=args.configuration_id,
            max_metadata_files=args.max_metadata_files,
            max_metadata_bytes=args.max_metadata_bytes,
            symbol=args.symbol,
            namespace=args.namespace,
            api_level=args.api_level,
            cann_version=args.cann_version,
            cann_build=args.cann_build,
            target_arch=args.target_arch,
            limit=args.limit,
            memory=args.memory,
            allow_roots=args.allow_root,
        )
    except (OSError, ValueError) as exc:
        result = {
            "schema_version": "2.0",
            "status": "error",
            "operation_status": "failed",
            **error_payload("inspect", exc),
        }
    write_json(args.out, result)
    # Keep stdout concise; exact source and full facts are in the requested artifact.
    print(json.dumps({**summarize(result), "out": str(args.out)}, ensure_ascii=False))
    return 1 if result["status"] == "error" else 0


if __name__ == "__main__":
    raise SystemExit(main())
