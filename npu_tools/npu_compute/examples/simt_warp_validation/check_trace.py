#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""Validate Chrome Trace structure without assuming SDK timestamp units."""

import json
from pathlib import Path
import sys


SIMT_OCCUPANCY_FILE_NAME = "SimtWarpOccupancy.json"
TRACE_EVENT_FIELDS = ("name", "pid", "tid", "ts", "dur", "args")
WARP_RECORD_FIELDS = (
    "aicore_id",
    "subcore_id",
    "warp_id",
    "start_time_raw",
    "end_time_raw",
    "duration_raw",
)


def require_fields(record, fields, error):
    for key in fields:
        if key not in record:
            raise ValueError(error)


def read_track_labels(events):
    tracks = {}
    for event in events:
        if event.get("ph") == "M" and event.get("name") == "thread_name":
            tracks[(event["pid"], event["tid"])] = event["args"]["name"]
    return tracks


def activity_track_name(event, data, category):
    if category == "SIMT VF":
        return "block" if data.get("windowSource") else "simt vf"
    return "block" if event["name"] == "simt vf" else "aiv activity"


def validate_activity(event, data, tracks, category):
    track = activity_track_name(event, data, category)
    if tracks.get((event["pid"], event["tid"])) != track:
        raise ValueError(f"invalid {track} track label")
    args = event["args"]
    if args["subcore_id"] not in (0, 1):
        raise ValueError(f"{track} recorded on a non-vector subcore")
    source = args.get("interval_source")
    if source not in (None, "vf_syscnt", "warp_range"):
        raise ValueError("unknown SIMT VF interval source")
    if source != "warp_range" and int(args["start_syscnt"]) > int(args["end_syscnt"]):
        raise ValueError(f"invalid {track} counter range")
    if category == "SIMT VF" and (source or data.get("simtVfSource")):
        int(args["vf_instr_address"])


def validate_warp(event, tracks):
    args = event["args"]
    require_fields(args, WARP_RECORD_FIELDS, "missing original Warp record field")
    if "blockId" in args:
        raise ValueError("unexpected Block ID")
    warp_id = args["warp_id"]
    if tracks.get((event["pid"], event["tid"])) not in (
        f"warp {warp_id}",
        f"Warp {warp_id}",
        f"warpid {warp_id}",
    ):
        raise ValueError("invalid Warp track label")


def validate_interval(event, data, tracks):
    require_fields(event, TRACE_EVENT_FIELDS, "missing trace event field")
    if event["dur"] < 0:
        raise ValueError("negative duration")
    category = event.get("cat")
    if category in ("SIMT VF", "AIV Activity"):
        validate_activity(event, data, tracks, category)
    else:
        validate_warp(event, tracks)


def validate_report(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    events = data["traceEvents"]
    intervals = [event for event in events if event.get("ph") == "X"]
    warp_count = sum(event.get("cat") == "SIMT Warp" for event in intervals)
    if not warp_count:
        raise ValueError("no Warp intervals")

    tracks = read_track_labels(events)
    for event in intervals:
        validate_interval(event, data, tracks)

    vf_count = 0
    if (
        data.get("windowSource")
        or data.get("simtVfSource")
        or data.get("simtVfTimeNote")
    ):
        vf_count = sum(event.get("cat") == "SIMT VF" for event in intervals)
    activity_count = len(intervals) - warp_count - vf_count
    print(
        f"PASS: {path}, Warp intervals={warp_count}, "
        f"SIMT VF intervals={vf_count}, AIV activity intervals={activity_count}"
    )


def main():
    if len(sys.argv) != 2:
        print("usage: check_trace.py <extracted-report-directory>", file=sys.stderr)
        return 2
    paths = list(Path(sys.argv[1]).rglob(SIMT_OCCUPANCY_FILE_NAME))
    if not paths:
        print(f"FAIL: {SIMT_OCCUPANCY_FILE_NAME} not found")
        return 1
    for path in paths:
        try:
            validate_report(path)
        except (OSError, ValueError, KeyError, TypeError) as error:
            print(f"FAIL: cannot validate {path}: {error}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
