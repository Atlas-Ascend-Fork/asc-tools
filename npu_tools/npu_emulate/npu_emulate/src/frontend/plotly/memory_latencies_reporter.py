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

from collections import defaultdict
import gzip
import plotly.graph_objects as go  # type: ignore
import numpy as np
import json
from npu_emulate.src.common.stage_reporter import StageReporter
from typing import List, Dict, Any, Optional, Set, Tuple
from npu_emulate.src.common.biprof_enum import BrifEvent, BrifEventType, InstrType
from npu_emulate.src.backend.database import database_handler
from npu_emulate.src.backend.database_manager import BackendDBManager
from npu_emulate.src.frontend.base.plotly_reporter_base import PlotlyReporterBase
import logging as log
import re


class IncorrectLatenciesData(Exception):
    pass


class MemoryLatenciesReporter(PlotlyReporterBase):
    # Structure of map:
    # (addr, instr_id, tag_id) -> [[BrifEvent]]
    addr_to_multi_cmd_map: Dict[Tuple[str, int, int], List[List[BrifEvent]]]
    # (instr_id, group_id) -> [BrifEvent]
    instr_id_grp_id_map: Dict[Tuple[int, int], List[BrifEvent]]
    instr_id_to_instr_pop_end_tick: Dict[int, List[int]]
    nd2nz_instrs: Set[int]
    nd2nz_records: List[Dict[str, Any]]
    mte2_id_pat: re.Pattern

    def __init__(self, output_dir):
        super().__init__(output_dir)
        self.mte2_id_pat = re.compile(".*id: (?P<id>[0-9]+)")

    def clear_maps(self) -> None:
        self.addr_to_multi_cmd_map = defaultdict(list)
        self.instr_id_grp_id_map = defaultdict(list)
        self.instr_id_to_instr_pop_end_tick = defaultdict(list)
        self.nd2nz_records = []
        self.nd2nz_instrs = set()

    def build_history_gram(self) -> Any:
        send_rd_cmd_to_push_biu_data_duration = []
        send_rd_cmd_to_recv_biu_data_duration = []
        recv_to_push_biu_data_duration = []
        for addr, instr_id, tag_id in self.addr_to_multi_cmd_map:
            for multi_round_same_addr_cmds in self.addr_to_multi_cmd_map[
                (addr, instr_id, tag_id)
            ]:
                send_rd_cmd_tick = 0
                recv_biu_data_tick = 0
                push_biu_data_tick = 0
                for cmd in multi_round_same_addr_cmds:
                    if cmd.event_type == BrifEventType.SEND_CMD_EVENT.value:
                        send_rd_cmd_tick = cmd.event_tick
                    if cmd.event_type == BrifEventType.PUSH_DATA_EVENT.value:
                        push_biu_data_tick = cmd.event_tick
                    if cmd.event_type == BrifEventType.RECV_DATA_EVENT.value:
                        if recv_biu_data_tick < cmd.event_tick:
                            recv_biu_data_tick = cmd.event_tick
                send_rd_cmd_to_push_biu_data_duration.append(
                    push_biu_data_tick - send_rd_cmd_tick
                )
                send_rd_cmd_to_recv_biu_data_duration.append(
                    recv_biu_data_tick - send_rd_cmd_tick
                )
                recv_to_push_biu_data_duration.append(
                    push_biu_data_tick - recv_biu_data_tick
                )
        return (
            send_rd_cmd_to_push_biu_data_duration,
            send_rd_cmd_to_recv_biu_data_duration,
            recv_to_push_biu_data_duration,
        )

    def convert_addr_to_multi_cmd_map_2_instr_id_grp_id_map(self) -> None:
        grp_id = 0
        for addr, instr_id, tag_id in self.addr_to_multi_cmd_map:
            for multi_cmds in self.addr_to_multi_cmd_map[(addr, instr_id, tag_id)]:
                for cmd in multi_cmds:
                    if cmd.event_type == BrifEventType.SEND_CMD_EVENT.value:
                        grp_id = cmd.group_id
                    self.instr_id_grp_id_map[(instr_id, grp_id)].append(cmd)

    def get_pid(self, event: Dict) -> Any:
        """Get the PID of the event, handling various edge cases"""
        pid = event.get("pid")
        # Handle None, empty string, etc.
        if pid is None or pid == "" or pid == 0:
            return "unknown"
        return pid

    def is_main_block(self, event: Dict) -> bool:
        """
        Determine if the event is a main_block
        Customize this function based on your data format
        """
        # Example implementation: assume main_block names start with "main_"
        return event.get("cat", "").startswith("main_") and event.get("ph") == "X"

    def is_sub_block(self, event: Dict) -> bool:
        """
        Determine if the event is a main_block
        Customize this function based on your data format
        """
        # Example implementation: assume main_block names start with "main_"
        return event.get("cat", "").startswith("sub_") and event.get("ph") == "X"

    def find_main_block_for_sub(
        self, sub_block: Dict, main_blocks_by_pid: Dict[Any, List[Dict]]
    ) -> Optional[Tuple[Any, Dict]]:
        """
        Find the main_block that the sub_block belongs to

        Args:
            sub_block: sub block event
            main_blocks_by_pid: dictionary of main_blocks grouped by PID

        Returns:
            (pid, main_block) tuple, or None if not found
        """
        sub_pid = self.get_pid(sub_block)
        sub_start = int(sub_block["ts"])
        sub_end = sub_start + int(sub_block["dur"])
        result = None

        # Only search in main_blocks with the same PID
        if sub_pid in main_blocks_by_pid:
            for main_block in main_blocks_by_pid[sub_pid]:
                main_start = int(main_block["ts"])
                main_end = main_start + int(main_block["dur"])

                # Check if sub_block is completely contained within main_block
                if main_start <= sub_start and sub_end <= main_end:
                    result = (sub_pid, main_block)
                    break

        return result

    @staticmethod
    def _should_skip_event(event: Dict) -> bool:
        return event.get("ph") != "X" or "ts" not in event or "dur" not in event

    def _is_main_or_incomplete(self, event: Dict) -> bool:
        return self.is_main_block(event) or self._should_skip_event(event)

    def assign_non_overlapping_tids_for_main_blocks(
        self,
        trace_events: List[Dict],
    ) -> List[Dict]:
        """
        Assign non-overlapping TIDs to main_blocks, and have sub_blocks follow
        their main_block's tid
        Supports multi-PID environment, different PIDs are processed
        independently

        Workflow:
        1. Group all main_blocks by PID
        2. Assign non-overlapping TIDs to main_blocks within each PID
        3. For each sub_block, find its main_block within the same PID and
        set the same TID

        Note: This implementation preserves original TID and
        PID information in args
        """
        # 1. Extract all main_blocks and group by PID
        main_blocks_by_pid: Dict[Any, List[Dict]] = defaultdict(list)
        main_blocks: List[Dict] = []

        for event in trace_events:
            if self.is_main_block(event):
                pid = self.get_pid(event)
                main_blocks_by_pid[pid].append(event)
                main_blocks.append(event)

        if not main_blocks:
            log.debug(
                "Warning: No main_block events found,cannot perform TID assignment"
            )
            return trace_events

        log.debug(
            f"Found {len(main_blocks)} main_block events across"
            f"{len(main_blocks_by_pid)} PIDs"
        )

        # 2. Assign non-overlapping TIDs to main_blocks within each PID
        total_tids = 0
        max_tids_per_pid = 0
        pid_tid_counts = {}

        for pid, blocks in main_blocks_by_pid.items():
            # Sort by start time
            blocks_sorted = sorted(blocks, key=lambda x: int(x["ts"]))

            # Track the end time of the last event on each TID
            tid_ends: List[int] = []

            for block in blocks_sorted:
                start = int(block["ts"])
                end = start + int(block["dur"])

                # Save original TID and PID for debugging
                original_tid = block.get("tid", "unknown")
                original_pid = block.get("pid", "unknown")

                block["args"] = block.get("args", {})
                block["args"]["original_tid"] = original_tid
                block["args"]["original_pid"] = original_pid

                # Try to place in an existing TID
                assigned = False
                for tid, last_end in enumerate(tid_ends):
                    if start >= last_end:
                        tid_ends[tid] = end
                        block["tid"] = tid
                        assigned = True
                        break

                # If no suitable TID found, create a new TID
                if not assigned:
                    tid_ends.append(end)
                    block["tid"] = len(tid_ends) - 1

            tid_count = len(tid_ends)
            pid_tid_counts[pid] = tid_count
            total_tids += tid_count
            max_tids_per_pid = max(max_tids_per_pid, tid_count)
            log.debug(
                f"PID {pid}: assigned {tid_count} TIDs, processed"
                f"{len(blocks)} main_blocks"
            )

        log.debug(
            f"main_block assignment complete, total {total_tids} TIDs used, "
            f"max {max_tids_per_pid} TIDs per PID"
        )

        # 3. Assign sub_blocks to the same TID as their main_block
        sub_blocks_processed = 0
        sub_blocks_unmatched = 0

        for event in trace_events:
            # Skip main_blocks and incomplete events
            if self._is_main_or_incomplete(event):
                continue

            # Find the main_block (only search within the same PID)
            result = self.find_main_block_for_sub(event, main_blocks_by_pid)
            if result:
                _, main_block = result

                # Save original TID and PID
                event["args"] = event.get("args", {})
                event["args"]["original_tid"] = event.get("tid", "unknown")
                event["args"]["original_pid"] = event.get("pid", "unknown")

                # Set to same TID as main_block, but preserve original PID
                event["tid"] = main_block["tid"]
                sub_blocks_processed += 1
            else:
                sub_blocks_unmatched += 1

        if sub_blocks_unmatched > 0:
            log.debug(
                f"Warning: {sub_blocks_unmatched} sub_blocks could not find "
                "matching main_block"
            )

        log.debug(
            f"Successfully processed {sub_blocks_processed} sub_blocks, "
            "their TIDs now follow their main_block"
        )
        return trace_events

    def assign_non_overlapping_tids_for_sub_blocks(
        self,
        trace_events: List[Dict],
    ) -> List[Dict]:
        """
        Assign non-overlapping TIDs to sub_block
        Supports multi-PID environment, different PIDs are processed
        independently

        Workflow:
        1. Group all main_block by PID
        2. Assign non-overlapping TID to main_block within each PID
        3. For each sub_block, find its main_block within the same PID
            and set the same TID

        Note:
            This implementation preserves original TID and PID information
            in args
        """
        # 1. Extract all sub_block and group by PID
        sub_blocks_by_pid: Dict[Any, List[Dict]] = defaultdict(list)
        sub_blocks: List[Dict] = []

        for event in trace_events:
            if self.is_sub_block(event):
                pid = self.get_pid(event)
                sub_blocks_by_pid[pid].append(event)
                sub_blocks.append(event)

        if not sub_blocks:
            log.debug(
                "Warning: No sub_block events found, cannot perform TID assignment"
            )
            return trace_events

        log.debug(
            f"Found {len(sub_blocks)} sub_block events across "
            f"{len(sub_blocks_by_pid)} PIDs"
        )

        # 2. Assign non-overlapping TIDs to sub_blocks within each PID
        total_tids = 0
        max_tids_per_pid = 0
        pid_tid_counts = {}

        for pid, blocks in sub_blocks_by_pid.items():
            # Sort by start time
            blocks_sorted = sorted(blocks, key=lambda x: int(x["ts"]))

            # Track the end time of the last event on each TID
            tid_ends: List[int] = []

            for block in blocks_sorted:
                start = int(block["ts"])
                end = start + int(block["dur"])

                # Save original TID and PID for debugging
                original_tid = block.get("tid", "unknown")
                original_pid = block.get("pid", "unknown")

                block["args"] = block.get("args", {})
                block["args"]["original_tid"] = original_tid
                block["args"]["original_pid"] = original_pid

                # Try to place in an existing TID
                assigned = False
                for tid, last_end in enumerate(tid_ends):
                    if start >= last_end:
                        tid_ends[tid] = end
                        block["tid"] = tid
                        assigned = True
                        break

                # If no suitable TID found, create a new TID
                if not assigned:
                    tid_ends.append(end)
                    block["tid"] = len(tid_ends) - 1

            tid_count = len(tid_ends)
            pid_tid_counts[pid] = tid_count
            total_tids += tid_count
            max_tids_per_pid = max(max_tids_per_pid, tid_count)
            log.debug(
                f"PID {pid}: assigned {tid_count} TIDs, "
                f"processed {len(blocks)} sub_blocks"
            )

        log.debug(
            f"sub_block assignment complete, total {total_tids} TIDs used, "
            f"max {max_tids_per_pid} TIDs per PID"
        )
        return trace_events

    def generate_tracing_with_subtasks(self, core_id: int) -> None:
        """
        Each time block contains multiple sub-intervals,
        pairing consecutive time points to form subtasks.
        """
        events: Dict = {"traceEvents": []}

        for instr_id in self.instr_id_to_instr_pop_end_tick:
            time_block = self.instr_id_to_instr_pop_end_tick[instr_id]
            # Create a total task for the time block
            block_start = time_block[0]
            block_end = time_block[1]

            # Total time block start
            events["traceEvents"].append(
                {
                    "name": str(instr_id),
                    "cat": "main_block",
                    "ph": "X",
                    "ts": block_start,
                    "dur": int(block_end) - int(block_start),
                    "pid": "main_block_core_" + str(core_id),
                    "tid": 0,
                    "args": {"total_points": len(time_block)},
                }
            )
        for addr, instr_id, tag_id in self.addr_to_multi_cmd_map:
            for multi_round_same_addr_cmds in self.addr_to_multi_cmd_map[
                (addr, instr_id, tag_id)
            ]:
                if instr_id not in self.instr_id_to_instr_pop_end_tick:
                    continue  # discard icache
                send_rd_cmd_tick = 0
                push_biu_data_tick = 0
                for cmd in multi_round_same_addr_cmds:
                    if cmd.event_type == BrifEventType.SEND_CMD_EVENT.value:
                        send_rd_cmd_tick = cmd.event_tick
                    if cmd.event_type == BrifEventType.PUSH_DATA_EVENT.value:
                        push_biu_data_tick = cmd.event_tick

                # Sub-task start
                events["traceEvents"].append(
                    {
                        "name": str(instr_id),
                        "cat": "sub_task",
                        "ph": "X",
                        "ts": send_rd_cmd_tick,
                        "dur": int(push_biu_data_tick) - int(send_rd_cmd_tick),
                        "pid": "sub_block_core_" + str(core_id),
                        "tid": 0,
                        "args": {},
                    }
                )

        events_no_overlap = {}
        events_no_overlap["traceEvents"] = (
            self.assign_non_overlapping_tids_for_main_blocks(events["traceEvents"])
        )
        events_no_overlap["traceEvents"] = (
            self.assign_non_overlapping_tids_for_sub_blocks(
                events_no_overlap["traceEvents"]
            )
        )
        file_path = f"{self.output_dir}/qi_core_{core_id}_nd2nz_cmd_breakdown.json"
        json_str = json.dumps(events_no_overlap, indent=2)
        if len(json_str) > 5242880:  # 5MB
            with gzip.open(file_path + ".gz", "wt") as f:
                f.write(json_str)
        else:
            with open(file_path, "w") as f:
                f.write(json_str)

    def nd2nz_keep_order_analysis(self, core_id: int) -> None:
        for key in self.instr_id_grp_id_map:
            if key[0] not in self.nd2nz_instrs:
                # skip entries not marked ND2NZ
                continue
            send_rd_cmd_first_tick = 0
            recv_biu_data_last_tick = 0
            push_biu_data_last_tick = 0
            for cmd in self.instr_id_grp_id_map[key]:
                if cmd.event_type == BrifEventType.SEND_CMD_EVENT.value:
                    if send_rd_cmd_first_tick == 0:
                        send_rd_cmd_first_tick = cmd.event_tick
                    elif send_rd_cmd_first_tick > cmd.event_tick:
                        send_rd_cmd_first_tick = cmd.event_tick
                if cmd.event_type == BrifEventType.RECV_DATA_EVENT.value:
                    if recv_biu_data_last_tick < cmd.event_tick:
                        recv_biu_data_last_tick = cmd.event_tick
                if cmd.event_type == BrifEventType.PUSH_DATA_EVENT.value:
                    if push_biu_data_last_tick < cmd.event_tick:
                        push_biu_data_last_tick = cmd.event_tick

            rec = {
                "core_id": core_id,
                "send_rd_cmd_first_tick": int(send_rd_cmd_first_tick),
                "recv_biu_data_last_tick": int(recv_biu_data_last_tick),
                "push_biu_data_last_tick": int(push_biu_data_last_tick),
                "send_to_recv": int(recv_biu_data_last_tick - send_rd_cmd_first_tick),
                "send_to_push": int(push_biu_data_last_tick - send_rd_cmd_first_tick),
                "recv_to_push": int(push_biu_data_last_tick - recv_biu_data_last_tick),
            }
            self.nd2nz_records.append(rec)

    def mte2_analysis(self, core_id: int) -> None:
        mte2_instrs = database_handler.execute(f"""
        SELECT ExecInstrParams AS Params, ExecInstrTickStart,
               ExecInstrTickEnd, ExecInstrName as Name
        FROM ExecutedInstructions
        WHERE ExecInstrCoreId={core_id}
              AND InstrTypeId={InstrType.MTE2.value}
              AND ExecInstrName!='MOV_SPR_XN'
        """).fetchall()

        for instr in mte2_instrs:
            params = instr["Params"]
            if params is None:
                continue
            mtch = self.mte2_id_pat.match(params)
            if mtch is None:
                continue
            self.instr_id_to_instr_pop_end_tick[int(mtch["id"])] = [
                instr["ExecInstrTickStart"],
                instr["ExecInstrTickEnd"],
            ]
            if "ND2NZ" in instr["Name"]:
                self.nd2nz_instrs.add(int(mtch["id"]))

    def draw_pic(self, data_list, desc_list, core_id: int) -> None:
        fig = go.Figure()

        for data, desc in zip(data_list, desc_list):
            counts, edges = np.histogram(data, bins=30)
            centers = (edges[:-1] + edges[1:]) / 2

            fig.add_trace(
                go.Bar(
                    x=centers.tolist(),
                    y=counts.tolist(),
                    name=desc,
                    opacity=0.75,
                    hovertemplate=(
                        "<b>Duration:</b> %{x:.0f} ticks<br>"
                        "<b>Count:</b> %{y}<extra></extra>"
                    ),
                )
            )

        fig.update_layout(
            title=dict(text="<b>Memory Latency Distributions</b>"),
            xaxis_title="Duration (ticks)",
            yaxis_title="Count",
            barmode="overlay",
            template="plotly",
            showlegend=True,
        )

        file_name = f"qi_core_{core_id}_memory_latencies.html"
        with open(f"{self.output_dir}/{file_name}", "w") as f:
            f.write(fig.to_html(include_plotlyjs=True))

    def draw_pic_nd2nz_curve(self, core_id: int) -> None:
        figs = []
        recs_sorted = sorted(
            self.nd2nz_records, key=lambda r: r["send_rd_cmd_first_tick"]
        )
        titles = [
            "Recv BIU data (last tick) -> Push BIU data (last tick)",
            "Send RD cmd (first tick) -> Recv BIU data (last tick)",
            "Send RD cmd (first tick) -> Push BIU data (last tick)",
        ]
        datapoints = [
            "recv_to_push",
            "send_to_recv",
            "send_to_push",
        ]

        for _, (title, datapoint) in enumerate(zip(titles, datapoints)):
            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=[r["send_rd_cmd_first_tick"] for r in recs_sorted],
                    y=[r[datapoint] for r in recs_sorted],
                    mode="lines+markers",
                    marker=dict(size=8),
                    hovertemplate=(
                        "<b>Tick:</b> %{x}<br><b>Duration:</b> %{y}<extra></extra>"
                    ),
                )
            )

            fig.update_layout(
                title=dict(text=f"<b>{title}</b>"),
                xaxis_title="Tick",
                yaxis_title="Duration",
                hovermode="x unified",
                template="plotly",
                height=700,
            )
            figs.append(fig)

        html_parts = []
        for _, fig in enumerate(figs):
            html_parts.append(fig.to_html(include_plotlyjs=True, full_html=False))

        combined_html = f"""<!DOCTYPE html>
<html>
<head>
    <title>ND2NZ Memory Latency Analysis</title>
    <style>
        .plot-container {{ margin-bottom: 50px; }}
    </style>
</head>
<body>
    <h1>ND2NZ Memory Latency Analysis</h1>
    <div class="plot-container">{html_parts[0]}</div>
    <div class="plot-container">{html_parts[1]}</div>
    <div class="plot-container">{html_parts[2]}</div>
</body>
</html>"""

        path_name = f"{self.output_dir}/qi_core_{core_id}_nd2nz_latencies.html"
        with open(path_name, "w") as f:
            f.write(combined_html)

    def get_tick_boundaries(self, core_id: int) -> Tuple[int, int]:
        vals = database_handler.execute(f"""
        SELECT MIN(ExecInstrTickStart) AS MinTick,
               MAX(ExecInstrTickEnd) AS MaxTick
               FROM ExecutedInstructions
               WHERE ExecInstrCoreId={core_id}
        """).fetchone()
        return (vals["MinTick"], vals["MaxTick"])

    def traverse_file(self, core_id: int, min_tick: int, max_tick: int) -> None:
        events = database_handler.execute(f"""
        SELECT * FROM BrifEvents
        WHERE CoreId={core_id} AND
              EventTick>={min_tick} AND
              EventTick<={max_tick}
        ORDER BY EventTick
        """).fetchall()

        for event_raw in events:
            event = BrifEvent(
                core_id,
                event_raw["EventType"],
                event_raw["EventTick"],
                event_raw["MteInstrId"],
                event_raw["PackageAddr"],
                event_raw["TagId"],
                event_raw["GroupId"],
            )
            key = (event.addr, event.mte_instr_id, event.tag_id)
            if event.event_type == BrifEventType.SEND_CMD_EVENT.value:
                self.addr_to_multi_cmd_map[key].append([event])
            if event.event_type == BrifEventType.RECV_DATA_EVENT.value:
                if key in self.addr_to_multi_cmd_map:
                    self.addr_to_multi_cmd_map[key][-1].append(event)
            if event.event_type == BrifEventType.PUSH_DATA_EVENT.value:
                if key not in self.addr_to_multi_cmd_map:
                    raise IncorrectLatenciesData
                self.addr_to_multi_cmd_map[key][-1].append(event)

    def _process(self) -> None:
        for core_id in BackendDBManager.get_active_cores():
            self.clear_maps()
            self.mte2_analysis(core_id)
            (min_tick, max_tick) = self.get_tick_boundaries(core_id)
            self.traverse_file(core_id, min_tick, max_tick)
            (
                send_rd_cmd_to_push_biu_data_duration,
                send_rd_cmd_to_recv_biu_data_duration,
                recv_to_push_biu_data_duration,
            ) = self.build_history_gram()

            if len(self.addr_to_multi_cmd_map) > 0:
                self.draw_pic(
                    [
                        recv_to_push_biu_data_duration,
                        send_rd_cmd_to_recv_biu_data_duration,
                        send_rd_cmd_to_push_biu_data_duration,
                    ],
                    [
                        "Recv-to-Push BIU data duration",
                        "Send RD cmd to Recv BIU data duration",
                        "Send RD cmd to Push BIU data duration",
                    ],
                    core_id,
                )

            if len(self.nd2nz_instrs) > 0:
                self.convert_addr_to_multi_cmd_map_2_instr_id_grp_id_map()
                self.nd2nz_keep_order_analysis(core_id)
                self.generate_tracing_with_subtasks(core_id)
                self.draw_pic_nd2nz_curve(core_id)

    def process(self) -> None:
        with StageReporter("[QI] Memory latencies report generation"):
            self._process()
