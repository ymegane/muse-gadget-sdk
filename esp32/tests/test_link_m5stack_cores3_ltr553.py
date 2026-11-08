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


class LinkM5stackCores3Ltr553Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "main/m5stack_cores3_ltr553.c").read_text()
        start = source.index("// ---- Protocol (host-tested)")
        (out / "ltr553_protocol.inc").write_text(source[start:source.index("// ---- I2C driver", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-Wall", "-Wextra", "-Werror", "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON), "-I", str(out)]
        commands = [
            [*cc, "-std=c11", *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/link_m5stack_cores3_ltr553_harness.c"), str(out / "cjson.o"), "-lm", "-o", str(out / "ltr553")],
        ]
        # The driver harness compiles the whole driver; its IDF headers are
        # empty and the harness supplies the fakes.
        fakes = out / "fakes"
        for header in ("driver/i2c_master.h", "esp_log.h", "esp_timer.h",
                       "freertos/FreeRTOS.h", "freertos/task.h", "sdkconfig.h"):
            (fakes / header).parent.mkdir(parents=True, exist_ok=True)
            (fakes / header).write_text("#pragma once\n")
        commands.append([
            *cc, "-std=c11", *flags, "-Wno-unused-function", "-I", str(fakes), "-I", str(ROOT / "main"),
            str(ROOT / "tests/link_m5stack_cores3_ltr553_driver_harness.c"), str(out / "cjson.o"), "-lm",
            "-o", str(out / "ltr553_driver"),
        ])
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_bytes_to_sensors_read_result(self):
        result = subprocess.run([str(self.out / "ltr553")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_driver_waits_for_the_bus_steps_gain_and_reinits(self):
        result = subprocess.run([str(self.out / "ltr553_driver")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_M5STACK_CORES3_LTR553")
        self.assertIn('add_command(commands, "sensors.read"', noise[start:noise.index("#endif", start)])
        app = (ROOT / "main/app.c").read_text()
        start_init = app.index("#if CONFIG_HOMEHUB_M5STACK_CORES3_LTR553\n    // Waits")
        self.assertIn("m5stack_cores3_ltr553_init();", app[start_init:app.index("#endif", start_init)])
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_M5STACK_CORES3_LTR553"):]
        self.assertIn('"sensors.read"', block[:block.index("#endif")])

    def test_feature_is_on_for_the_cores3_only(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        start = kconfig.index("config HOMEHUB_M5STACK_CORES3_LTR553")
        block = kconfig[start:kconfig.index("\n\n", start)]
        self.assertIn("depends on MUSE_BOARD_M5STACK_CORES3", block)
        self.assertIn("default y", block)
        start = kconfig.index("config HOMEHUB_M5STACK_CORES3_AUTO_BRIGHTNESS")
        block = kconfig[start:kconfig.index("\n\n", start)]
        self.assertIn("depends on HOMEHUB_M5STACK_CORES3_LTR553", block)
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn('if(CONFIG_HOMEHUB_M5STACK_CORES3_LTR553)\n    list(APPEND GADGET_SRCS "m5stack_cores3_ltr553.c")', cmake)


if __name__ == "__main__":
    unittest.main()
