/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "kernel/notification_double_flick.h"

#include "pbl/drivers/rtc.h"
#include "pbl/util/uuid.h"
#include "popups/notifications/notification_window.h"
#include "shell/prefs.h"

static const uint32_t DOUBLE_FLICK_DISMISS_WINDOW_MS = 1500;
static bool s_dismiss_pending;
static RtcTicks s_first_flick_ticks;
static Uuid s_notification_id = UUID_INVALID_INIT;

static void prv_clear_pending(void) {
  s_dismiss_pending = false;
  s_notification_id = UUID_INVALID;
}

void notification_double_flick_handle_shake(PebbleEvent *event) {
  if (!shell_prefs_get_double_flick_dismiss_notification_enabled()) {
    prv_clear_pending();
    return;
  }

  Uuid notification_id;
  if (!notification_window_get_current_modal_dismissible_notification_id(&notification_id)) {
    prv_clear_pending();
    return;
  }

  const RtcTicks now = rtc_get_ticks();
  const RtcTicks dismiss_window_ticks =
      (RtcTicks)DOUBLE_FLICK_DISMISS_WINDOW_MS * RTC_TICKS_HZ / 1000;
  if (s_dismiss_pending && uuid_equal(&s_notification_id, &notification_id) &&
      (now - s_first_flick_ticks) <= dismiss_window_ticks) {
    // Clear before invoking so another queued shake cannot repeat this dismissal.
    prv_clear_pending();
    if (notification_window_dismiss_current_modal_notification()) {
      event->task_mask |= 1 << PebbleTask_App;
    }
    return;
  }

  s_dismiss_pending = true;
  s_first_flick_ticks = now;
  s_notification_id = notification_id;
}

#ifdef UNITTEST
void notification_double_flick_test_reset(void) {
  prv_clear_pending();
}
#endif
