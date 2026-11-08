# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class LinkDashboardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        command = (ROOT / "main/dashboard_command.c").read_text()
        start = command.index("// ---- Parser (host-tested)")
        parser = command[start:command.index("// ---- Command", start)]
        ui = (ROOT / "components/muse/muse_dashboard.c").read_text()
        start = ui.index("muse_dashboard_condition_t muse_dashboard_condition(")
        names = ui[start:ui.index("\n}\n", start) + 3]
        (out / "dashboard_parser.inc").write_text(names + parser)
        (out / "lvgl.h").write_text("typedef struct lv_obj_t lv_obj_t;\n")
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-Wall", "-Wextra", "-Werror", "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON),
                 "-I", str(out), "-I", str(ROOT / "components/muse")]
        commands = [
            [*cc, "-std=c11", *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/link_dashboard_harness.c"),
             str(out / "cjson.o"), "-lm", "-o", str(out / "dashboard")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_parses_the_dashboard_payload(self):
        result = subprocess.run([str(self.out / "dashboard")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_DASHBOARD\n    dashboard_command_describe")
        self.assertIn("dashboard_command_describe(commands)", noise[start:noise.index("#endif", start)])
        self.assertIn('"display.show_dashboard"', (ROOT / "main/dashboard_command.c").read_text())
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_DASHBOARD"):]
        self.assertIn('"display.show_dashboard"', block[:block.index("#endif")])

    def test_dashboard_is_off_unless_a_board_turns_it_on(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        block = kconfig[kconfig.index("config HOMEHUB_DASHBOARD\n"):]
        self.assertIn("default n", block[:block.index("config HOMEHUB_DASHBOARD_IDLE_S")])
        self.assertNotIn("HOMEHUB_DASHBOARD", (ROOT / "devices/sdkconfig.muse").read_text())


if __name__ == "__main__":
    unittest.main()
