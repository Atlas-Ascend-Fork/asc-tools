# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Optional, bounded evidence memory. Records host decisions, never makes repairs.

File hashes prove identity, not the truth of a claim. Snapshots are immutable;
invalidation is local and propagates through explicitly declared dependencies.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import time
from pathlib import Path

from harness.engine.common import force_utf8_stdio, load_json
from harness.engine.verification_evidence import digest

CONTEXT_KEYS = {"sdk", "target", "configuration", "recipe"}
COLLECTIONS = ("facts", "hypotheses", "obligations", "collaborations")
MAX_FILES, MAX_BYTES = 64, 16 * 1024 * 1024


class EvidenceReader:
    def __init__(self, roots):
        self.roots = [Path(p).resolve() for p in roots]
        self.cache, self.bytes = {}, 0

    def read(self, name):
        path = Path(name).resolve()
        if not any(path == r or r in path.parents for r in self.roots):
            raise ValueError(f"evidence outside explicit roots: {path}")
        key = str(path)
        if key not in self.cache:
            if len(self.cache) >= MAX_FILES:
                raise ValueError("evidence file budget exceeded")
            try:
                with path.open("rb") as stream:
                    data = stream.read(MAX_BYTES - self.bytes + 1)
                self.bytes += len(data)
                if self.bytes > MAX_BYTES:
                    raise ValueError("evidence byte budget exceeded")
                value = hashlib.sha256(data).hexdigest()
            except OSError:
                value = None
            self.cache[key] = value
        return key, self.cache[key]


def _identity(row):
    # Re-recording identical evidence is not progress. Observation timestamps
    # remain in history but cannot refresh dependencies or reset loop detection.
    return digest(
        {k: v for k, v in row.items() if k not in {"status", "gaps", "recorded_ns"}}
    )


def _seal(state):
    return {**state, "state_id": digest(state)}


def _load(previous):
    state = copy.deepcopy(previous)
    identity = state.pop("state_id", None)
    if state.get("schema_version") != "repair-memory-v1" or identity != digest(state):
        raise ValueError("memory snapshot missing, changed or incompatible")
    return state


def _refresh(state, reader):
    facts, visiting, done = state["facts"], set(), set()

    def visit(name):
        if name in done:
            return facts[name]
        if name in visiting:
            raise ValueError("cyclic fact dependencies")
        visiting.add(name)
        fact = facts[name]
        gaps = []
        for path, expected in fact["files"].items():
            _, actual = reader.read(path)
            if actual is None or expected is None or actual != expected:
                gaps.append("file_missing_or_changed:" + path)
        for key, expected in fact["context"].items():
            if state["context"].get(key) != expected or expected is None:
                gaps.append("context_missing_or_changed:" + key)
        for parent, expected in fact["dependencies"].items():
            if (
                parent not in facts
                or visit(parent)["status"] != "current"
                or _identity(facts[parent]) != expected
            ):
                gaps.append("dependency_missing_or_changed:" + parent)
        if not (fact["files"] or fact["context"] or fact["dependencies"]):
            gaps.append("unbound_claim")
        fact.update(status="stale" if gaps else "current", gaps=gaps)
        visiting.remove(name)
        done.add(name)
        return fact

    for name in facts:
        visit(name)
    for collection in COLLECTIONS[1:]:
        for row in state[collection].values():
            gaps = [
                name
                for name, identity in row["dependencies"].items()
                if name not in facts
                or facts[name]["status"] != "current"
                or _identity(facts[name]) != identity
            ]
            row.update(status="stale" if gaps else "current", gaps=gaps)
            # 'current' is identity validity only; neither a hypothesis nor a
            # host-recorded obligation outcome becomes a tool verdict.
    return state


def advance(project: Path, previous=None, event=None, *, allow_roots=()):
    project = project.resolve()
    event = copy.deepcopy(event or {})
    if set(event) - {"context", "constraints", *COLLECTIONS, "attempt"}:
        raise ValueError("unknown memory event fields")
    state = (
        _load(previous)
        if previous
        else {
            "schema_version": "repair-memory-v1",
            "project": str(project),
            "context": {},
            "constraints": [],
            **{k: {} for k in COLLECTIONS},
            "attempts": [],
            "sequence": 0,
        }
    )
    if state["project"] != str(project):
        raise ValueError("memory belongs to a different project")
    reader = EvidenceReader([project, *allow_roots])
    if "context" in event:
        if (
            not isinstance(event["context"], dict)
            or set(event["context"]) - CONTEXT_KEYS
        ):
            raise ValueError("context accepts sdk/target/configuration/recipe only")
        if any(
            v is not None and (not isinstance(v, str) or len(v) > 4096)
            for v in event["context"].values()
        ):
            raise ValueError("context values must be short strings or null")
        # Replacement, not merge: an omitted current dimension is unknown.
        state["context"] = event["context"]
    if "constraints" in event:
        if state["sequence"] and event["constraints"] != state["constraints"]:
            raise ValueError(
                "task constraints cannot be silently relaxed; start a new task"
            )
        if not isinstance(event["constraints"], list) or not all(
            isinstance(x, str) for x in event["constraints"]
        ):
            raise ValueError("constraints must be a string array")
        state["constraints"] = event["constraints"]
    _refresh(state, reader)
    facts = state["facts"]
    rows = event.get("facts", [])
    if not isinstance(rows, list):
        raise ValueError("facts must be an array")
    ids = [x.get("id") for x in rows]
    if len(set(ids)) != len(ids):
        raise ValueError("duplicate fact ids in an event")
    pending = {}
    for row in rows:
        if set(row) - {
            "id",
            "claim",
            "files",
            "context_keys",
            "depends_on",
            "origin",
            "roles",
        }:
            raise ValueError("unknown fact fields")
        if not isinstance(row.get("id"), str) or not row["id"] or not row.get("claim"):
            raise ValueError("fact needs id and claim")
        if row.get("origin", "host_observation") not in {
            "host_observation",
            "tool_output",
            "knowledge",
            "inference",
        }:
            raise ValueError("unknown fact origin")
        if set(row.get("context_keys", [])) - CONTEXT_KEYS:
            raise ValueError("unknown context dimension")
        files = {}
        for name in row.get("files", []):
            path = Path(name)
            files.update([reader.read(path if path.is_absolute() else project / path)])
        pending[row["id"]] = {
            "id": row["id"],
            "claim": row["claim"],
            "origin": row.get("origin", "host_observation"),
            "files": files,
            "context": {
                k: state["context"].get(k) for k in row.get("context_keys", [])
            },
            "dependencies": {},
            "recorded_ns": time.time_ns(),
            "depends_on": row.get("depends_on", []),
            "roles": row.get("roles", []),
        }
    visiting = set()

    def bind(name):
        if name in visiting:
            raise ValueError("cyclic fact dependencies")
        if name in pending:
            visiting.add(name)
            row = pending[name]
            row["dependencies"] = {p: _identity(bind(p)) for p in row["depends_on"]}
            row.pop("depends_on")
            facts[name] = row
            del pending[name]
            visiting.remove(name)
        if name not in facts:
            raise ValueError("unknown fact dependency: " + str(name))
        return facts[name]

    for name in list(pending):
        bind(name)
    _refresh(state, reader)
    for collection in COLLECTIONS[1:]:
        for row in event.get(collection, []):
            required = {"id", "claim", "depends_on"}
            extra = (
                {"support", "refute", "next_question"}
                if collection == "hypotheses"
                else (
                    {"outcome"}
                    if collection == "obligations"
                    else {"skill", "stage", "applicability", "disposition"}
                )
            )
            if (
                set(row) - required - extra
                or not required <= set(row)
                or not row["id"]
                or not row["claim"]
            ):
                raise ValueError("invalid " + collection + " record")
            refs = (
                set(row["depends_on"])
                | set(row.get("support", []))
                | set(row.get("refute", []))
            )
            if not refs or refs - facts.keys():
                raise ValueError("decisions require known evidence facts")
            row["dependencies"] = {p: _identity(facts[p]) for p in sorted(refs)}
            row.pop("depends_on")
            if collection == "obligations" and row.get("outcome") not in {
                "pending",
                "host_reported_pass",
                "host_reported_fail",
                "blocked",
            }:
                raise ValueError(
                    "obligation outcome must be explicit and host-reported"
                )
            if collection == "collaborations":
                if row.get("stage") not in {
                    "discovered",
                    "loaded",
                    "executed",
                    "evidence_returned",
                }:
                    raise ValueError("unknown collaboration stage")
                if row.get("applicability") not in {
                    "unknown",
                    "applicable",
                    "conflict",
                    "inapplicable",
                }:
                    raise ValueError("unknown applicability")
                if row.get("disposition") not in {"candidate", "adopted", "rejected"}:
                    raise ValueError("unknown disposition")
                if row["disposition"] == "adopted" and (
                    row["stage"] != "evidence_returned"
                    or row["applicability"] != "applicable"
                    or any(facts[p]["status"] != "current" for p in refs)
                ):
                    raise ValueError(
                        "adoption needs returned, applicable, current evidence"
                    )
            state[collection][row["id"]] = row
    _refresh(state, reader)
    if "attempt" in event:
        attempt = event["attempt"]
        if (
            set(attempt) != {"question", "action", "outcome", "facts"}
            or not attempt["question"]
            or not attempt["action"]
        ):
            raise ValueError("attempt needs question/action/outcome/facts")
        if not attempt["facts"] or set(attempt["facts"]) - facts.keys():
            raise ValueError("attempt needs known evidence facts")
        if any(facts[p]["status"] != "current" for p in attempt["facts"]):
            raise ValueError(
                "attempt evidence is stale; refresh the affected observations"
            )
        if attempt["outcome"] not in {
            "progress",
            "no_progress",
            "infra_failure",
            "blocked",
        }:
            raise ValueError("unknown attempt outcome")
        state["attempts"].append(
            {
                **attempt,
                "binding": digest(
                    {
                        "context": state["context"],
                        "facts": {p: _identity(facts[p]) for p in attempt["facts"]},
                    }
                ),
            }
        )
    if sum(len(state[x]) for x in COLLECTIONS) > 512 or len(state["attempts"]) > 256:
        raise ValueError("task memory budget exceeded; narrow the task scope")
    state["sequence"] += 1
    state["parent_id"] = previous.get("state_id") if previous else None
    state["read_cost"] = {"files": len(reader.cache), "bytes": reader.bytes}
    return _seal(state)


def summary(state):
    attempts = state["attempts"]
    repeated = False
    if attempts:
        last = attempts[-1]
        repeated = last["outcome"] == "no_progress" and any(
            all(a[k] == last[k] for k in ("question", "action", "binding", "outcome"))
            for a in attempts[:-1]
        )
    return {
        "schema_version": state["schema_version"],
        "state_id": state["state_id"],
        "facts": {k: x["status"] for k, x in state["facts"].items()},
        "stale_decisions": {
            c: [k for k, v in state[c].items() if v["status"] != "current"]
            for c in COLLECTIONS[1:]
        },
        "repeat_without_new_evidence": repeated,
        "next_step": "change_discriminating_question_or_handoff"
        if repeated
        else "host_decides",
        "acceptance": "not_assessed",
        "build_invoked": False,
        "read_cost": state["read_cost"],
    }


def investigation(cards, state=None):
    """Expose discriminating questions; matching roles never prove semantics."""
    current = (
        {}
        if state is None
        else {k: v for k, v in state["facts"].items() if v["status"] == "current"}
    )
    plans = []
    for card in cards:
        questions = []
        for check in card.get("checks", []):
            refs = {
                role: [k for k, f in current.items() if role in f.get("roles", [])]
                for role in check["evidence_roles"]
            }
            missing = [k for k, v in refs.items() if not v]
            questions.append(
                {
                    **check,
                    "evidence_refs": refs,
                    "missing_roles": missing,
                    "status": "needs_evidence"
                    if missing
                    else "host_evidence_available",
                }
            )
        plans.append(
            {
                "mechanism": card["id"],
                "questions": questions,
                "next_question": next(
                    (x["question"] for x in questions if x["missing_roles"]), None
                ),
                "constraints": card["constraints"],
                "verification_obligation": card["verify"],
                "cannbot_candidates": card["cannbot"],
                "applicability": "host_must_check",
            }
        )
    return plans


def main():
    force_utf8_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--event", type=Path)
    parser.add_argument("--allow-root", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("output must be new; preserve earlier snapshots")
    try:
        state = advance(
            args.project,
            load_json(args.state) if args.state else None,
            load_json(args.event) if args.event else None,
            allow_roots=args.allow_root,
        )
        # Exclusive create also prevents concurrent writers overwriting a snapshot.
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(state, stream, ensure_ascii=False, indent=2)
        print(json.dumps({**summary(state), "out": str(args.out)}, ensure_ascii=False))
        return 0
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(
            json.dumps({"status": "error", "error": str(exc), "build_invoked": False})
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
