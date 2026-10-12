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

import logging as log
from typing import List


class FinalLogger:
    def __init__(self) -> None:
        self.general_info_msg: List[str] = []
        self.kernel_info_msg: List[str] = []

    def add_general_info(self, message: str) -> None:
        self.general_info_msg.append(message)

    def add_kernel_info(self, message: str) -> None:
        self.kernel_info_msg.append(message)

    def report(self, return_code: int = 0) -> None:
        if return_code == 0:
            log.info(
                "BiProf successfully finished kernel profiling, "
                "reports are ready for review"
            )
        else:
            log.error("An error occurred during kernel profiling, please check logs")
        log.info("=" * 20)
        for message_group, group_name in [
            (self.general_info_msg, "Report INFO"),
            (self.kernel_info_msg, "Functions INFO"),
        ]:
            if len(message_group) != 0:
                log.info(group_name)
                for msg in message_group:
                    log.info(msg)
                log.info("=" * 20)


final_logger = FinalLogger()
