/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/events.h"

void notification_double_flick_handle_shake(PebbleEvent *event);

#ifdef UNITTEST
void notification_double_flick_test_reset(void);
#endif
