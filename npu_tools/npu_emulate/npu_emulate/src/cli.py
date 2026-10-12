#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

"""End-to-end CLI for npu-emulate.

This module ports the former ``npu-emulate.sh`` wrapper to Python so the whole
flow lives inside the installed package and is reachable through the single
``npu-emulate`` console entry point:

  npu-emulate collect --output DIR <exec-path> [--] [emulate args...]
  npu-emulate report  --input-db DB [--output DIR] [--] [emulate args...]

Both subcommands finish with "stage 3": export the analysis DB to CSV, copy the
generated JSON reports, and pack everything into a ``.npu-rep`` archive.
``collect`` additionally drives the external ``npusim record`` binary (shipped
with CANN, not with this package) to produce the contract DB first.

Everything after ``--`` is forwarded verbatim to the underlying
``npu_emulate report`` engine (``BiProfRunner``).
"""

import argparse
import gzip
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

from npu_emulate.src.runner import BiProfRunner
from npu_emulate.tools.export_csv import export_database
from npu_emulate.tools.pack_npu_rep import RepError, collect_entries, encode_rep

# Contract DB files produced by `npusim record` look like 0000_*.db.
DB_PATTERN = re.compile(r"^\d{4}_.*\.db$")

# npusim record is invoked for the Ascend950 SoC by default.
DEFAULT_SOC = "Ascend950"


def _usage() -> str:
    return (
        "Usage:\n"
        "  npu-emulate collect --output DIR <exec-path> [--] [emulate args...]\n"
        "  npu-emulate report  --input-db DB [--output DIR] [--] [emulate args...]\n"
    )


def _error(message: str) -> None:
    sys.stderr.write(f"ERROR: {message}\n")


def _run_emulate_report(db: str, report_dir: str, emulate_args: List[str]) -> int:
    """Stage 2: drive the npu_emulate report engine on a single contract DB."""
    print(f"==> npu_emulate report --input-db {db} --output {report_dir}")
    return BiProfRunner.run(
        ["report", "--input-db", db, "--output", report_dir, *emulate_args]
    )


def _run_stage3(db: str, report_dir: str) -> int:
    """Stage 3: CSV export + JSON copy + pack into a .npu-rep archive."""
    npu_rep_dir = os.path.join(report_dir, "npu-rep")
    if os.path.isdir(npu_rep_dir):
        shutil.rmtree(npu_rep_dir)
    os.makedirs(npu_rep_dir, exist_ok=True)

    # 3a. Export every non-empty DB table/view to CSV.
    print(f"==> export_csv --input-db {db} --output-dir {npu_rep_dir}")
    export_database(db, npu_rep_dir)

    # 3b. Copy JSON reports produced by stage 2 into the npu-rep dir.
    report_subdirs = [p for p in Path(report_dir).glob("*_reports") if p.is_dir()]
    for report_subdir in report_subdirs:
        for src in list(report_subdir.glob("*.json")) + list(
            report_subdir.glob("*.json.gz")
        ):
            if src.suffix == ".gz":
                dst = os.path.join(npu_rep_dir, src.with_suffix("").name)
                with gzip.open(src, "rb") as gz, open(dst, "wb") as out:
                    shutil.copyfileobj(gz, out)
            else:
                shutil.copy(src, npu_rep_dir)

    # 3c. Pack the npu-rep dir into <db-stem>.npu-rep.
    db_stem = Path(db).stem
    npu_rep_file = os.path.join(report_dir, f"{db_stem}.npu-rep")
    if os.path.lexists(npu_rep_file):
        os.remove(npu_rep_file)
    print(f"==> pack_npu_rep {npu_rep_dir} -> {npu_rep_file}")
    try:
        encoded = encode_rep(collect_entries(Path(npu_rep_dir)))
        with open(npu_rep_file, "xb") as out:
            out.write(encoded)
    except RepError as exc:
        _error(f"npu-rep-pack: {exc}")
        return 1
    print(f"[OK] packed {npu_rep_dir} -> {npu_rep_file}")
    print(f"     total size: {len(encoded)} bytes")
    return 0


def _find_latest_record(out_dir: str) -> Optional[str]:
    """Return the most recent ``record/`` directory under *out_dir*, or None."""
    records = []
    for root, dirs, _files in os.walk(out_dir):
        for name in dirs:
            if name == "record":
                records.append(os.path.join(root, name))
    if not records:
        return None
    records.sort()
    return records[-1]


def _collect_dbs(record_dir: str) -> List[str]:
    """Contract DBs inside *record_dir*, sorted; only the first is processed
    for now (drop the slice to process all)."""
    dbs = [
        os.path.join(record_dir, name)
        for name in os.listdir(record_dir)
        if DB_PATTERN.match(name) and os.path.isfile(os.path.join(record_dir, name))
    ]
    dbs.sort()
    return dbs[:1]


def cmd_collect(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(prog="npu-emulate collect", add_help=False)
    parser.add_argument("-o", "--output", dest="output")
    # Everything from the first positional onward up to "--" is the app to record
    # plus its arguments; everything after "--" is forwarded to npu_emulate report.
    known, remaining = parser.parse_known_args(argv)

    out_dir = known.output
    if not out_dir:
        _error("--output DIR is required")
        sys.stderr.write(_usage())
        return 1

    app_args: List[str] = []
    emulate_args: List[str] = []
    if "--" in remaining:
        sep = remaining.index("--")
        app_args = remaining[:sep]
        emulate_args = remaining[sep + 1 :]
    else:
        app_args = remaining

    if not app_args:
        _error("<exec-path> is required")
        sys.stderr.write(_usage())
        return 1

    exec_path = app_args[0]
    forwarded_app_args = app_args[1:]
    if not os.path.exists(exec_path):
        _error(f"exec-path not found: {exec_path}")
        return 1

    os.makedirs(out_dir, exist_ok=True)
    out_dir = os.path.abspath(out_dir)

    record_before = _find_latest_record(out_dir)

    # Stage 1: npusim record. App args are forwarded via -u as a single
    # shlex-split string, matching how npusim builds argv = [<app>] + shlex.split(user_option).
    npusim_args = ["record", exec_path, "-o", out_dir, "-s", DEFAULT_SOC]
    if forwarded_app_args:
        user_option = " ".join(forwarded_app_args)
        npusim_args += ["-u", user_option]
        print(f"==> npusim app args (-u): {user_option}")

    try:
        subprocess.run(["npusim", *npusim_args], check=True)
    except FileNotFoundError:
        _error("npusim not found on PATH (it ships with CANN, not this package)")
        return 1
    except subprocess.CalledProcessError as exc:
        return exc.returncode

    record_after = _find_latest_record(out_dir)
    if not record_after:
        _error(f"npusim did not produce a record/ dir under {out_dir}")
        return 1
    if record_after == record_before:
        print(f"WARN: no new record/ dir; reusing {record_after}")

    dbs = _collect_dbs(record_after)
    if not dbs:
        _error(f"no {DB_PATTERN.pattern} found under {record_after}")
        return 1

    run_dir = os.path.dirname(record_after)
    report_dir = os.path.join(run_dir, "report")
    os.makedirs(report_dir, exist_ok=True)

    ret = 0
    for db in dbs:
        ret = _run_emulate_report(db, report_dir, emulate_args)
        if ret != 0:
            return ret
        ret = _run_stage3(db, report_dir)
        if ret != 0:
            return ret
    return ret


def cmd_report(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(prog="npu-emulate report", add_help=False)
    parser.add_argument("--input-db", dest="input_db")
    parser.add_argument("-o", "--output", dest="output")
    known, remaining = parser.parse_known_args(argv)

    input_db = known.input_db
    if not input_db:
        _error("--input-db DB is required")
        sys.stderr.write(_usage())
        return 1
    if not os.path.isfile(input_db):
        _error(f"db not found: {input_db}")
        return 1

    # Everything after "--" is forwarded to the report engine; bare flags are
    # kept for the engine too (matching the previous shell behaviour).
    emulate_args: List[str] = []
    if "--" in remaining:
        sep = remaining.index("--")
        emulate_args = remaining[sep + 1 :]
    else:
        emulate_args = remaining

    if known.output:
        report_dir = known.output
    else:
        db_dir = os.path.abspath(os.path.dirname(input_db))
        if os.path.basename(db_dir) == "record":
            report_dir = os.path.join(os.path.dirname(db_dir), "report")
        else:
            report_dir = os.path.join(db_dir, "report")
    os.makedirs(report_dir, exist_ok=True)

    ret = _run_emulate_report(input_db, report_dir, emulate_args)
    if ret != 0:
        return ret
    return _run_stage3(input_db, report_dir)


def main(argv: Optional[List[str]] = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)

    if not args or args[0] in ("-h", "--help"):
        sys.stdout.write(_usage())
        return 0

    sub = args[0]
    rest = args[1:]
    if sub == "collect":
        return cmd_collect(rest)
    if sub == "report":
        return cmd_report(rest)

    _error(f"unknown subcommand: {sub}")
    sys.stderr.write(_usage())
    return 1
