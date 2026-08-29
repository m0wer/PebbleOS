/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "app_cpu_watchdog.h"

#include "app_install_types.h"
#include "app_manager.h"
#include "kernel/event_loop.h"
#include "kernel/pebble_tasks.h"
#include "pbl/services/regular_timer.h"

#include <pbl/logging/logging.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_CPU_WATCHDOG_THRESHOLD_CENTIPCT 8000
#define APP_CPU_WATCHDOG_BREACH_LIMIT 5

static PebbleTaskRuntimeSnapshot s_previous_snapshot;
static bool s_has_previous_snapshot;
static AppInstallId s_previous_install_id;
static AppInstallId s_tracked_install_id;
static uint8_t s_consecutive_breaches;

static void prv_reset_tracking(void) {
  s_tracked_install_id = INSTALL_ID_INVALID;
  s_consecutive_breaches = 0;
}

static bool prv_is_third_party_watchface(const PebbleProcessMd *md, AppInstallId install_id) {
  return md && md->process_type == ProcessTypeWatchface &&
         md->process_storage == ProcessStorageFlash && app_install_id_from_app_db(install_id);
}

static void prv_collect_and_evaluate(void *data) {
  PebbleTaskRuntimeSnapshot current;
  if (!pebble_task_get_runtime_snapshot(&current)) {
    return;
  }

  const AppInstallId install_id = app_manager_get_current_app_id();
  if (!s_has_previous_snapshot) {
    s_previous_snapshot = current;
    s_previous_install_id = install_id;
    s_has_previous_snapshot = true;
    return;
  }

  const uint32_t delta_total = current.total_run_time - s_previous_snapshot.total_run_time;
  const uint32_t delta_app =
      current.task_run_time[PebbleTask_App] - s_previous_snapshot.task_run_time[PebbleTask_App];
  s_previous_snapshot = current;

  const AppInstallId interval_install_id = s_previous_install_id;
  s_previous_install_id = install_id;

  if (!delta_total || interval_install_id != install_id) {
    prv_reset_tracking();
    return;
  }

  uint32_t app_cpu_centipct = (uint32_t)(((uint64_t)delta_app * 10000U) / delta_total);
  if (app_cpu_centipct > 10000) {
    app_cpu_centipct = 10000;
  }

  const PebbleProcessMd *md = app_manager_get_current_app_md();
  if (!prv_is_third_party_watchface(md, install_id)) {
    prv_reset_tracking();
    return;
  }

  if (s_tracked_install_id != install_id) {
    s_tracked_install_id = install_id;
    s_consecutive_breaches = 0;
  }

  if (app_cpu_centipct < APP_CPU_WATCHDOG_THRESHOLD_CENTIPCT) {
    s_consecutive_breaches = 0;
    return;
  }

  if (++s_consecutive_breaches < APP_CPU_WATCHDOG_BREACH_LIMIT) {
    return;
  }

  PBL_LOG_ERR("Terminating runaway watchface (id=%" PRId32 ", cpu_pct=%" PRIu32 ")", install_id,
              app_cpu_centipct / 100);
  prv_reset_tracking();
  app_manager_close_current_app(false);
}

static void prv_timer_callback(void *data) {
  launcher_task_add_callback(prv_collect_and_evaluate, NULL);
}

void app_cpu_watchdog_init(void) {
  static RegularTimerInfo s_timer = {
      .cb = prv_timer_callback,
  };

  s_previous_snapshot = (PebbleTaskRuntimeSnapshot){0};
  s_has_previous_snapshot = false;
  s_previous_install_id = INSTALL_ID_INVALID;
  prv_reset_tracking();

  regular_timer_add_minutes_callback(&s_timer);
}
