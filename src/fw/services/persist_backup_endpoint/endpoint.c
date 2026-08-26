/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/persist_backup_endpoint.h"

#include "confirmation.h"

#include <limits.h>
#include <string.h>

#include <pbl/drivers/rng.h>
#include <pbl/drivers/rtc.h>
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "pbl/os/mutex.h"
#include "pbl/services/comm_session/session.h"
#include "pbl/services/comm_session/session_send_buffer.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/persist.h"
#include "pbl/services/system_task.h"
#include "pbl/util/crc32.h"
#include "process_management/app_install_manager.h"
#include "system/status_codes.h"

#define PERSIST_BACKUP_ENDPOINT_ID 9001
#define PERSIST_BACKUP_VERSION 1
#define PERSIST_BACKUP_HEADER_SIZE 8
#define PERSIST_BACKUP_RESPONSE_HEADER_SIZE 10
#define PERSIST_BACKUP_PROMPT_TIMEOUT_MS 30000
#define PERSIST_BACKUP_IDLE_TIMEOUT_MS 60000

typedef enum {
  CommandGetInfo = 0x01,
  CommandOpenExport = 0x02,
  CommandOpenImport = 0x03,
  CommandCancel = 0x04,
  CommandListStores = 0x10,
  CommandOpenStore = 0x11,
  CommandReadPage = 0x12,
  CommandCloseStore = 0x13,
  CommandFinishExport = 0x14,
  CommandBeginStore = 0x20,
  CommandPutRecord = 0x21,
  CommandCommitStore = 0x22,
  CommandFinishImport = 0x23,
} PersistBackupCommand;

typedef enum {
  StatusOk,
  StatusPending,
  StatusDenied,
  StatusUnsupportedVersion,
  StatusMalformed,
  StatusUnauthorized,
  StatusBusy,
  StatusNotFound,
  StatusLimitExceeded,
  StatusStaleSnapshot,
  StatusChecksumMismatch,
  StatusTargetNotEmpty,
  StatusStorageFull,
  StatusExpired,
  StatusOutOfOrder,
  StatusInternal,
} PersistBackupStatus;

typedef enum {
  TransactionIdle,
  TransactionPendingExport,
  TransactionPendingImport,
  TransactionExport,
  TransactionImport,
} TransactionState;

typedef struct {
  TransactionState state;
  CommSession *session;
  uint16_t request_id;
  uint32_t transaction_id;
  uint32_t inventory_generation;
  PersistBackupExport *export;
  PersistBackupImport *import;
  bool store_open;
  bool store_done;
  uint32_t record_count;
  uint32_t value_bytes;
  uint32_t crc;
  uint32_t next_sequence;
  uint32_t expected_store_count;
  uint32_t expected_import_records;
  uint32_t expected_import_bytes;
  uint32_t imported_store_count;
  uint32_t imported_records;
  uint32_t imported_bytes;
  uint32_t authorization_request_id;
} PersistBackupState;

static PersistBackupState s_state;
static PebbleMutex *s_mutex;
static TimerID s_timeout_timer;
static uint32_t s_next_authorization_request_id;

static uint16_t prv_read_u16(const uint8_t *data) {
  return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t prv_read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static void prv_write_u16(uint8_t *data, uint16_t value) {
  data[0] = value >> 8;
  data[1] = value;
}

static void prv_write_u32(uint8_t *data, uint32_t value) {
  data[0] = value >> 24;
  data[1] = value >> 16;
  data[2] = value >> 8;
  data[3] = value;
}

static void prv_read_uuid(Uuid *uuid, const uint8_t *data) {
  memcpy(uuid, data, UUID_SIZE);
}

static void prv_lock(void) {
  mutex_lock(s_mutex);
}

static void prv_unlock(void) {
  mutex_unlock(s_mutex);
}

static void prv_send(CommSession *session, uint8_t command, uint16_t request_id,
                     uint32_t transaction_id, PersistBackupStatus status, const uint8_t *payload,
                     size_t payload_length) {
  const size_t max_payload = comm_session_send_buffer_get_max_payload_length(session);
  if (max_payload < PERSIST_BACKUP_RESPONSE_HEADER_SIZE ||
      payload_length > max_payload - PERSIST_BACKUP_RESPONSE_HEADER_SIZE) {
    return;
  }
  const size_t length = PERSIST_BACKUP_RESPONSE_HEADER_SIZE + payload_length;
  uint8_t *response = kernel_malloc(length);
  if (!response) {
    return;
  }
  response[0] = command | 0x80;
  response[1] = PERSIST_BACKUP_VERSION;
  prv_write_u16(&response[2], request_id);
  prv_write_u32(&response[4], transaction_id);
  prv_write_u16(&response[8], status);
  if (payload_length) {
    memcpy(&response[PERSIST_BACKUP_RESPONSE_HEADER_SIZE], payload, payload_length);
  }
  comm_session_send_data(session, PERSIST_BACKUP_ENDPOINT_ID, response, length,
                         COMM_SESSION_DEFAULT_TIMEOUT);
  kernel_free(response);
}

static void prv_release_state(PersistBackupState *state) {
  if (state->export) {
    persist_backup_export_close(state->export);
  }
  if (state->import) {
    persist_backup_import_abort(state->import);
  }
}

static PersistBackupState prv_take_state(void) {
  PersistBackupState old = s_state;
  s_state = (PersistBackupState){};
  new_timer_stop(s_timeout_timer);
  return old;
}

static void *prv_authorization_context(uint32_t request_id) {
  return (void *)(uintptr_t)request_id;
}

static void prv_schedule_timeout(uint32_t milliseconds);

static void prv_timeout_cleanup(void *context) {
  PersistBackupState old = {};
  bool pending = false;
  const uint32_t authorization_request_id = (uintptr_t)context;
  prv_lock();
  if (s_state.state != TransactionIdle &&
      s_state.authorization_request_id == authorization_request_id) {
    pending =
        s_state.state == TransactionPendingExport || s_state.state == TransactionPendingImport;
    old = prv_take_state();
  }
  prv_unlock();
  if (old.state == TransactionIdle) {
    return;
  }
  if (pending) {
    persist_backup_confirmation_cancel(prv_authorization_context(old.authorization_request_id));
    prv_send(old.session,
             old.state == TransactionPendingImport ? CommandOpenImport : CommandOpenExport,
             old.request_id, 0, StatusExpired, NULL, 0);
  }
  prv_release_state(&old);
}

static void prv_timeout_fired(void *context) {
  system_task_add_callback(prv_timeout_cleanup, context);
}

static void prv_schedule_timeout(uint32_t milliseconds) {
  new_timer_start(s_timeout_timer, milliseconds, prv_timeout_fired,
                  prv_authorization_context(s_state.authorization_request_id), 0);
}

static bool prv_is_authenticated(CommSession *session, uint32_t transaction_id) {
  return s_state.state != TransactionIdle && s_state.session == session &&
         s_state.transaction_id == transaction_id;
}

static uint32_t prv_new_transaction_id(void) {
  uint32_t transaction_id = 0;
  if (!rng_rand(&transaction_id) || transaction_id == 0) {
    transaction_id = (uint32_t)rtc_get_ticks() ^ (uint32_t)rtc_get_time();
    if (transaction_id == 0) {
      transaction_id = 1;
    }
  }
  return transaction_id;
}

static PersistBackupStatus prv_status_from_storage(status_t status) {
  if (status == E_DOES_NOT_EXIST) {
    return StatusNotFound;
  }
  if (status == E_BUSY) {
    return StatusBusy;
  }
  if (status == E_OUT_OF_STORAGE || status == E_RANGE) {
    return StatusStorageFull;
  }
  return StatusInternal;
}

static void prv_update_crc(uint32_t *crc, uint32_t key, const uint8_t *value, size_t value_length) {
  uint8_t record_header[6];
  prv_write_u32(record_header, key);
  prv_write_u16(&record_header[4], value_length);
  *crc = crc32(*crc, record_header, sizeof(record_header));
  *crc = crc32(*crc, value, value_length);
}

typedef struct {
  uint8_t *data;
  size_t remaining;
  uint16_t count;
  uint32_t crc;
  uint32_t value_bytes;
} ExportPage;

static bool prv_export_record(uint32_t key, const uint8_t *value, size_t value_length,
                              void *context) {
  ExportPage *page = context;
  const size_t length = 6 + value_length;
  if (length > page->remaining) {
    return false;
  }
  prv_write_u32(page->data, key);
  prv_write_u16(&page->data[4], value_length);
  memcpy(&page->data[6], value, value_length);
  prv_update_crc(&page->crc, key, value, value_length);
  page->data += length;
  page->remaining -= length;
  page->count++;
  page->value_bytes += value_length;
  return true;
}

typedef struct {
  uint16_t cursor;
  uint16_t count;
  uint16_t capacity;
  uint8_t *data;
  bool done;
} InventoryPage;

static bool prv_inventory_record(const Uuid *uuid, void *context) {
  InventoryPage *page = context;
  if (page->cursor) {
    page->cursor--;
    return true;
  }
  if (page->count == page->capacity) {
    page->done = false;
    return false;
  }
  memcpy(page->data, uuid, UUID_SIZE);
  page->data += UUID_SIZE;
  page->count++;
  return true;
}

static void prv_confirmation_complete(bool approved, void *context) {
  PersistBackupState old = {};
  PersistBackupState response_state = {};
  const uint32_t authorization_request_id = (uintptr_t)context;
  prv_lock();
  if ((s_state.state == TransactionPendingExport || s_state.state == TransactionPendingImport) &&
      s_state.authorization_request_id == authorization_request_id) {
    if (!approved) {
      old = prv_take_state();
    } else {
      s_state.transaction_id = prv_new_transaction_id();
      s_state.inventory_generation = persist_backup_inventory_get_generation();
      s_state.state =
          s_state.state == TransactionPendingImport ? TransactionImport : TransactionExport;
      response_state = s_state;
      prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
    }
  }
  prv_unlock();
  if (old.state != TransactionIdle) {
    prv_send(old.session,
             old.state == TransactionPendingImport ? CommandOpenImport : CommandOpenExport,
             old.request_id, 0, StatusDenied, NULL, 0);
    prv_release_state(&old);
  } else if (response_state.state != TransactionIdle) {
    prv_send(response_state.session,
             response_state.state == TransactionImport ? CommandOpenImport : CommandOpenExport,
             response_state.request_id, response_state.transaction_id, StatusOk, NULL, 0);
  }
}

// Requires s_mutex to be held. This function releases it before sending or invoking UI code.
static void prv_abort_authenticated_locked(PersistBackupStatus status, uint8_t command,
                                           uint16_t request_id) {
  PersistBackupState old = prv_take_state();
  prv_unlock();
  prv_send(old.session, command, request_id, old.transaction_id, status, NULL, 0);
  persist_backup_confirmation_cancel(prv_authorization_context(old.authorization_request_id));
  prv_release_state(&old);
}

static bool prv_abort_malformed_if_authenticated(CommSession *session, uint32_t transaction_id,
                                                 uint8_t command, uint16_t request_id) {
  prv_lock();
  if (prv_is_authenticated(session, transaction_id)) {
    prv_abort_authenticated_locked(StatusMalformed, command, request_id);
    return true;
  }
  prv_unlock();
  return false;
}

static void prv_handle_get_info(CommSession *session, uint16_t request_id, uint32_t transaction_id,
                                const uint8_t *payload, size_t payload_length) {
  if (transaction_id != 0 || payload_length != 0) {
    if (prv_abort_malformed_if_authenticated(session, transaction_id, CommandGetInfo, request_id)) {
      return;
    }
    prv_send(session, CommandGetInfo, request_id, transaction_id, StatusMalformed, NULL, 0);
    return;
  }
  uint8_t response[12];
  prv_write_u32(response, 1);
  prv_write_u16(&response[4], PERSIST_BACKUP_VALUE_MAX_LENGTH);
  prv_write_u32(&response[6], persist_service_get_max_size());
  const size_t max_payload = comm_session_send_buffer_get_max_payload_length(session);
  prv_write_u16(&response[10], max_payload > UINT16_MAX ? UINT16_MAX : max_payload);
  prv_send(session, CommandGetInfo, request_id, 0, StatusOk, response, sizeof(response));
}

static void prv_handle_open(CommSession *session, uint8_t command, uint16_t request_id,
                            uint32_t transaction_id, const uint8_t *payload,
                            size_t payload_length) {
  const bool is_import = command == CommandOpenImport;
  if (transaction_id != 0 || payload_length != (is_import ? 12 : 0)) {
    if (prv_abort_malformed_if_authenticated(session, transaction_id, command, request_id)) {
      return;
    }
    prv_send(session, command, request_id, transaction_id, StatusMalformed, NULL, 0);
    return;
  }
  prv_lock();
  if (s_state.state != TransactionIdle) {
    prv_unlock();
    prv_send(session, command, request_id, 0, StatusBusy, NULL, 0);
    return;
  }
  s_state = (PersistBackupState){
      .state = is_import ? TransactionPendingImport : TransactionPendingExport,
      .session = session,
      .request_id = request_id,
      .expected_store_count = is_import ? prv_read_u32(payload) : 0,
      .expected_import_records = is_import ? prv_read_u32(&payload[4]) : 0,
      .expected_import_bytes = is_import ? prv_read_u32(&payload[8]) : 0,
      .authorization_request_id = ++s_next_authorization_request_id,
  };
  if (s_state.authorization_request_id == 0) {
    s_state.authorization_request_id = ++s_next_authorization_request_id;
  }
  const uint32_t authorization_request_id = s_state.authorization_request_id;
  prv_schedule_timeout(PERSIST_BACKUP_PROMPT_TIMEOUT_MS);
  prv_unlock();
  prv_send(session, command, request_id, 0, StatusPending, NULL, 0);
  persist_backup_confirmation_request(is_import, prv_confirmation_complete,
                                      prv_authorization_context(authorization_request_id));
}

static void prv_handle_list(CommSession *session, uint16_t request_id, uint32_t transaction_id,
                            const uint8_t *payload, size_t payload_length) {
  if (payload_length != 2) {
    prv_lock();
    if (prv_is_authenticated(session, transaction_id)) {
      prv_abort_authenticated_locked(StatusMalformed, CommandListStores, request_id);
      return;
    }
    prv_unlock();
    prv_send(session, CommandListStores, request_id, transaction_id, StatusMalformed, NULL, 0);
    return;
  }
  const size_t max_payload = comm_session_send_buffer_get_max_payload_length(session);
  if (max_payload < PERSIST_BACKUP_RESPONSE_HEADER_SIZE + 8 + UUID_SIZE) {
    prv_send(session, CommandListStores, request_id, transaction_id, StatusLimitExceeded, NULL, 0);
    return;
  }
  uint8_t *response = kernel_malloc(max_payload);
  if (!response) {
    prv_send(session, CommandListStores, request_id, transaction_id, StatusInternal, NULL, 0);
    return;
  }
  PersistBackupStatus status = StatusOk;
  size_t response_length = 0;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    status = StatusUnauthorized;
  } else if (s_state.state != TransactionExport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandListStores, request_id);
    kernel_free(response);
    return;
  } else if (s_state.inventory_generation != persist_backup_inventory_get_generation()) {
    status = StatusStaleSnapshot;
  } else {
    InventoryPage page = {
        .cursor = prv_read_u16(payload),
        .capacity = (max_payload - PERSIST_BACKUP_RESPONSE_HEADER_SIZE - 8) / UUID_SIZE,
        .data = &response[8],
        .done = true,
    };
    if (page.capacity > UINT8_MAX) {
      page.capacity = UINT8_MAX;
    }
    persist_backup_inventory_each(prv_inventory_record, &page);
    prv_write_u32(response, s_state.inventory_generation);
    prv_write_u16(&response[4], prv_read_u16(payload) + page.count);
    response[6] = page.done;
    response[7] = page.count;
    response_length = 8 + page.count * UUID_SIZE;
    prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
  }
  prv_unlock();
  prv_send(session, CommandListStores, request_id, transaction_id, status, response,
           response_length);
  kernel_free(response);
}

static void prv_handle_open_store(CommSession *session, uint16_t request_id,
                                  uint32_t transaction_id, const uint8_t *payload,
                                  size_t payload_length) {
  PersistBackupStatus result = StatusOk;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionExport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandOpenStore, request_id);
    return;
  } else if (payload_length != UUID_SIZE) {
    prv_abort_authenticated_locked(StatusMalformed, CommandOpenStore, request_id);
    return;
  } else if (s_state.store_open) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandOpenStore, request_id);
    return;
  } else if (s_state.inventory_generation != persist_backup_inventory_get_generation()) {
    result = StatusStaleSnapshot;
  } else {
    Uuid uuid;
    prv_read_uuid(&uuid, payload);
    status_t status = persist_backup_export_open(&uuid, &s_state.export);
    if (FAILED(status)) {
      result = prv_status_from_storage(status);
    } else {
      s_state.store_open = true;
      s_state.store_done = false;
      s_state.record_count = 0;
      s_state.value_bytes = 0;
      s_state.crc = CRC32_INIT;
      prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
    }
  }
  prv_unlock();
  prv_send(session, CommandOpenStore, request_id, transaction_id, result, NULL, 0);
}

static void prv_handle_read_page(CommSession *session, uint16_t request_id, uint32_t transaction_id,
                                 const uint8_t *payload, size_t payload_length) {
  if (payload_length != 2 || prv_read_u16(payload) == 0) {
    prv_lock();
    if (prv_is_authenticated(session, transaction_id)) {
      prv_abort_authenticated_locked(StatusMalformed, CommandReadPage, request_id);
      return;
    }
    prv_unlock();
    prv_send(session, CommandReadPage, request_id, transaction_id, StatusMalformed, NULL, 0);
    return;
  }
  const size_t max_payload = comm_session_send_buffer_get_max_payload_length(session);
  if (max_payload < PERSIST_BACKUP_RESPONSE_HEADER_SIZE + 3 + 6 + PERSIST_BACKUP_VALUE_MAX_LENGTH) {
    prv_send(session, CommandReadPage, request_id, transaction_id, StatusLimitExceeded, NULL, 0);
    return;
  }
  uint8_t *response = kernel_malloc(max_payload);
  if (!response) {
    prv_send(session, CommandReadPage, request_id, transaction_id, StatusInternal, NULL, 0);
    return;
  }
  PersistBackupStatus result = StatusOk;
  size_t response_length = 0;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionExport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandReadPage, request_id);
    kernel_free(response);
    return;
  } else if (!s_state.store_open || s_state.store_done) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandReadPage, request_id);
    kernel_free(response);
    return;
  } else if (s_state.inventory_generation != persist_backup_inventory_get_generation()) {
    result = StatusStaleSnapshot;
  } else {
    ExportPage page = {
        .data = &response[3],
        .remaining = max_payload - PERSIST_BACKUP_RESPONSE_HEADER_SIZE - 3,
        .crc = s_state.crc,
    };
    bool done = false;
    status_t status = persist_backup_export_page(s_state.export, prv_read_u16(payload),
                                                 prv_export_record, &page, &done);
    if (status == E_AGAIN) {
      result = StatusStaleSnapshot;
    } else if (FAILED(status)) {
      result = prv_status_from_storage(status);
    } else {
      response[0] = done;
      prv_write_u16(&response[1], page.count);
      response_length = 3 + (page.data - &response[3]);
      s_state.crc = page.crc;
      s_state.record_count += page.count;
      s_state.value_bytes += page.value_bytes;
      s_state.store_done = done;
      prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
    }
  }
  prv_unlock();
  prv_send(session, CommandReadPage, request_id, transaction_id, result, response, response_length);
  kernel_free(response);
}

static void prv_handle_close_store(CommSession *session, uint16_t request_id,
                                   uint32_t transaction_id, const uint8_t *payload,
                                   size_t payload_length) {
  uint8_t response[12];
  PersistBackupStatus result = StatusOk;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionExport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandCloseStore, request_id);
    return;
  } else if (payload_length != 0) {
    prv_abort_authenticated_locked(StatusMalformed, CommandCloseStore, request_id);
    return;
  } else if (!s_state.store_open || !s_state.store_done) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandCloseStore, request_id);
    return;
  } else {
    PersistBackupExport *export = s_state.export;
    s_state.export = NULL;
    s_state.store_open = false;
    prv_write_u32(response, s_state.record_count);
    prv_write_u32(&response[4], s_state.value_bytes);
    prv_write_u32(&response[8], s_state.crc);
    prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
    prv_unlock();
    persist_backup_export_close(export);
    prv_send(session, CommandCloseStore, request_id, transaction_id, result, response,
             sizeof(response));
    return;
  }
  prv_unlock();
  prv_send(session, CommandCloseStore, request_id, transaction_id, result, NULL, 0);
}

static void prv_handle_finish_export(CommSession *session, uint16_t request_id,
                                     uint32_t transaction_id, const uint8_t *payload,
                                     size_t payload_length) {
  PersistBackupStatus result = StatusOk;
  PersistBackupState old = {};
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionExport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandFinishExport, request_id);
    return;
  } else if (payload_length != 0) {
    prv_abort_authenticated_locked(StatusMalformed, CommandFinishExport, request_id);
    return;
  } else if (s_state.store_open) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandFinishExport, request_id);
    return;
  } else if (s_state.inventory_generation != persist_backup_inventory_get_generation()) {
    old = prv_take_state();
    result = StatusStaleSnapshot;
  } else {
    old = prv_take_state();
  }
  prv_unlock();
  prv_send(session, CommandFinishExport, request_id, transaction_id, result, NULL, 0);
  prv_release_state(&old);
}

static void prv_handle_begin_store(CommSession *session, uint16_t request_id,
                                   uint32_t transaction_id, const uint8_t *payload,
                                   size_t payload_length) {
  PersistBackupStatus result = StatusOk;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionImport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandBeginStore, request_id);
    return;
  } else if (payload_length != 28) {
    prv_abort_authenticated_locked(StatusMalformed, CommandBeginStore, request_id);
    return;
  } else if (s_state.import) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandBeginStore, request_id);
    return;
  } else if (s_state.imported_store_count == s_state.expected_store_count ||
             prv_read_u32(&payload[16]) >
                 s_state.expected_import_records - s_state.imported_records ||
             prv_read_u32(&payload[20]) > s_state.expected_import_bytes - s_state.imported_bytes) {
    result = StatusLimitExceeded;
  } else {
    Uuid uuid;
    prv_read_uuid(&uuid, payload);
    if (app_install_get_id_for_uuid(&uuid) != INSTALL_ID_INVALID) {
      result = StatusTargetNotEmpty;
    } else {
      status_t status =
          persist_backup_import_begin(&uuid, prv_read_u32(&payload[16]), prv_read_u32(&payload[20]),
                                      prv_read_u32(&payload[24]), &s_state.import);
      if (FAILED(status)) {
        result = status == E_BUSY ? StatusTargetNotEmpty : prv_status_from_storage(status);
      } else {
        s_state.next_sequence = 0;
        s_state.record_count = prv_read_u32(&payload[16]);
        s_state.value_bytes = prv_read_u32(&payload[20]);
        prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
      }
    }
  }
  prv_unlock();
  prv_send(session, CommandBeginStore, request_id, transaction_id, result, NULL, 0);
}

static void prv_handle_put_record(CommSession *session, uint16_t request_id,
                                  uint32_t transaction_id, const uint8_t *payload,
                                  size_t payload_length) {
  PersistBackupStatus result = StatusOk;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionImport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandPutRecord, request_id);
    return;
  } else if (payload_length < 10 || prv_read_u16(&payload[8]) == 0 ||
             payload_length != 10u + (size_t)prv_read_u16(&payload[8])) {
    prv_abort_authenticated_locked(StatusMalformed, CommandPutRecord, request_id);
    return;
  } else if (prv_read_u16(&payload[8]) > PERSIST_BACKUP_VALUE_MAX_LENGTH) {
    prv_abort_authenticated_locked(StatusLimitExceeded, CommandPutRecord, request_id);
    return;
  } else if (!s_state.import || prv_read_u32(payload) != s_state.next_sequence) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandPutRecord, request_id);
    return;
  } else {
    status_t status = persist_backup_import_put(s_state.import, prv_read_u32(&payload[4]),
                                                &payload[10], prv_read_u16(&payload[8]));
    if (FAILED(status)) {
      prv_abort_authenticated_locked(prv_status_from_storage(status), CommandPutRecord, request_id);
      return;
    }
    s_state.next_sequence++;
    prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
  }
  prv_unlock();
  prv_send(session, CommandPutRecord, request_id, transaction_id, result, NULL, 0);
}

static void prv_handle_commit_store(CommSession *session, uint16_t request_id,
                                    uint32_t transaction_id, const uint8_t *payload,
                                    size_t payload_length) {
  PersistBackupStatus result = StatusOk;
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionImport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandCommitStore, request_id);
    return;
  } else if (payload_length != 0 || !s_state.import) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandCommitStore, request_id);
    return;
  } else {
    status_t status = persist_backup_import_commit(s_state.import);
    if (FAILED(status)) {
      prv_abort_authenticated_locked(
          status == E_ERROR ? StatusChecksumMismatch : prv_status_from_storage(status),
          CommandCommitStore, request_id);
      return;
    }
    s_state.import = NULL;
    s_state.imported_store_count++;
    s_state.imported_records += s_state.record_count;
    s_state.imported_bytes += s_state.value_bytes;
    prv_schedule_timeout(PERSIST_BACKUP_IDLE_TIMEOUT_MS);
  }
  prv_unlock();
  prv_send(session, CommandCommitStore, request_id, transaction_id, result, NULL, 0);
}

static void prv_handle_finish_import(CommSession *session, uint16_t request_id,
                                     uint32_t transaction_id, const uint8_t *payload,
                                     size_t payload_length) {
  PersistBackupStatus result = StatusOk;
  PersistBackupState old = {};
  prv_lock();
  if (!prv_is_authenticated(session, transaction_id)) {
    result = StatusUnauthorized;
  } else if (s_state.state != TransactionImport) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandFinishImport, request_id);
    return;
  } else if (payload_length != 0) {
    prv_abort_authenticated_locked(StatusMalformed, CommandFinishImport, request_id);
    return;
  } else if (s_state.import) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandFinishImport, request_id);
    return;
  } else if (s_state.imported_store_count != s_state.expected_store_count ||
             s_state.imported_records != s_state.expected_import_records ||
             s_state.imported_bytes != s_state.expected_import_bytes) {
    prv_abort_authenticated_locked(StatusOutOfOrder, CommandFinishImport, request_id);
    return;
  } else {
    old = prv_take_state();
  }
  prv_unlock();
  prv_send(session, CommandFinishImport, request_id, transaction_id, result, NULL, 0);
  prv_release_state(&old);
}

void persist_backup_endpoint_protocol_msg_callback(CommSession *session, const uint8_t *data,
                                                   size_t length) {
  if (!session || !data || length < PERSIST_BACKUP_HEADER_SIZE) {
    return;
  }
  const uint8_t command = data[0];
  const uint8_t version = data[1];
  const uint16_t request_id = prv_read_u16(&data[2]);
  const uint32_t transaction_id = prv_read_u32(&data[4]);
  const uint8_t *payload = &data[PERSIST_BACKUP_HEADER_SIZE];
  const size_t payload_length = length - PERSIST_BACKUP_HEADER_SIZE;
  if (!comm_session_is_system(session)) {
    prv_send(session, command, request_id, transaction_id, StatusUnauthorized, NULL, 0);
    return;
  }
  if (version != PERSIST_BACKUP_VERSION) {
    prv_send(session, command, request_id, transaction_id, StatusUnsupportedVersion, NULL, 0);
    return;
  }
  switch (command) {
    case CommandGetInfo:
      prv_handle_get_info(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandOpenExport:
    case CommandOpenImport:
      prv_handle_open(session, command, request_id, transaction_id, payload, payload_length);
      break;
    case CommandCancel: {
      PersistBackupState old = {};
      prv_lock();
      if (!prv_is_authenticated(session, transaction_id)) {
        prv_unlock();
        prv_send(session, command, request_id, transaction_id, StatusUnauthorized, NULL, 0);
        break;
      }
      if (payload_length != 0) {
        prv_abort_authenticated_locked(StatusMalformed, command, request_id);
        break;
      }
      old = prv_take_state();
      prv_unlock();
      prv_send(session, command, request_id, transaction_id, StatusOk, NULL, 0);
      persist_backup_confirmation_cancel(prv_authorization_context(old.authorization_request_id));
      prv_release_state(&old);
      break;
    }
    case CommandListStores:
      prv_handle_list(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandOpenStore:
      prv_handle_open_store(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandReadPage:
      prv_handle_read_page(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandCloseStore:
      prv_handle_close_store(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandFinishExport:
      prv_handle_finish_export(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandBeginStore:
      prv_handle_begin_store(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandPutRecord:
      prv_handle_put_record(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandCommitStore:
      prv_handle_commit_store(session, request_id, transaction_id, payload, payload_length);
      break;
    case CommandFinishImport:
      prv_handle_finish_import(session, request_id, transaction_id, payload, payload_length);
      break;
    default:
      prv_lock();
      if (prv_is_authenticated(session, transaction_id)) {
        prv_abort_authenticated_locked(StatusOutOfOrder, command, request_id);
      } else {
        prv_unlock();
        prv_send(session, command, request_id, transaction_id, StatusMalformed, NULL, 0);
      }
      break;
  }
}

void persist_backup_endpoint_handle_comm_session_event(const PebbleCommSessionEvent *event) {
  if (!event || !event->is_system || event->is_open) {
    return;
  }
  PersistBackupState old = {};
  prv_lock();
  if (s_state.state != TransactionIdle) {
    old = prv_take_state();
  }
  prv_unlock();
  persist_backup_confirmation_cancel(prv_authorization_context(old.authorization_request_id));
  prv_release_state(&old);
}

void persist_backup_endpoint_init(void) {
  if (!s_mutex) {
    s_mutex = mutex_create();
  }
  if (s_timeout_timer == TIMER_INVALID_ID) {
    s_timeout_timer = new_timer_create();
  }
}
