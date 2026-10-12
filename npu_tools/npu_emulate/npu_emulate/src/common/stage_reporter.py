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

from dataclasses import dataclass
import os
from typing import Optional, List, Any, Type
import datetime

from npu_emulate.src.common.utils import Message, Timer
from npu_emulate.src.common.biprof_context import biprof_ctx


class StageReporter:
    @dataclass
    class StageEntry:
        name: str
        start_time: datetime.datetime
        elapsed: float
        hidden: bool = False

    stages_time: List["StageReporter.StageEntry"] = []
    min_time: Optional[datetime.datetime] = None

    def __init__(self, stage_name: str, hidden: bool = False) -> None:
        self._stage_name: str = stage_name
        self._hidden: bool = hidden
        self.timer: Timer = Timer()

    def __enter__(self) -> "StageReporter":
        self.start()
        return self

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_value: Optional[BaseException],
        traceback: Optional[Any],
    ) -> None:
        self.end()

    def start(self) -> None:
        time_now: datetime.datetime = Message.status_message(
            f"{self._stage_name} started"
        )
        if self.__class__.min_time is None:
            self.__class__.min_time = time_now
        self.timer.reset()

    def end(self) -> None:
        elapsed: float = self.timer.count()
        end_time = Message.status_message(
            f"{self._stage_name} ended. Elapsed time {elapsed:.2f} s"
        )
        time_start = end_time - datetime.timedelta(seconds=elapsed)
        self.__class__.stages_time.append(
            self.StageEntry(self._stage_name, time_start, elapsed, self._hidden)
        )

    @classmethod
    def separate(cls) -> None:
        cls.stages_time.append(
            cls.StageEntry(
                "Separator",
                datetime.datetime.now(tz=datetime.timezone.utc),
                -1.0,
                False,
            )
        )

    @classmethod
    def dump_perf_table(cls) -> None:
        log_file: str = os.path.join(biprof_ctx.output_dir, "perf_log.dump")

        if not cls.stages_time:
            return

        stages_no_sep = [s for s in cls.stages_time if s.elapsed >= 0]
        if not stages_no_sep:
            return

        total_entry = max(stages_no_sep, key=lambda x: x.elapsed)
        total_time: float = total_entry.elapsed

        with open(log_file, "w+") as fp:
            time_fmt: str = "{:80s}{:>15s}{:>10.2f}{:>9.2f}%\n"
            fp.write(
                "{:80s}{:>15s}{:^25s}\n".format("Stage name", "Start time", "Duration")
            )
            fp.write(("-" * 120) + "\n")

            acc_time: float = 0.0
            for entry in cls.stages_time:
                if entry.elapsed < 0:
                    fp.write(("-" * 120) + "\n")
                    continue
                if entry.hidden:
                    continue

                acc_time += entry.elapsed
                assert cls.min_time is not None

                formatted = time_fmt.format(
                    entry.name[:80],
                    f"{(entry.start_time - cls.min_time).total_seconds():.2f}",
                    entry.elapsed,
                    entry.elapsed / total_time * 100,
                )
                fp.write(formatted)

            other_time: float = total_time - acc_time
            if other_time > 0:
                fp.write(("-" * 120) + "\n")
                fp.write(
                    time_fmt.format(
                        "Other", " ", other_time, other_time / total_time * 100
                    )
                )

            fp.write(("=" * 120) + "\n")
            fp.write(time_fmt.format("Total execution time", "", total_time, 100))
