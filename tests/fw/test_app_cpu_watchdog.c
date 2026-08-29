/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "process_management/app_cpu_watchdog.h"

#include "kernel/event_loop.h"
#include "kernel/pebble_tasks.h"
#include "process_management/app_install_types.h"
#include "process_management/app_manager.h"
#include "pbl/services/regular_timer.h"

#include "stubs_logging.h"

static RegularTimerInfo *s_timer;
static PebbleTaskRuntimeSnapshot s_snapshot;
static bool s_snapshot_succeeds;
static PebbleProcessMd s_app_md;
static AppInstallId s_install_id;
static int s_close_count;

void regular_timer_add_minutes_callback(RegularTimerInfo *timer) {
  s_timer = timer;
}

void launcher_task_add_callback(CallbackEventCallback callback, void *data) {
  callback(data);
}

bool pebble_task_get_runtime_snapshot(PebbleTaskRuntimeSnapshot *snapshot) {
  if (!s_snapshot_succeeds) {
    return false;
  }
  *snapshot = s_snapshot;
  return true;
}

const PebbleProcessMd *app_manager_get_current_app_md(void) {
  return &s_app_md;
}

AppInstallId app_manager_get_current_app_id(void) {
  return s_install_id;
}

bool app_install_id_from_app_db(AppInstallId id) {
  return id > INSTALL_ID_INVALID;
}

void app_manager_close_current_app(bool gracefully) {
  cl_assert(!gracefully);
  s_close_count++;
}

static void prv_fire_sample(uint32_t app_cpu_centipct) {
  s_snapshot.total_run_time += 10000;
  s_snapshot.task_run_time[PebbleTask_App] += app_cpu_centipct;
  s_timer->cb(s_timer->cb_data);
}

void test_app_cpu_watchdog__initialize(void) {
  s_timer = NULL;
  s_snapshot = (PebbleTaskRuntimeSnapshot){0};
  s_snapshot_succeeds = true;
  s_app_md = (PebbleProcessMd){
      .process_type = ProcessTypeWatchface,
      .process_storage = ProcessStorageFlash,
  };
  s_install_id = 1;
  s_close_count = 0;
  app_cpu_watchdog_init();
}

void test_app_cpu_watchdog__cleanup(void) {}

void test_app_cpu_watchdog__requires_five_consecutive_high_samples(void) {
  prv_fire_sample(0);
  for (int i = 0; i < 4; i++) {
    prv_fire_sample(9000);
  }
  cl_assert_equal_i(s_close_count, 0);

  prv_fire_sample(9000);
  cl_assert_equal_i(s_close_count, 1);
}

void test_app_cpu_watchdog__low_sample_resets_breach_count(void) {
  prv_fire_sample(0);
  for (int i = 0; i < 4; i++) {
    prv_fire_sample(9000);
  }
  prv_fire_sample(7999);
  for (int i = 0; i < 4; i++) {
    prv_fire_sample(9000);
  }

  cl_assert_equal_i(s_close_count, 0);
  prv_fire_sample(9000);
  cl_assert_equal_i(s_close_count, 1);
}

void test_app_cpu_watchdog__app_switch_resets_breach_count(void) {
  prv_fire_sample(0);
  for (int i = 0; i < 4; i++) {
    prv_fire_sample(9000);
  }

  s_install_id = 2;
  for (int i = 0; i < 5; i++) {
    prv_fire_sample(9000);
  }
  cl_assert_equal_i(s_close_count, 0);

  prv_fire_sample(9000);
  cl_assert_equal_i(s_close_count, 1);
}

void test_app_cpu_watchdog__ignores_apps_and_builtin_watchfaces(void) {
  prv_fire_sample(0);

  s_app_md.process_type = ProcessTypeApp;
  for (int i = 0; i < 5; i++) {
    prv_fire_sample(10000);
  }

  s_app_md.process_type = ProcessTypeWatchface;
  s_app_md.process_storage = ProcessStorageBuiltin;
  for (int i = 0; i < 5; i++) {
    prv_fire_sample(10000);
  }

  cl_assert_equal_i(s_close_count, 0);
}
