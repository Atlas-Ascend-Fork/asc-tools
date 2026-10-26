#! /usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import contextlib
import importlib
import json
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from io import StringIO
from pathlib import Path
from unittest.mock import patch

THIS_FILE_NAME = __file__
FILE_PATH = os.path.dirname(os.path.realpath(THIS_FILE_NAME))
OPTYPE_COLLECTOR_PATH = os.path.join(
    FILE_PATH, "../../../../", "utils/optype_collector"
)
sys.path = [OPTYPE_COLLECTOR_PATH] + sys.path

optype_collector_main = importlib.import_module(
    "optype_collector.optype_collector_main"
)


class TestOpTypeCollector(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = Path(tempfile.mkdtemp(prefix="optype_collector_ut_"))
        self.ascend_home = self.tmp_dir / "Ascend" / "latest"

    def tearDown(self):
        shutil.rmtree(str(self.tmp_dir), ignore_errors=True)

    def _write_json(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def _builtin_config(self, soc, name="builtin.json"):
        return (
            self.ascend_home
            / "opp"
            / "built-in"
            / "op_impl"
            / "ai_core"
            / "tbe"
            / "config"
            / soc
            / name
        )

    def _builtin_split_config(self, package, soc, name="split.json"):
        return (
            self.ascend_home
            / "opp"
            / "built-in"
            / "op_impl"
            / "ai_core"
            / "tbe"
            / "config"
            / package
            / soc
            / name
        )

    def _vendor_config(self, vendor, soc, name="custom.json"):
        return (
            self.ascend_home
            / "opp"
            / "vendors"
            / vendor
            / "op_impl"
            / "ai_core"
            / "tbe"
            / "config"
            / soc
            / name
        )

    def _custom_opp_config(self, root, vendor, soc, name="custom.json"):
        return root / vendor / "op_impl" / "ai_core" / "tbe" / "config" / soc / name

    def _platform_config(self, arch, file_name, soc_version, short_soc_version):
        config_path = self.ascend_home / arch / "data" / "platform_config" / file_name
        config_path.parent.mkdir(parents=True, exist_ok=True)
        config_path.write_text(
            "[version]\nSoC_version={}\nShort_SoC_version={}\n".format(
                soc_version, short_soc_version
            ),
            encoding="utf-8",
        )
        return config_path

    def _run_main(self, argv, env):
        output = StringIO()
        with patch.dict(os.environ, env, clear=True):
            with contextlib.redirect_stdout(output):
                ret = optype_collector_main.main(argv)
        return ret, output.getvalue()

    def test_platform_config_maps_public_soc_to_short_lookup_and_preserves_input_in_output(
        self,
    ):
        self._platform_config(
            "aarch64-linux", "Ascend910B.ini", "Ascend910B", "Ascend910"
        )
        self._write_json(self._builtin_config("ascend910"), {"PublicMappedOp": {}})

        ret, output = self._run_main(
            ["Ascend910B", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("PublicMappedOp", output)
        self.assertIn("SoC                    : Ascend910B", output)
        self.assertNotIn("MatchedSoC", output)
        self.assertNotIn("Matched SoC dirs", output)

    def test_unrelated_non_utf8_platform_config_does_not_abort_builtin_scan(self):
        self._platform_config(
            "x86_64-linux", "Ascend910B.ini", "Ascend910B", "Ascend910"
        )
        invalid = self.ascend_home / "x86_64-linux/data/platform_config/unrelated.ini"
        invalid.write_bytes(b"[version]\n# invalid UTF-8: \xff\n")
        self._write_json(self._builtin_config("ascend910"), {"PublicMappedOp": {}})
        self._write_json(self._builtin_config("ascend910b"), {"InternalNameOp": {}})
        env = os.environ.copy()
        env.update(
            ASCEND_HOME_PATH=str(self.ascend_home),
            PYTHONPATH=OPTYPE_COLLECTOR_PATH,
            PYTHONIOENCODING="utf-8",
        )

        for soc, expected in (
            ("Ascend910B", "PublicMappedOp"),
            ("ascend910b", "InternalNameOp"),
        ):
            with self.subTest(soc=soc):
                result = subprocess.run(
                    [sys.executable, "-m", "optype_collector", soc, "--builtin"],
                    env=env,
                    capture_output=True,
                    encoding="utf-8",
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(expected, result.stdout)
                self.assertNotIn("Traceback", result.stderr)

    def test_platform_config_mapped_ascend950_keeps_legacy_910_95_compatibility(self):
        self._platform_config(
            "aarch64-linux", "Ascend950DT_95A2.ini", "Ascend950DT_95A2", "Ascend950"
        )
        self._write_json(self._builtin_config("ascend910_95"), {"LegacyAliasOp": {}})
        self._write_json(self._builtin_config("ascend950"), {"CurrentAliasOp": {}})

        ret, output = self._run_main(
            ["Ascend950DT_95A2", "--builtin"],
            {"ASCEND_HOME_PATH": str(self.ascend_home)},
        )

        self.assertEqual(ret, 0)
        self.assertIn("LegacyAliasOp", output)
        self.assertIn("CurrentAliasOp", output)
        self.assertIn("SoC                    : Ascend950DT_95A2", output)
        self.assertNotIn("SoC                    : ascend910_95", output)

    def test_lowercase_soc_uses_input_as_internal_lookup_name_before_platform_config(
        self,
    ):
        self._platform_config(
            "x86_64-linux", "Ascend910B.ini", "Ascend910B", "Ascend910"
        )
        self._write_json(self._builtin_config("ascend910"), {"FullNameOnly": {}})
        self._write_json(
            self._builtin_config("ascend910b"), {"LegacyLowercaseOnly": {}}
        )

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("LegacyLowercaseOnly", output)
        self.assertNotIn("FullNameOnly", output)
        self.assertIn("SoC                    : ascend910b", output)

    def test_public_soc_name_case_is_preserved_in_output(self):
        self._platform_config(
            "x86_64-linux", "Ascend910B.ini", "Ascend910B", "Ascend910"
        )
        self._write_json(self._builtin_config("ascend910"), {"FullNameOnly": {}})
        self._write_json(self._builtin_config("ascend910b"), {"CaseFallbackOnly": {}})

        ret, output = self._run_main(
            ["ascend910B", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("SoC                    : ascend910B", output)
        self.assertIn("FullNameOnly", output)
        self.assertNotIn("CaseFallbackOnly", output)
        self.assertNotIn("Available SoC", output)

    def test_unsupported_soc_error_does_not_list_available_soc_versions(self):
        self._platform_config(
            "x86_64-linux", "Ascend910B.ini", "Ascend910B", "Ascend910"
        )
        self._write_json(self._builtin_config("ascend910"), {"Add": {}})

        ret, output = self._run_main(
            ["Ascend310P3", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 2)
        self.assertIn("SoC version is not supported: Ascend310P3", output)
        self.assertNotIn("Available SoC", output)
        self.assertNotIn("Ascend910B", output)

    def test_expand_soc_aliases_only_expands_lowercase_ascend950_to_legacy_name(self):
        self.assertEqual(
            optype_collector_main.expand_soc_aliases("ascend910_95"), ["ascend910_95"]
        )
        self.assertEqual(
            optype_collector_main.expand_soc_aliases("ascend950"),
            ["ascend950", "ascend910_95"],
        )
        self.assertEqual(optype_collector_main.expand_soc_aliases("Ascend950"), [])
        self.assertEqual(optype_collector_main.expand_soc_aliases("Ascend910B"), [])

    def test_expand_soc_aliases_uses_public_soc_map_case_insensitively(self):
        name_map = optype_collector_main.SocNameMap(
            external_to_short={"Ascend910B": ["ascend910"]},
            external_lower_to_short={"ascend910b": ["ascend910"]},
        )

        self.assertEqual(
            optype_collector_main.expand_soc_aliases("Ascend910B", name_map),
            ["ascend910"],
        )
        self.assertEqual(
            optype_collector_main.expand_soc_aliases("ascend910B", name_map),
            ["ascend910"],
        )
        self.assertEqual(
            optype_collector_main.expand_soc_aliases("ascend910b", name_map),
            ["ascend910b"],
        )

    def test_missing_ascend_home_path_tells_user_to_source_cann_set_env(self):
        ret, output = self._run_main(["ascend910b", "--builtin"], {})

        self.assertEqual(ret, 2)
        self.assertIn("CANN environment variables are not configured", output)
        self.assertIn(
            "Please execute: source <CANN_install_path>/cann/set_env.sh", output
        )
        self.assertNotIn("ASCEND_HOME_PATH is not set", output)

    def test_invalid_ascend_home_path_returns_error_before_scan(self):
        self.ascend_home.mkdir(parents=True, exist_ok=True)

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 2)
        self.assertIn("CANN environment variables are not configured", output)
        self.assertIn(
            "Please execute: source <CANN_install_path>/cann/set_env.sh", output
        )
        self.assertNotIn("ASCEND_HOME_PATH is invalid", output)
        self.assertNotIn(str(self.ascend_home / "opp"), output)
        self.assertIn("[Errors]", output)

    def test_missing_builtin_soc_reports_unsupported_without_available_soc_dirs(self):
        self._write_json(self._builtin_config("ascend910b"), {"Add": {}})

        ret, output = self._run_main(
            ["ascend310p", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 2)
        self.assertIn("SoC version is not supported: ascend310p", output)
        self.assertNotIn("Available SoC", output)
        self.assertNotIn("ascend910b", output)

    def test_custom_list_reports_wrong_soc_without_available_soc_dirs(self):
        self._write_json(
            self._vendor_config("vendor_a", "ascend910b"), {"CustomOnly": {}}
        )

        ret, output = self._run_main(
            ["ascend310p", "--custom"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 2)
        self.assertIn("SoC version is not supported: ascend310p", output)
        self.assertNotIn("Available SoC", output)
        self.assertNotIn("ascend910b", output)
        self.assertIn("[Errors]", output)

    def test_custom_list_reports_wrong_soc_from_ascend_custom_opp_path(self):
        custom_root = self.tmp_dir / "direct_custom"
        self._write_json(
            custom_root
            / "op_impl"
            / "ai_core"
            / "tbe"
            / "config"
            / "ascend910b"
            / "direct.json",
            {"DirectOnly": {}},
        )
        (self.ascend_home / "opp").mkdir(parents=True, exist_ok=True)

        ret, output = self._run_main(
            ["qwe", "--custom"],
            {
                "ASCEND_HOME_PATH": str(self.ascend_home),
                "ASCEND_CUSTOM_OPP_PATH": str(custom_root),
            },
        )

        self.assertEqual(ret, 2)
        self.assertIn("SoC version is not supported: qwe", output)
        self.assertNotIn("Available SoC", output)
        self.assertNotIn("ascend910b", output)

    def test_wrapper_reports_wrong_custom_soc_from_ascend_custom_opp_path(self):
        custom_root = self.tmp_dir / "direct_custom"
        self._write_json(
            custom_root
            / "op_impl"
            / "ai_core"
            / "tbe"
            / "config"
            / "ascend910b"
            / "direct.json",
            {"DirectOnly": {}},
        )
        (self.ascend_home / "opp").mkdir(parents=True, exist_ok=True)
        wrapper = Path(OPTYPE_COLLECTOR_PATH) / "optype_collector.sh"
        env = {
            "ASCEND_HOME_PATH": str(self.ascend_home),
            "ASCEND_CUSTOM_OPP_PATH": str(custom_root),
            "PYTHONPATH": OPTYPE_COLLECTOR_PATH,
            "PATH": os.environ.get("PATH", ""),
        }

        proc = subprocess.run(
            ["bash", str(wrapper), "qwe", "--custom"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            check=False,
        )

        self.assertEqual(proc.returncode, 2)
        self.assertIn("SoC version is not supported: qwe", proc.stdout)
        self.assertNotIn("Available SoC", proc.stdout)
        self.assertNotIn("ascend910b", proc.stdout)

    def test_missing_soc_error_does_not_expose_tried_soc_dirs(self):
        self._write_json(self._builtin_config("ascend910b"), {"Add": {}})

        ret, output = self._run_main(
            ["ascend950", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 2)
        self.assertIn("SoC version is not supported: ascend950", output)
        self.assertNotIn("Available SoC", output)
        self.assertNotIn("Tried SoC", output)
        self.assertNotIn("Tried SoC dirs", output)

    def test_builtin_list_collects_whole_package_and_split_package(self):
        self._write_json(self._builtin_config("ascend910b"), {"Add": {}, "MatMul": {}})
        self._write_json(self._builtin_split_config("nn", "ascend910b"), {"Relu": {}})

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("[Scan Info]", output)
        self.assertIn("[OpType List]", output)
        self.assertIn("Add", output)
        self.assertIn("MatMul", output)
        self.assertIn("Relu", output)
        self.assertIn("ConfigFiles", output)

    def test_builtin_list_collects_explicit_optype_keys_and_container_names(self):
        self._write_json(
            self._builtin_config("ascend910b"),
            {
                "ops": [
                    {"name": "NameFromOps"},
                    {"opType": "TypeFromExplicitKey"},
                ],
                "opList": [
                    {"op_name": "NameFromSnakeCaseKey"},
                ],
            },
        )

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("NameFromOps", output)
        self.assertIn("TypeFromExplicitKey", output)
        self.assertIn("NameFromSnakeCaseKey", output)

    def test_list_groups_optypes_by_directory_when_multiple_sources_are_selected(self):
        self._write_json(self._builtin_config("ascend910b"), {"Add": {}})
        self._write_json(self._builtin_split_config("nn", "ascend910b"), {"Relu": {}})

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn(
            "[builtin] {}".format(self._builtin_config("ascend910b").parent), output
        )
        self.assertIn(
            "[builtin] {}".format(
                self._builtin_split_config("nn", "ascend910b").parent
            ),
            output,
        )
        self.assertIn("    Add", output)
        self.assertIn("    Relu", output)

    def test_all_list_marks_builtin_and_custom_directory_groups(self):
        self._write_json(self._builtin_config("ascend910b"), {"Add": {}})
        self._write_json(
            self._vendor_config("vendor_a", "ascend910b"), {"CustomOnly": {}}
        )

        ret, output = self._run_main(
            ["ascend910b", "--all"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn(
            "[builtin] {}".format(self._builtin_config("ascend910b").parent), output
        )
        self.assertIn(
            "[custom] {}".format(self._vendor_config("vendor_a", "ascend910b").parent),
            output,
        )
        self.assertIn("    Add", output)
        self.assertIn("    CustomOnly", output)

    def test_detects_custom_builtin_and_custom_custom_conflicts(self):
        custom_root = self.tmp_dir / "custom_opp"
        self._write_json(self._builtin_config("ascend910b"), {"Add": {}, "MatMul": {}})
        self._write_json(
            self._vendor_config("vendor_a", "ascend910b"), {"Add": {}, "CustomSame": {}}
        )
        self._write_json(
            self._custom_opp_config(custom_root, "vendor_b", "ascend910b"),
            {"CustomSame": {}, "Other": {}},
        )

        ret, output = self._run_main(
            ["--detect-conflicts", "ascend910b"],
            {
                "ASCEND_HOME_PATH": str(self.ascend_home),
                "ASCEND_CUSTOM_OPP_PATH": str(custom_root),
            },
        )

        self.assertEqual(ret, 1)
        self.assertIn("Custom package conflicts with built-in OpTypes", output)
        self.assertIn("Custom package conflicts with another custom package", output)
        self.assertIn("Add", output)
        self.assertIn("CustomSame", output)
        self.assertIn("vendor_a", output)
        self.assertIn("vendor_b", output)

    def test_ascend950_collects_current_and_legacy_dirs_but_external_soc_is_ascend950(
        self,
    ):
        self._write_json(self._builtin_config("ascend910_95"), {"AliasOpA": {}})
        self._write_json(self._builtin_config("ascend950"), {"AliasOpB": {}})

        ret, output = self._run_main(
            ["ascend950", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("AliasOpA", output)
        self.assertIn("AliasOpB", output)
        self.assertIn("SoC                    : ascend950", output)
        self.assertNotIn("SoC                    : ascend910_95", output)
        self.assertNotIn("MatchedSoC", output)
        self.assertNotIn("Matched SoC dirs", output)

    def test_ascend910_95_does_not_collect_ascend950_dir(self):
        self._write_json(self._builtin_config("ascend910_95"), {"LegacyOnly": {}})
        self._write_json(self._builtin_config("ascend950"), {"CurrentOnly": {}})

        ret, output = self._run_main(
            ["ascend910_95", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("LegacyOnly", output)
        self.assertNotIn("CurrentOnly", output)
        self.assertIn("SoC                    : ascend910_95", output)

    def test_alias_soc_same_custom_source_is_merged_without_custom_custom_conflict(
        self,
    ):
        self._write_json(self._builtin_config("ascend950"), {"BuiltinOnly": {}})
        self._write_json(
            self._vendor_config("vendor_a", "ascend910_95"), {"AliasSame": {}}
        )
        self._write_json(
            self._vendor_config("vendor_a", "ascend950"),
            {"AliasSame": {}, "AliasOther": {}},
        )

        ret, output = self._run_main(
            ["--detect-conflicts", "ascend950"],
            {"ASCEND_HOME_PATH": str(self.ascend_home)},
        )

        self.assertEqual(ret, 0)
        self.assertIn("Custom packages        : 1", output)
        self.assertIn("Custom vs Custom       : 0 group(s)", output)
        self.assertIn("No duplicate OpType conflicts found", output)

    def test_nested_parameter_names_are_not_collected_as_optypes(self):
        self._write_json(
            self._builtin_config("ascend910b"),
            {
                "GridSample": {
                    "input0": {"name": "x"},
                    "input1": {"name": "grid"},
                    "output0": {"name": "y"},
                    "attr": {"list": "align_corners"},
                }
            },
        )

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("GridSample", output)
        self.assertNotIn("\n  x\n", output)
        self.assertNotIn("\n  grid\n", output)
        self.assertNotIn("\n  y\n", output)

    def test_invalid_json_is_reported_after_scan_without_stopping_other_files(self):
        self._write_json(self._builtin_config("ascend910b", "valid.json"), {"Add": {}})
        invalid_json = self._builtin_config("ascend910b", "invalid.json")
        invalid_json.parent.mkdir(parents=True, exist_ok=True)
        invalid_json.write_text("{ invalid json", encoding="utf-8")

        ret, output = self._run_main(
            ["ascend910b", "--builtin"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("Add", output)
        self.assertIn("[Warnings]", output)
        self.assertIn("Failed to parse JSON", output)

    def test_missing_custom_opp_path_does_not_block_vendors_scan(self):
        self._write_json(
            self._vendor_config("vendor_a", "ascend910b"), {"CustomOnly": {}}
        )

        ret, output = self._run_main(
            ["ascend910b", "--custom"], {"ASCEND_HOME_PATH": str(self.ascend_home)}
        )

        self.assertEqual(ret, 0)
        self.assertIn("CustomOnly", output)
        self.assertIn("ASCEND_CUSTOM_OPP_PATH is not set", output)

    def test_custom_scan_skips_empty_path_segment_and_reports_missing_custom_root(self):
        direct_custom_root = self.tmp_dir / "direct_custom"
        missing_custom_root = self.tmp_dir / "missing_custom"
        self._write_json(
            direct_custom_root
            / "op_impl"
            / "ai_core"
            / "tbe"
            / "config"
            / "ascend910b"
            / "direct.json",
            {"DirectOnly": {}},
        )
        (self.ascend_home / "opp").mkdir(parents=True, exist_ok=True)
        custom_opp_path = os.pathsep.join(
            [str(direct_custom_root), "", str(missing_custom_root)]
        )

        ret, output = self._run_main(
            ["ascend910b", "--custom"],
            {
                "ASCEND_HOME_PATH": str(self.ascend_home),
                "ASCEND_CUSTOM_OPP_PATH": custom_opp_path,
            },
        )

        self.assertEqual(ret, 0)
        self.assertIn("DirectOnly", output)
        self.assertIn("ASCEND_CUSTOM_OPP_PATH Path does not exist", output)
        self.assertIn(str(missing_custom_root), output)

    def test_scan_config_dir_reports_missing_non_directory_empty_and_no_optypes(self):
        missing_source = optype_collector_main._scan_config_dir(
            optype_collector_main.BUILTIN,
            "ascend910b",
            "ascend910b",
            self.tmp_dir / "missing_dir",
        )
        self.assertEqual(missing_source.status, "ERROR")
        self.assertIn("Path does not exist", missing_source.errors[0])
        self.assertIn("ascend910b", missing_source.identity)

        file_path = self.tmp_dir / "not_a_dir.json"
        file_path.write_text("{}", encoding="utf-8")
        file_source = optype_collector_main._scan_config_dir(
            optype_collector_main.BUILTIN,
            "ascend910b",
            "ascend910b",
            file_path,
        )
        self.assertEqual(file_source.status, "ERROR")
        self.assertIn("Scan path is not a directory", file_source.errors[0])

        empty_dir = self.tmp_dir / "empty_config"
        empty_dir.mkdir(parents=True)
        empty_source = optype_collector_main._scan_config_dir(
            optype_collector_main.BUILTIN,
            "ascend910b",
            "ascend910b",
            empty_dir,
        )
        self.assertEqual(empty_source.status, "WARN")
        self.assertIn("No JSON config files found", empty_source.warnings[0])

        no_optype_dir = self.tmp_dir / "no_optype_config"
        self._write_json(no_optype_dir / "config.json", {"socVersion": "ascend910b"})
        no_optype_source = optype_collector_main._scan_config_dir(
            optype_collector_main.BUILTIN,
            "ascend910b",
            "ascend910b",
            no_optype_dir,
        )
        self.assertEqual(no_optype_source.status, "WARN")
        self.assertIn("No OpTypes parsed", no_optype_source.warnings[0])

    def test_duplicate_custom_opp_path_does_not_duplicate_config_file_count(self):
        custom_root = self.tmp_dir / "custom_opp"
        self._write_json(
            self._custom_opp_config(custom_root, "vendor_a", "ascend910b"),
            {"CustomOnly": {}},
        )
        (self.ascend_home / "opp").mkdir(parents=True, exist_ok=True)
        duplicate_custom_path = os.pathsep.join([str(custom_root), str(custom_root)])

        with patch.dict(
            os.environ,
            {
                "ASCEND_HOME_PATH": str(self.ascend_home),
                "ASCEND_CUSTOM_OPP_PATH": duplicate_custom_path,
            },
            clear=True,
        ):
            result = optype_collector_main.scan_optypes(
                "ascend910b", need_builtin=False, need_custom=True
            )

        self.assertEqual(len(result.custom_sources), 1)
        self.assertEqual(len(result.custom_sources[0].config_files), 1)
        self.assertEqual(result.custom_sources[0].optypes, {"CustomOnly"})

    def test_detect_conflicts_ignores_same_root_custom_sources(self):
        left = optype_collector_main.OpTypeSource(
            optype_collector_main.CUSTOM,
            "ascend910b",
            "ascend910b",
            self.tmp_dir / "same_root",
            vendor_name="vendor_a",
            optypes={"SameOp"},
        )
        right = optype_collector_main.OpTypeSource(
            optype_collector_main.CUSTOM,
            "ascend910b",
            "ascend910b",
            self.tmp_dir / "same_root",
            vendor_name="vendor_b",
            optypes={"SameOp"},
        )

        report = optype_collector_main.detect_conflicts([], [left, right])

        self.assertFalse(report.has_conflicts)
        self.assertEqual(report.custom_custom, [])

    def test_no_arguments_prints_help_and_returns_two(self):
        ret, output = self._run_main([], {})

        self.assertEqual(ret, 2)
        self.assertIn("usage: optype_collector", output)
        self.assertIn("--detect-conflicts", output)

    def test_detect_conflicts_returns_two_when_scan_has_errors(self):
        ret, output = self._run_main(["--detect-conflicts", "ascend910b"], {})

        self.assertEqual(ret, 2)
        self.assertIn("CANN environment variables are not configured", output)
        self.assertIn(
            "Please execute: source <CANN_install_path>/cann/set_env.sh", output
        )
        self.assertIn("[Conflict Summary]", output)

    def test_module_entrypoint_uses_absolute_import_like_msobjdump(self):
        main_py = Path(OPTYPE_COLLECTOR_PATH) / "optype_collector" / "__main__.py"
        content = main_py.read_text(encoding="utf-8")

        self.assertIn(
            "from optype_collector.optype_collector_main import main", content
        )
        self.assertNotIn("from .optype_collector_main import main", content)

    def test_module_entrypoint_exits_with_main_return_code(self):
        with patch("optype_collector.optype_collector_main.main", return_value=7):
            with self.assertRaises(SystemExit) as context:
                runpy.run_module("optype_collector.__main__", run_name="__main__")

        self.assertEqual(context.exception.code, 7)


if __name__ == "__main__":
    unittest.main()
