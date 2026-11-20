#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="simt-occupancy-test-") as directory:
    subprocess.run([sys.argv[1], "--child", "warp-csv", directory], check=True)
    trace = json.loads((Path(directory) / "SimtWarpOccupancy.json").read_text())
    events = trace["traceEvents"]
    intervals = [event for event in events if event.get("cat") == "SIMT Warp"]
    assert len(intervals) == 3
    interval = intervals[0]
    assert len({event["pid"] for event in intervals}) == 3
    assert all(event["ts"] == 0 for event in intervals)
    cores = {
        event["pid"]: event["args"]["name"]
        for event in events
        if event["name"] == "process_name"
    }
    lanes = {
        (event["pid"], event["tid"]): event["args"]["name"]
        for event in events
        if event["name"] == "thread_name"
    }
    for event, core, warp_id in zip(intervals, ("aiv5", "aiv6", "aic3"), (7, 1, 1)):
        assert cores[event["pid"]] == core + " (local clock)"
        assert lanes[event["pid"], event["tid"]] == f"warp {warp_id}"
        assert "blockId" not in event["args"]
        assert event["name"] == f"warp {warp_id}"
        assert event["args"]["function_name"] == 'kernel,"test"'
    assert interval["dur"] == 4
    assert interval["args"]["warp_id"] == 7
    assert interval["args"]["start_time_raw"] == 10
    assert interval["args"]["end_time_raw"] == 14
    assert trace["unalignedWarpCount"] == 3
    assert not (Path(directory) / "SimtWarp.csv").exists()


def collect(scenario):
    with tempfile.TemporaryDirectory(prefix="simt-occupancy-test-") as directory:
        subprocess.run([sys.argv[1], "--child", scenario, directory], check=True)
        return json.loads((Path(directory) / "SimtWarpOccupancy.json").read_text())


for scenario in ("warp-csv-aligned", "warp-csv-wrap"):
    aligned = collect(scenario)
    assert aligned.get("alignmentMode") == "same_replay_syscnt", aligned
    intervals = [
        event for event in aligned["traceEvents"] if event.get("cat") == "SIMT Warp"
    ]
    assert [event["ts"] for event in intervals] == [0, 3, 5, 0.002]
    assert [event["dur"] for event in intervals] == [0.002, 0.0025, 0.005, 0.004]
    assert intervals[1]["args"]["vf_start_syscnt"] == "9007199254743993"

rated = collect("warp-csv-rated-frequency")
assert rated["alignmentMode"] == "same_replay_syscnt", rated
rated_intervals = [
    item for item in rated["traceEvents"] if item.get("cat") == "SIMT Warp"
]
assert [item["args"]["clock_frequency_source"] for item in rated_intervals] == [
    "rated",
    "rated",
    "current",
    "rated",
]
assert [item["args"]["clock_frequency_mhz"] for item in rated_intervals] == [
    1500,
    1500,
    1500,
    1500,
]
assert rated_intervals[1]["ts"] == 3
assert abs(rated_intervals[3]["ts"] - 4 / 1500) < 1e-12


def events(report, category):
    return [e for e in report["traceEvents"] if e.get("cat") == category]


two = collect("warp-csv-activity-local")
assert two["alignedWarpCount"] == 4
warps = events(two, "SIMT Warp")
assert [e["ts"] for e in warps] == [0, 3, 5, 6]
assert [e["dur"] for e in warps] == [0.002, 0.0025, 0.005, 0.004]
assert warps[-1]["args"]["vf_start_syscnt"] == "9007199254746993"
windows = events(two, "SIMT VF")
assert [
    (e["name"], e["ts"], e["dur"]) for e in windows if e["pid"] == warps[0]["pid"]
] == [("block", 0, 1.5), ("block", 6, 2.3)]
hybrid = collect("warp-csv-hybrid-vf")
hybrid_warps = events(hybrid, "SIMT Warp")
assert hybrid["alignmentMode"] == "same_replay_syscnt"
assert [e["ts"] for e in hybrid_warps] == [6, 3, 5, 6.002]
assert [e["args"]["vf_start_syscnt"] for e in hybrid_warps[::3]] == [
    "9007199254746993",
    "9007199254746993",
]
assert [
    (e["ts"], e["dur"])
    for e in events(hybrid, "SIMT VF")
    if e["pid"] == hybrid_warps[0]["pid"]
] == [(6, 2.3)]
assert hybrid["syscntOrigin"] == "9007199254740993"

for scenario in (
    "repeated-warp",
    "missing-anchor",
    "multiple-vfs",
    "hybrid-unknown-vf",
):
    partial = collect(f"warp-csv-{scenario}")
    assert partial["alignmentMode"] == "partial_same_replay_syscnt"
    assert partial["unalignedWarpCount"] > 0
    if scenario == "hybrid-unknown-vf":
        assert not any(
            e["args"]["aicore_id"] == 2 and e["args"]["subcore_id"] == 1
            for e in events(partial, "SIMT VF")
        )
    for event in events(partial, "SIMT Warp"):
        if "vf_start_syscnt" not in event["args"]:
            assert all(v["pid"] != event["pid"] for v in events(partial, "SIMT VF"))

unavailable = collect("warp-csv-no-frequency")
assert unavailable["unalignedWarpCount"] == 4
assert not events(unavailable, "SIMT VF")
