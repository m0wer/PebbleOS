/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/persist_backup_endpoint.h"

#include "services/persist_backup_endpoint/confirmation.h"
#include <string.h>

#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "pbl/os/mutex.h"
#include "pbl/services/comm_session/session.h"
#include "pbl/services/comm_session/session_send_buffer.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/persist.h"
#include "pbl/services/system_task.h"
#include "pbl/util/crc32.h"
#include <pbl/drivers/rtc.h>
#include "process_management/app_install_manager.h"
#include "system/status_codes.h"
#include <stdlib.h>

typedef struct PersistBackupExport {
  uint8_t cursor;
} PersistBackupExport;

typedef struct PersistBackupImport {
  uint32_t records;
} PersistBackupImport;

static uint8_t s_response[700];
static size_t s_response_length;
static size_t s_max_payload;
static PersistBackupConfirmationCallback s_confirmation_callback;
static void *s_confirmation_context;
static NewTimerCallback s_timer_callback;
static void *s_timer_context;
static uint32_t s_inventory_generation;
static bool s_installed;
static bool s_import_aborted;
static uint32_t s_import_next_key;
static uint32_t s_import_commit_count;
static uint32_t s_import_expected_records;
static uint32_t s_import_expected_bytes;
static uint32_t s_import_expected_crc;
static PersistBackupExport s_export;
static PersistBackupImport s_import;
static const uint8_t s_uuid_one[UUID_SIZE] = {1};
static const uint8_t s_uuid_two[UUID_SIZE] = {2};
static const uint8_t s_value_one[] = {0xaa};
static const uint8_t s_value_two[] = {0xbb};

void *kernel_malloc(size_t bytes) {
  return malloc(bytes);
}

void kernel_free(void *ptr) {
  free(ptr);
}

PebbleMutex *mutex_create(void) {
  return (PebbleMutex *)1;
}

void mutex_lock(PebbleMutex *mutex) {}

void mutex_unlock(PebbleMutex *mutex) {}

static uint16_t prv_u16(const uint8_t *data) {
  return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t prv_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static void prv_write_u32(uint8_t *data, uint32_t value) {
  data[0] = value >> 24;
  data[1] = value >> 16;
  data[2] = value >> 8;
  data[3] = value;
}

static uint32_t prv_record_crc(uint32_t crc, uint32_t key, const uint8_t *value,
                               size_t value_length) {
  uint8_t header[] = {key >> 24, key >> 16, key >> 8, key, 0, value_length};
  return crc32(crc32(crc, header, sizeof(header)), value, value_length);
}

static void prv_request(CommSession *session, uint8_t command, uint16_t request_id,
                        uint32_t transaction_id, const uint8_t *payload, size_t payload_length) {
  uint8_t request[300] = {command,
                          1,
                          request_id >> 8,
                          request_id,
                          transaction_id >> 24,
                          transaction_id >> 16,
                          transaction_id >> 8,
                          transaction_id};
  cl_assert(payload_length <= sizeof(request) - 8);
  memcpy(&request[8], payload, payload_length);
  persist_backup_endpoint_protocol_msg_callback(session, request, 8 + payload_length);
}

static uint32_t prv_transaction_id(void) {
  return prv_u32(&s_response[4]);
}

bool comm_session_is_system(CommSession *session) {
  return session == (CommSession *)1 || session == (CommSession *)2;
}

size_t comm_session_send_buffer_get_max_payload_length(const CommSession *session) {
  return s_max_payload;
}

bool comm_session_send_data(CommSession *session, uint16_t endpoint_id, const uint8_t *data,
                            size_t length, uint32_t timeout_ms) {
  cl_assert_equal_i(endpoint_id, 9001);
  cl_assert(length <= sizeof(s_response));
  memcpy(s_response, data, length);
  s_response_length = length;
  return true;
}

bool system_task_add_callback(SystemTaskEventCallback callback, void *data) {
  callback(data);
  return true;
}

TimerID new_timer_create(void) {
  return 1;
}

bool new_timer_start(TimerID timer, uint32_t timeout_ms, NewTimerCallback callback, void *data,
                     uint32_t flags) {
  s_timer_callback = callback;
  s_timer_context = data;
  return true;
}

bool new_timer_stop(TimerID timer) {
  s_timer_callback = NULL;
  s_timer_context = NULL;
  return true;
}

void persist_backup_confirmation_request(bool is_import, PersistBackupConfirmationCallback callback,
                                         void *context) {
  s_confirmation_callback = callback;
  s_confirmation_context = context;
}

void persist_backup_confirmation_cancel(void *context) {
  s_confirmation_callback = NULL;
  s_confirmation_context = NULL;
}

bool rng_rand(uint32_t *rand_out) {
  *rand_out = 0x12345678;
  return true;
}

RtcTicks rtc_get_ticks(void) {
  return 1;
}

time_t rtc_get_time(void) {
  return 2;
}

size_t persist_service_get_max_size(void) {
  return 4096;
}

uint32_t persist_backup_inventory_get_generation(void) {
  return s_inventory_generation;
}

status_t persist_backup_inventory_each(PersistBackupInventoryCallback callback, void *context) {
  if (!callback((const Uuid *)s_uuid_one, context)) {
    return S_SUCCESS;
  }
  callback((const Uuid *)s_uuid_two, context);
  return S_SUCCESS;
}

status_t persist_backup_export_open(const Uuid *uuid, PersistBackupExport **export_out) {
  if (memcmp(uuid, s_uuid_one, UUID_SIZE)) {
    return E_DOES_NOT_EXIST;
  }
  s_export.cursor = 0;
  *export_out = &s_export;
  return S_SUCCESS;
}

status_t persist_backup_export_page(PersistBackupExport *export, uint32_t max_records,
                                    PersistBackupRecordCallback callback, void *context,
                                    bool *done_out) {
  const uint8_t *values[] = {s_value_one, s_value_two};
  const uint32_t keys[] = {7, 8};
  uint32_t used = 0;
  while (export->cursor < 2 && used < max_records) {
    if (!callback(keys[export->cursor], values[export->cursor], 1, context)) {
      *done_out = false;
      return S_SUCCESS;
    }
    export->cursor++;
    used++;
  }
  *done_out = export->cursor == 2;
  return S_SUCCESS;
}

void persist_backup_export_close(PersistBackupExport *export) {}

status_t persist_backup_import_begin(const Uuid *uuid, uint32_t expected_record_count,
                                     uint32_t expected_value_bytes, uint32_t expected_crc,
                                     PersistBackupImport **import_out) {
  s_import.records = 0;
  s_import_expected_records = expected_record_count;
  s_import_expected_bytes = expected_value_bytes;
  s_import_expected_crc = expected_crc;
  *import_out = &s_import;
  return S_SUCCESS;
}

status_t persist_backup_import_put(PersistBackupImport *import, uint32_t key, const uint8_t *value,
                                   size_t value_len) {
  cl_assert_equal_i(key, 7);
  cl_assert_equal_i(value_len, 1);
  cl_assert_equal_i(value[0], 0xaa);
  s_import_next_key = key;
  import->records++;
  return S_SUCCESS;
}

status_t persist_backup_import_commit(PersistBackupImport *import) {
  cl_assert_equal_i(import->records, 1);
  s_import_commit_count++;
  return S_SUCCESS;
}

void persist_backup_import_abort(PersistBackupImport *import) {
  s_import_aborted = true;
}

AppInstallId app_install_get_id_for_uuid(const Uuid *uuid) {
  return s_installed ? 1 : INSTALL_ID_INVALID;
}

void test_persist_backup_endpoint__initialize(void) {
  s_response_length = 0;
  s_max_payload = 656;
  s_confirmation_callback = NULL;
  s_confirmation_context = NULL;
  s_timer_callback = NULL;
  s_timer_context = NULL;
  s_inventory_generation = 1;
  s_installed = false;
  s_import_aborted = false;
  s_import_next_key = 0;
  s_import_commit_count = 0;
  s_import_expected_records = 0;
  s_import_expected_bytes = 0;
  s_import_expected_crc = 0;
  persist_backup_endpoint_init();
  PebbleCommSessionEvent event = {.is_system = true, .is_open = false};
  persist_backup_endpoint_handle_comm_session_event(&event);
}

void test_persist_backup_endpoint__get_info_and_bad_version(void) {
  prv_request((CommSession *)1, 1, 4, 0, NULL, 0);
  cl_assert_equal_i(s_response[0], 0x81);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert_equal_i(prv_u32(&s_response[10]), 1);

  uint8_t request[] = {1, 2, 0, 4, 0, 0, 0, 0};
  persist_backup_endpoint_protocol_msg_callback((CommSession *)1, request, sizeof(request));
  cl_assert_equal_i(prv_u16(&s_response[8]), 3);
}

void test_persist_backup_endpoint__pending_approve_deny_and_timeout(void) {
  prv_request((CommSession *)1, 2, 5, 0, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 1);
  cl_assert(s_confirmation_callback);
  s_confirmation_callback(true, s_confirmation_context);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert(prv_transaction_id());
  NewTimerCallback stale_timer_callback = s_timer_callback;
  void *stale_timer_context = s_timer_context;

  prv_request((CommSession *)1, 4, 6, prv_transaction_id(), NULL, 0);
  prv_request((CommSession *)1, 2, 7, 0, NULL, 0);
  s_confirmation_callback(false, s_confirmation_context);
  cl_assert_equal_i(prv_u16(&s_response[8]), 2);

  prv_request((CommSession *)1, 2, 8, 0, NULL, 0);
  cl_assert(s_timer_callback);
  s_timer_callback(s_timer_context);
  cl_assert_equal_i(prv_u16(&s_response[8]), 13);

  prv_request((CommSession *)1, 2, 9, 0, NULL, 0);
  PersistBackupConfirmationCallback stale_callback = s_confirmation_callback;
  void *stale_context = s_confirmation_context;
  prv_request((CommSession *)1, 4, 10, 0, NULL, 0);
  prv_request((CommSession *)1, 2, 11, 0, NULL, 0);
  stale_callback(true, stale_context);
  cl_assert_equal_i(prv_u16(&s_response[8]), 1);
  s_confirmation_callback(true, s_confirmation_context);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  const uint32_t transaction_id = prv_transaction_id();
  stale_timer_callback(stale_timer_context);
  prv_request((CommSession *)1, 0x10, 12, transaction_id, (uint8_t[]){0, 0}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
}

void test_persist_backup_endpoint__export_auth_pagination_and_stale(void) {
  prv_request((CommSession *)1, 2, 9, 0, NULL, 0);
  s_confirmation_callback(true, s_confirmation_context);
  const uint32_t transaction_id = prv_transaction_id();
  prv_request((CommSession *)2, 0x10, 10, transaction_id, (uint8_t[]){0, 0}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 5);

  prv_request((CommSession *)1, 0x10, 11, transaction_id, (uint8_t[]){0, 0}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert_equal_i(s_response[17], 2);
  prv_request((CommSession *)1, 0x11, 12, transaction_id, s_uuid_one, UUID_SIZE);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  prv_request((CommSession *)1, 0x12, 13, transaction_id, (uint8_t[]){0, 1}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert_equal_i(s_response[10], 0);
  prv_request((CommSession *)1, 0x12, 14, transaction_id, (uint8_t[]){0, 1}, 2);
  cl_assert_equal_i(s_response[10], 1);
  prv_request((CommSession *)1, 0x13, 15, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert_equal_i(prv_u32(&s_response[10]), 2);
  cl_assert_equal_i(prv_u32(&s_response[14]), 2);
  uint32_t export_crc = prv_record_crc(0, 7, s_value_one, sizeof(s_value_one));
  export_crc = prv_record_crc(export_crc, 8, s_value_two, sizeof(s_value_two));
  cl_assert_equal_i(prv_u32(&s_response[18]), export_crc);

  s_inventory_generation++;
  prv_request((CommSession *)1, 0x14, 16, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 9);
  prv_request((CommSession *)1, 0x10, 17, transaction_id, (uint8_t[]){0, 0}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 5);
}

void test_persist_backup_endpoint__import_order_installed_cancel_and_disconnect(void) {
  uint8_t open_payload[12] = {};
  uint8_t begin_payload[28] = {};
  const uint32_t import_crc = prv_record_crc(0, 7, s_value_one, sizeof(s_value_one));
  prv_write_u32(open_payload, 1);
  prv_write_u32(&open_payload[4], 1);
  prv_write_u32(&open_payload[8], 1);
  memcpy(begin_payload, s_uuid_one, UUID_SIZE);
  prv_write_u32(&begin_payload[16], 1);
  prv_write_u32(&begin_payload[20], 1);
  prv_write_u32(&begin_payload[24], import_crc);
  prv_request((CommSession *)1, 3, 17, 0, open_payload, sizeof(open_payload));
  s_confirmation_callback(true, s_confirmation_context);
  uint32_t transaction_id = prv_transaction_id();
  s_installed = true;
  prv_request((CommSession *)1, 0x20, 18, transaction_id, begin_payload, sizeof(begin_payload));
  cl_assert_equal_i(prv_u16(&s_response[8]), 11);
  s_installed = false;
  prv_request((CommSession *)1, 0x20, 19, transaction_id, begin_payload, sizeof(begin_payload));
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert_equal_i(s_import_expected_records, 1);
  cl_assert_equal_i(s_import_expected_bytes, 1);
  cl_assert_equal_i(s_import_expected_crc, import_crc);
  prv_request((CommSession *)1, 0x21, 20, transaction_id,
              (uint8_t[]){0, 0, 0, 1, 0, 0, 0, 7, 0, 1, 0xaa}, 11);
  cl_assert_equal_i(prv_u16(&s_response[8]), 14);
  cl_assert(s_import_aborted);

  prv_request((CommSession *)1, 3, 21, 0, open_payload, sizeof(open_payload));
  s_confirmation_callback(true, s_confirmation_context);
  transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 4, 22, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);

  prv_request((CommSession *)1, 3, 23, 0, open_payload, sizeof(open_payload));
  s_confirmation_callback(true, s_confirmation_context);
  transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 0x20, 24, transaction_id, begin_payload, sizeof(begin_payload));
  prv_request((CommSession *)1, 0x21, 25, transaction_id,
              (uint8_t[]){0, 0, 0, 0, 0, 0, 0, 7, 0, 1, 0xaa}, 11);
  prv_request((CommSession *)1, 0x22, 26, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);
  cl_assert_equal_i(s_import_commit_count, 1);
  prv_request((CommSession *)1, 0x23, 27, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 0);

  prv_request((CommSession *)1, 3, 28, 0, open_payload, sizeof(open_payload));
  s_confirmation_callback(true, s_confirmation_context);
  transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 0x20, 29, transaction_id, begin_payload, sizeof(begin_payload));
  PebbleCommSessionEvent event = {.is_system = true, .is_open = false};
  persist_backup_endpoint_handle_comm_session_event(&event);
  cl_assert_equal_p(s_confirmation_callback, NULL);
  cl_assert(s_import_aborted);
}

void test_persist_backup_endpoint__authenticated_errors_and_active_timeout_abort(void) {
  uint8_t open_payload[12] = {};
  uint8_t begin_payload[28] = {};
  const uint32_t import_crc = prv_record_crc(0, 7, s_value_one, sizeof(s_value_one));
  prv_write_u32(open_payload, 1);
  prv_write_u32(&open_payload[4], 1);
  prv_write_u32(&open_payload[8], 1);
  memcpy(begin_payload, s_uuid_one, UUID_SIZE);
  prv_write_u32(&begin_payload[16], 1);
  prv_write_u32(&begin_payload[20], 1);
  prv_write_u32(&begin_payload[24], import_crc);
  prv_request((CommSession *)1, 3, 30, 0, open_payload, sizeof(open_payload));
  s_confirmation_callback(true, s_confirmation_context);
  uint32_t transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 0x20, 31, transaction_id, begin_payload, sizeof(begin_payload));

  uint8_t oversized_record[267] = {0, 0, 0, 0, 0, 0, 0, 7, 1, 1};
  prv_request((CommSession *)1, 0x21, 32, transaction_id, oversized_record,
              sizeof(oversized_record));
  cl_assert_equal_i(prv_u16(&s_response[8]), 8);
  cl_assert(s_import_aborted);

  prv_request((CommSession *)1, 2, 33, 0, NULL, 0);
  s_confirmation_callback(true, s_confirmation_context);
  transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 0x7f, 34, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 14);
  prv_request((CommSession *)1, 0x10, 35, transaction_id, (uint8_t[]){0, 0}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 5);

  prv_request((CommSession *)1, 2, 36, 0, NULL, 0);
  s_confirmation_callback(true, s_confirmation_context);
  transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 0x12, 37, transaction_id, NULL, 0);
  cl_assert_equal_i(prv_u16(&s_response[8]), 4);
  prv_request((CommSession *)1, 0x10, 38, transaction_id, (uint8_t[]){0, 0}, 2);
  cl_assert_equal_i(prv_u16(&s_response[8]), 5);

  prv_request((CommSession *)1, 3, 39, 0, open_payload, sizeof(open_payload));
  s_confirmation_callback(true, s_confirmation_context);
  transaction_id = prv_transaction_id();
  prv_request((CommSession *)1, 0x20, 40, transaction_id, begin_payload, sizeof(begin_payload));
  s_timer_callback(s_timer_context);
  cl_assert(s_import_aborted);
}
