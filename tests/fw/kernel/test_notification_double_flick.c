/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "kernel/notification_double_flick.h"

#include "popups/notifications/notification_window.h"
#include "shell/prefs.h"

#include "clar.h"

static RtcTicks s_now;
static bool s_dismiss_succeeds;

bool shell_prefs_get_double_flick_dismiss_notification_enabled(void) {
  return true;
}

bool notification_window_get_current_modal_dismissible_notification_id(Uuid *id_out) {
  *id_out = (Uuid)UUID_INVALID;
  return true;
}

bool notification_window_dismiss_current_modal_notification(void) {
  return s_dismiss_succeeds;
}

RtcTicks rtc_get_ticks(void) {
  return s_now;
}

static PebbleEvent prv_shake_event(void) {
  return (PebbleEvent){
      .type = PEBBLE_ACCEL_SHAKE_EVENT,
  };
}

void test_notification_double_flick__initialize(void) {
  s_now = 0;
  s_dismiss_succeeds = false;
  notification_double_flick_test_reset();
}

void test_notification_double_flick__first_shake_is_not_consumed(void) {
  PebbleEvent event = prv_shake_event();

  notification_double_flick_handle_shake(&event);

  cl_assert_equal_i(0, event.task_mask & (1 << PebbleTask_App));
}

void test_notification_double_flick__successful_second_shake_is_consumed(void) {
  PebbleEvent first = prv_shake_event();
  PebbleEvent second = prv_shake_event();
  s_dismiss_succeeds = true;

  notification_double_flick_handle_shake(&first);
  s_now++;
  notification_double_flick_handle_shake(&second);

  cl_assert_equal_i(0, first.task_mask & (1 << PebbleTask_App));
  cl_assert_equal_i(1 << PebbleTask_App, second.task_mask & (1 << PebbleTask_App));
}

void test_notification_double_flick__unsuccessful_second_shake_is_not_consumed(void) {
  PebbleEvent first = prv_shake_event();
  PebbleEvent second = prv_shake_event();

  notification_double_flick_handle_shake(&first);
  s_now++;
  notification_double_flick_handle_shake(&second);

  cl_assert_equal_i(0, first.task_mask & (1 << PebbleTask_App));
  cl_assert_equal_i(0, second.task_mask & (1 << PebbleTask_App));
}
