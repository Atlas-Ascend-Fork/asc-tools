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

import abc
from typing import List, Optional
import plotly.graph_objects as go  # type: ignore


class PlotlyReporterBase(abc.ABC):
    def __init__(
        self,
        output_dir: str,
    ) -> None:
        self.output_dir = output_dir

    @abc.abstractmethod
    def _process(self) -> None:
        pass

    @abc.abstractmethod
    def process(self) -> None:
        pass

    @staticmethod
    def save_multifigures_report(
        figures: List[go.Figure],
        report_path: str,
        title: Optional[str] = None,
    ) -> None:
        style = """
        <style>
            body {
                font-family: Arial, sans-serif;
                margin: 0;
                padding: 0;
            }
            .report-title {
                text-align: center;
                padding: 24px 0 8px 0;
                font-size: 28px;
                font-weight: bold;
                color: #222;
                letter-spacing: 0.5px;
            }
            table {
                border-collapse: collapse;
                width: 100%;
                font-family: Arial, sans-serif;
                margin-bottom: 20px;
            }
            th, td {
                border: 1px solid #dddddd;
                text-align: left;
                padding: 12px;
            }
            th {
                background-color: #f2f2f2;
                font-weight: bold;
            }
            tr:nth-child(even) {
                background-color: #f9f9f9;
            }
            tr:hover {
                background-color: #f5f5f5;
            }
            .plot-container {
                height: 100vh !important;
                width: 100vw !important;
            }
        </style>
        """

        title_tag = f"<title>{title}</title>" if title is not None else ""
        title_header = (
            f'<div class="report-title">{title}</div>' if title is not None else ""
        )

        with open(report_path, "w") as f:
            for i, fig in enumerate(figures):
                html = fig.to_html(
                    full_html=(i == 0),
                    include_plotlyjs=(i == 0),
                )

                if i == 0:
                    html = html.replace(
                        "<head>",
                        f"<head>{title_tag}{style}",
                    )
                    html = html.replace(
                        "<body>",
                        f"<body>{title_header}",
                    )

                f.write(html)
