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
from concurrent.futures import ProcessPoolExecutor, as_completed
from typing import Any, Dict, List

from npu_emulate.src.common.biprof_context import biprof_ctx
from npu_emulate.src.common.biprof_enum import (
    CoreType,
    CTSectionType,
    InstrType,
    SubQueueType,
)
from npu_emulate.src.common.logger import final_logger
from npu_emulate.src.common.stage_reporter import StageReporter
from npu_emulate.src.frontend.tracing.chrome_tracing_default import (
    ChromeTracingBuilder,
    DatabaseManager,
    INSTR_TO_QUEUE_TYPE,
    InstrTraceBuilder,
    TraceId,
)


PIPES_WITH_SUBQUEUES = ["PUSHQ", "CUBE", "FIXP", "MTE1", "MTE2", "MTE3"]


class SmartPipeTraceBuilder(InstrTraceBuilder):
    def smart_pipes_instr_lists(self, instrs: List[Any]) -> List[List[Any]]:
        instr_by_subqueue: List[List[Any]] = []

        for instr in instrs:
            subqueue = instr["SubQueueType"]
            while len(instr_by_subqueue) <= subqueue:
                instr_by_subqueue.append([])
            instr_by_subqueue[subqueue].append(instr)
        res: List[List[Any]] = []
        for i, subqueue_instrs in enumerate(instr_by_subqueue):
            if (
                i not in [t.value for t in SubQueueType]
                or SubQueueType(i) == SubQueueType.EXEC
            ):
                res.append(subqueue_instrs)
            else:
                res += self.set_inline_instrs_lists(subqueue_instrs)

        return res

    def get_instr_events(
        self, inline_instrs, pid: TraceId, pipe: str, track_name: str = None
    ):
        res = []
        subqueue_slot_counters: Dict[int, int] = defaultdict(int)
        for tid, thread_line in enumerate(inline_instrs):
            if len(thread_line) == 0:
                continue
            tid_name = f"{pipe}_SLOT{tid:0>2}"
            if pipe in PIPES_WITH_SUBQUEUES and thread_line:
                subqueue = thread_line[0]["SubQueueType"]
                tid_name = f"{pipe}_{SubQueueType(subqueue).name}"
                if SubQueueType(subqueue) != SubQueueType.EXEC:
                    tid_name += f"_SLOT{subqueue_slot_counters[subqueue]}"
                    subqueue_slot_counters[subqueue] += 1
            res.append(self.thread_name_event(pid, tid_name))
            for instr in thread_line:
                if instr["Name"].lower().startswith(
                    "bar"
                ) and not tid_name.lower().startswith("all"):
                    res.append(self.get_bar_instr_event(pid, tid_name, instr))
                else:
                    res.append(self.get_instruction_event(pid, tid_name, instr))

                if instr["Id"] in self.event_details_map:
                    details = self.event_details_map[instr["Id"]]
                    pipe_flow_event = self.get_pipe_flow_event(
                        pid, tid_name, instr["Start"], details
                    )
                    res.append(pipe_flow_event)

                if instr["Id"] in self.dispatch_events_map:
                    details = self.dispatch_events_map[instr["Id"]]
                    dispatch_flow_event = self.get_dispatch_flow_event(
                        pid, tid_name, instr["Start"], details
                    )
                    res.append(dispatch_flow_event)

        return res

    def get_pipe_trace(
        self, core_id: int, core_type_id: int, pipe_id: int
    ) -> List[Any]:
        res: List[Any] = []
        instrs = DatabaseManager.get_instrs_by_unit_pipe(core_id, core_type_id, pipe_id)
        instrs = self.filter_instructions(instrs)
        instrs = self.prehandle_instrs(instrs, core_id, core_type_id, pipe_id)
        if InstrType(pipe_id).name in PIPES_WITH_SUBQUEUES:
            instrs = self.smart_pipes_instr_lists(instrs)
        else:
            instrs = self.set_inline_instrs_lists(instrs)

        group = INSTR_TO_QUEUE_TYPE[InstrType(pipe_id)]
        core_type = CoreType(core_type_id)
        t_name = InstrType(pipe_id).name
        pid = self.get_next_process_id(core_id)
        track_name = self.make_instr_track_name(core_type, group, t_name)
        res.append(self.process_name_event(pid, track_name))
        res += self.get_instr_events(instrs, pid, t_name, track_name)

        pid = self.get_next_process_id(core_id)
        t_name = f"{InstrType(pipe_id)}_DISPATCH"
        dispatch_track_name = self.make_instr_track_name(
            core_type, group, t_name, CTSectionType.PRE
        )
        dispatch_events = self.get_dispatch_events(
            pid, dispatch_track_name, core_id, core_type_id, pipe_id
        )
        if len(dispatch_events) != 0:
            res.append(self.process_name_event(pid, dispatch_track_name))
            res += dispatch_events

        return self.events_asdict(res)


class SmartPipeTracingBuilder(ChromeTracingBuilder):
    trace_builder_cls = SmartPipeTraceBuilder

    @classmethod
    def generate(cls, output_dir) -> int:
        core_list = DatabaseManager.get_active_core_list()
        with StageReporter("Smart pipe tracing generation"):
            with ProcessPoolExecutor(biprof_ctx.jobs) as executor:
                futures = [
                    executor.submit(
                        cls.generate_core_tracing, core, output_dir, "smart_pipe"
                    )
                    for core in core_list
                ]
                for future in as_completed(futures):
                    result = future.result()
                    if result != 0:
                        return 1
        final_logger.add_general_info(f"Smart pipe tracing reports: {output_dir}")
        return 0
