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

"""Export every table and view in a contract/analysis SQLite database to CSV.

For each table and view in the database a CSV file is written to the output
directory.  Empty tables/views are skipped (no CSV file is emitted) but
still appear in ``manifest.json`` with ``row_count`` 0 so the schema
remains visible to downstream consumers.  A ``manifest.json`` file records
metadata (name, type, row count, columns) for every exported object.

Cell value conventions:
  * ``NULL``   -> empty string
  * ``BLOB``   -> ``0x``-prefixed lower-case hex string
  * other types are rendered by :mod:`csv` as-is (ints, floats, text).

Usage:
    python export_csv.py --input-db <db_path> --output-dir <csv_dir>

Only the Python standard library is required.
"""

import argparse
import csv
import json
import math
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path


def _quote_ident(name):
    """Quote a SQL identifier with double quotes, escaping embedded quotes."""
    return '"' + name.replace('"', '""') + '"'


def _cell_to_str(value):
    """Convert a single cell value to a CSV-friendly representation.

    ``None`` becomes an empty string; ``bytes``/``bytearray`` become a
    ``0x``-prefixed hex string.  Everything else is returned unchanged and
    left to :class:`csv.writer` to serialise.
    """
    if value is None:
        return ""
    if isinstance(value, (bytes, bytearray)):
        return "0x" + bytes(value).hex()
    return value


def _sqlite_ln(x):
    """Natural logarithm, matching SQLite 3.35+'s built-in ``LN``.

    SQLite math functions (including ``LN``) were added in 3.35.0.  Older
    distributions (e.g. Ubuntu 20.04 ships 3.31.x) lack them, so we register
    a Python-side replacement to keep view definitions that use ``LN``
    parsable on every platform.
    """
    if x is None:
        return None
    try:
        val = float(x)
    except (TypeError, ValueError):
        return None
    if val <= 0:
        return None
    return math.log(val)


def _register_compat_functions(conn):
    """Register Python functions for SQL built-ins missing on old SQLite."""
    conn.create_function("LN", 1, _sqlite_ln)


def export_one(conn, name, obj_type, out_dir):
    """Export a single table or view to ``<out_dir>/<name>.csv``.

    Returns a metadata dict for the manifest.  Empty tables/views are skipped
    (no CSV file is written) but still appear in the manifest with
    ``row_count`` 0.  When the object cannot be queried (for example a view
    whose underlying table is missing) the error is recorded and no file is
    written.
    """
    # Column metadata.  PRAGMA table_info is the primary path, but it can
    # fail on old SQLite versions when a view definition references a
    # function the engine does not recognise (e.g. LN on SQLite < 3.35).
    # Fall back to "SELECT * FROM <name> LIMIT 0" and cursor.description.
    columns = []
    col_names = []
    error = None
    try:
        pragma_rows = conn.execute(
            "PRAGMA table_info(%s)" % _quote_ident(name)
        ).fetchall()
        columns = [{"name": r[1], "type": (r[2] or "")} for r in pragma_rows]
        col_names = [c["name"] for c in columns]
    except sqlite3.Error:
        pass  # handled by the SELECT below or the fallback

    csv_path = out_dir / (name + ".csv")
    row_count = 0
    wrote_file = False
    try:
        cur = conn.execute("SELECT * FROM %s" % _quote_ident(name))
        if not col_names:
            col_names = [d[0] for d in cur.description]
            columns = [{"name": n, "type": ""} for n in col_names]
        first = cur.fetchone()
        if first is None:
            cur.close()
        else:
            with open(csv_path, "w", newline="", encoding="utf-8") as fh:
                writer = csv.writer(fh)
                writer.writerow(col_names)
                writer.writerow([_cell_to_str(v) for v in first])
                row_count = 1
                for row in cur:
                    writer.writerow([_cell_to_str(v) for v in row])
                    row_count += 1
            wrote_file = True
    except sqlite3.Error as exc:
        error = str(exc)
        if not col_names:
            col_names = []
            columns = []

    meta = {
        "name": name,
        "type": obj_type,
        "row_count": row_count,
        "columns": columns,
    }
    if wrote_file:
        meta["file"] = name + ".csv"
    if error:
        meta["error"] = error
    return meta


def export_database(db_path, out_dir):
    """Export every table and view in *db_path* into *out_dir*.

    Returns the manifest dict that was also written to ``manifest.json``.
    """
    db_path = Path(db_path).resolve()
    out_dir = Path(out_dir).resolve()
    if not db_path.is_file():
        raise FileNotFoundError("Database not found: %s" % db_path)
    out_dir.mkdir(parents=True, exist_ok=True)

    conn = sqlite3.connect(str(db_path))
    _register_compat_functions(conn)
    try:
        objects = conn.execute(
            "SELECT type, name FROM sqlite_master "
            "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
            "ORDER BY type, name"
        ).fetchall()
        entries = []
        total_rows = 0
        for obj_type, name in objects:
            meta = export_one(conn, name, obj_type, out_dir)
            entries.append(meta)
            total_rows += meta["row_count"]
    finally:
        conn.close()

    manifest = {
        "database": str(db_path),
        "exported_at": datetime.now(timezone.utc).isoformat(),
        "total_objects": len(entries),
        "total_tables": sum(1 for e in entries if e["type"] == "table"),
        "total_views": sum(1 for e in entries if e["type"] == "view"),
        "total_rows": total_rows,
        "objects": entries,
    }
    with open(out_dir / "manifest.json", "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    return manifest


def _print_summary(manifest, out_dir):
    """Print a human-readable summary table to stdout."""
    print("Database   : %s" % manifest["database"])
    print("Output dir : %s" % out_dir)
    print("Exported at: %s" % manifest["exported_at"])
    print("Tables     : %d" % manifest["total_tables"])
    print("Views      : %d" % manifest["total_views"])
    print("Total rows : %d" % manifest["total_rows"])
    print()
    header = "%-42s %-6s %8s" % ("Name", "Type", "Rows")
    print(header)
    print("-" * len(header))
    for obj in manifest["objects"]:
        flag = " !" if "error" in obj else ""
        print("%-42s %-6s %8d%s" % (obj["name"], obj["type"], obj["row_count"], flag))
    errors = [e for e in manifest["objects"] if "error" in e]
    print()
    print(
        "CSV files  : %d  (manifest.json written)"
        % sum(1 for o in manifest["objects"] if "file" in o)
    )
    if errors:
        print("Errors     : %d object(s) could not be queried" % len(errors))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Export every SQLite table/view to CSV + manifest.json."
    )
    parser.add_argument(
        "--input-db", required=True, help="Path to the SQLite database file."
    )
    parser.add_argument(
        "--output-dir", required=True, help="Directory to write CSV files into."
    )
    args = parser.parse_args(argv)
    manifest = export_database(args.input_db, args.output_dir)
    _print_summary(manifest, Path(args.output_dir).resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
