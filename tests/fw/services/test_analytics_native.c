/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include <stdint.h>
#include <string.h>

#include "fake_rtc.h"
#include "pbl/services/analytics/analytics.h"
#include "pbl/services/analytics/backend.h"
#include "pbl/services/data_logging/data_logging_service.h"
#include "pbl/util/attributes.h"
#include "pbl/util/build_id.h"

#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_prompt.h"

typedef struct PACKED {
  uint8_t version;
  uint64_t timestamp;
  uint8_t build_id[BUILD_ID_EXPECTED_LEN];
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) uint32_t metric_##key;
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) int32_t metric_##key;
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) \
  uint32_t metric_##key;                                        \
  uint16_t metric_##key##_scale;
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) \
  int32_t metric_##key;                                       \
  uint16_t metric_##key##_scale;
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) uint32_t metric_##key;
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) char metric_##key[(len) + 1];
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
} TestNativeHeartbeatRecord;

static const uint8_t s_build_id[BUILD_ID_EXPECTED_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10, 0x20, 0x30, 0x40,
};

const struct {
  uint32_t name_length;
  uint32_t data_length;
  uint32_t type;
  uint8_t data[BUILD_ID_NAME_EXPECTED_LEN + BUILD_ID_EXPECTED_LEN];
} TINTIN_BUILD_ID = {
    .name_length = BUILD_ID_NAME_EXPECTED_LEN,
    .data_length = BUILD_ID_EXPECTED_LEN,
    .type = 3,
    .data =
        {
            'G',  'N',  'U',  '\0', 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10, 0x20, 0x30, 0x40,
        },
};

extern const struct pbl_analytics_backend_ops pbl_analytics__native_ops;
extern void pbl_analytics__native_heartbeat(void);
extern void pbl_analytics__native_init(void);

static TestNativeHeartbeatRecord s_record;
static unsigned s_log_count;

DataLoggingSession *dls_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_size,
                               bool buffered, bool resume, const Uuid *uuid) {
  cl_assert_equal_i(tag, DlsSystemTagAnalyticsNativeHeartbeat);
  cl_assert_equal_i(item_type, DATA_LOGGING_BYTE_ARRAY);
  cl_assert_equal_i(item_size, sizeof(s_record));
  cl_assert(!buffered);
  cl_assert(!resume);
  cl_assert(uuid != NULL);
  return (DataLoggingSession *)1;
}

DataLoggingResult dls_log(DataLoggingSession *session, const void *data, uint32_t num_items) {
  cl_assert_equal_p(session, (DataLoggingSession *)1);
  cl_assert_equal_i(num_items, 1);
  memcpy(&s_record, data, sizeof(s_record));
  s_log_count++;
  return DATA_LOGGING_SUCCESS;
}

void test_analytics_native__initialize(void) {
  memset(&s_record, 0, sizeof(s_record));
  s_log_count = 0;
  fake_rtc_init(0, 1234567890);
  pbl_analytics__native_init();
}

void test_analytics_native__cleanup(void) {}

void test_analytics_native__heartbeat_serializes_metric_types(void) {
  pbl_analytics__native_ops.set_unsigned(PBL_ANALYTICS_KEY(memory_pct_max), 0xfedcba98);
  pbl_analytics__native_ops.set_signed(PBL_ANALYTICS_KEY(utc_offset_s), -12345);
  pbl_analytics__native_ops.set_unsigned(PBL_ANALYTICS_KEY(battery_soc_pct), 9876);
  pbl_analytics__native_ops.set_signed(PBL_ANALYTICS_KEY(battery_voltage_delta), -42000);
  pbl_analytics__native_ops.set_signed(PBL_ANALYTICS_KEY(battery_current_avg_ua), -123456);
  pbl_analytics__native_ops.set_signed(PBL_ANALYTICS_KEY(battery_current_peak_ua), 789012);
  pbl_analytics__native_ops.set_unsigned(PBL_ANALYTICS_KEY(battery_current_sample_count), 61);
  pbl_analytics__native_ops.set_string(PBL_ANALYTICS_KEY(fw_version), "v4.36.0-test");
  pbl_analytics__native_ops.set_string(PBL_ANALYTICS_KEY(watchface_name), "Test Face");
  pbl_analytics__native_ops.timer_start(PBL_ANALYTICS_KEY(battery_charge_time_ms));
  fake_rtc_increment_ticks(RTC_TICKS_HZ);
  pbl_analytics__native_ops.timer_stop(PBL_ANALYTICS_KEY(battery_charge_time_ms));

  pbl_analytics__native_heartbeat();

  cl_assert_equal_i(s_log_count, 1);
  cl_assert_equal_i(s_record.version, 5);
  cl_assert_equal_i(s_record.timestamp, 1234567890);
  cl_assert_equal_m(s_record.build_id, s_build_id, sizeof(s_build_id));
  cl_assert_equal_m(&s_record.metric_memory_pct_max, &(uint32_t){0xfedcba98}, sizeof(uint32_t));
  cl_assert_equal_i(s_record.metric_utc_offset_s, -12345);
  cl_assert_equal_i(s_record.metric_battery_soc_pct, 9876);
  cl_assert_equal_i(s_record.metric_battery_soc_pct_scale, 100);
  cl_assert_equal_i(s_record.metric_battery_voltage_delta, -42000);
  cl_assert_equal_i(s_record.metric_battery_voltage_delta_scale, 1000);
  cl_assert_equal_i(s_record.metric_battery_current_avg_ua, -123456);
  cl_assert_equal_i(s_record.metric_battery_current_peak_ua, 789012);
  cl_assert_equal_i(s_record.metric_battery_current_sample_count, 61);
  cl_assert_equal_i(s_record.metric_battery_charge_time_ms, 1000);
  cl_assert_equal_s(s_record.metric_fw_version, "v4.36.0-test");
  cl_assert_equal_s(s_record.metric_watchface_name, "Test Face");
}
