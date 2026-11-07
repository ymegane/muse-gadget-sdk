/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The M5Stack CoreS3's ambient light sensor: a Lite-On LTR-553ALS on the
// BSP's internal I2C bus, the one the AXP2101 and touch share. A background
// task reads it every 5 seconds, steps its gain to keep the reading in range,
// and keeps the latest illuminance; sensors.read answers with that.

#pragma once

#include "cJSON.h"

// Start the polling task. It waits for the board to bring up the BSP's I2C
// bus (it never creates the bus itself), then keeps retrying while the sensor
// doesn't answer.
void m5stack_cores3_ltr553_init(void);

// sensors.read: the latest illuminance in lux, with its age. With no recent
// reading, an error.
cJSON *m5stack_cores3_ltr553_command(void);
