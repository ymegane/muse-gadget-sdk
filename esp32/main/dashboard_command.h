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

// display.show_dashboard (CONFIG_HOMEHUB_DASHBOARD): the agent sends indoor
// climate and the weather, and the full UI shows them on its dashboard tile
// (components/muse/muse_dashboard.c).

#pragma once

#include "cJSON.h"

// Adds display.show_dashboard to link.register's commands.
void dashboard_command_describe(cJSON *commands);

// Handles display.show_dashboard. `params` may be NULL.
cJSON *dashboard_command(const cJSON *params);
