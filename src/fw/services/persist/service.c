/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/pbl_malloc.h"
#include "pbl/os/mutex.h"
#include "process_management/app_install_manager.h"
#include "pbl/services/filesystem/app_file.h"
#include "pbl/services/filesystem/pfs.h"
#include "pbl/services/settings/settings_file.h"
#include "pbl/util/crc32.h"
#include <pbl/logging/logging.h>
#include "system/passert.h"
#include "pbl/util/attributes.h"
#include "pbl/util/list.h"
#include "pbl/util/math.h"
#include "util/units.h"

PBL_LOG_MODULE_DEFINE(service_persist, CONFIG_SERVICE_PERSIST_LOG_LEVEL);

#define PERSIST_STORAGE_MAX_SPACE MiBYTES(1)
#define PERSIST_STORAGE_INITIAL_ALLOC KiBYTES(4)

typedef struct PersistStore {
  ListNode  list_node;
  Uuid uuid;
  SettingsFile file;
  bool file_open;
  uint8_t usage_count;          //!< How many clients are using this store
  uint8_t backup_ref_count;
  uint32_t mutation_generation;
} PersistStore;

struct PersistBackupExport {
  PersistStore *store;
  uint32_t generation;
  uint32_t cursor;
};

struct PersistBackupImport {
  Uuid uuid;
  SettingsFile file;
  uint32_t expected_record_count;
  uint32_t expected_value_bytes;
  uint32_t expected_crc;
  uint32_t record_count;
  uint32_t value_bytes;
  uint32_t crc;
};

typedef struct {
  uint32_t remaining;
  PersistBackupRecordCallback callback;
  void *context;
  status_t status;
} ExportPageContext;

// Each open client has a PersistStore structure linked into this list. If both
// a worker and foreground app of the same UUID are running, then they share the
// same store.
static ListNode *s_client_stores;
static PebbleMutex *s_mutex;
static uint32_t s_inventory_generation;
static PersistBackupImport *s_active_import;

static bool prv_uuid_list_filter(ListNode* node, void* data) {
  const Uuid *uuid = data;
  PersistStore* store = (PersistStore*)node;
  return uuid_equal(&store->uuid, uuid);
}

static PersistStore * prv_find_open_store(const Uuid *uuid) {
    return (PersistStore *)list_find(s_client_stores, prv_uuid_list_filter,
                                     (void *)uuid);
}

static ALWAYS_INLINE void prv_lock(void) {
  mutex_lock_with_lr(s_mutex, (uint32_t)__builtin_return_address(0));
}

static inline void prv_unlock(void) {
  mutex_unlock(s_mutex);
}

// "ps" prefix + 32 hex chars (16-byte UUID) + NUL.
#define PERSIST_FILE_NAME_MAX_LENGTH sizeof("ps000102030405060708090a0b0c0d0e0f")
#define PERSIST_ROLLBACK_FILE_NAME "psrb"

typedef struct PACKED {
  uint32_t magic;
  Uuid uuid;
  uint32_t record_count;
  uint32_t value_bytes;
  uint32_t crc;
  uint32_t marker_crc;
} PersistRollbackMarker;

#define PERSIST_ROLLBACK_MARKER_MAGIC 0x50524231

static status_t prv_get_file_name(char *name, size_t buf_len, const Uuid *uuid) {
  // Persist files are named "ps<uuid-hex>". The "ps" prefix indicates the file
  // is in SettingsFile format. The UUID is stable across reinstalls, so the
  // file follows the app regardless of its (volatile) AppInstallId.
  const uint8_t *b = (const uint8_t *)uuid;
  return snprintf(name, buf_len,
                  "ps%02x%02x%02x%02x%02x%02x%02x%02x"
                  "%02x%02x%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static bool prv_file_exists(const char *name) {
  int fd = pfs_open(name, OP_FLAG_READ, 0, 0);
  if (fd < 0) {
    return false;
  }
  pfs_close(fd);
  return true;
}

static void prv_free_store_if_unused(PersistStore *store) {
  if (store->usage_count || store->backup_ref_count) {
    return;
  }
  if (store->file_open) {
    settings_file_close(&store->file);
  }
  list_remove(&store->list_node, &s_client_stores, NULL);
  kernel_free(store);
}

static status_t prv_open_store_file(PersistStore *store, bool create) {
  if (store->file_open) {
    return S_SUCCESS;
  }
  char filename[PERSIST_FILE_NAME_MAX_LENGTH];
  status_t status = prv_get_file_name(filename, sizeof(filename), &store->uuid);
  if (FAILED(status)) {
    return status;
  }
  const bool existed = prv_file_exists(filename);
  if (!create && !existed) {
    return E_DOES_NOT_EXIST;
  }
  status = settings_file_open_growable(&store->file, filename, PERSIST_STORAGE_MAX_SPACE,
                                       PERSIST_STORAGE_INITIAL_ALLOC);
  if (PASSED(status)) {
    store->file_open = true;
    if (!existed) {
      s_inventory_generation++;
    }
  }
  return status;
}

static uint32_t prv_marker_crc(const PersistRollbackMarker *marker) {
  return crc32(CRC32_INIT, marker, offsetof(PersistRollbackMarker, marker_crc));
}

static bool prv_marker_is_valid(const PersistRollbackMarker *marker) {
  return marker->magic == PERSIST_ROLLBACK_MARKER_MAGIC &&
         marker->marker_crc == prv_marker_crc(marker);
}

static status_t prv_write_marker(const PersistRollbackMarker *marker) {
  int fd = pfs_open(PERSIST_ROLLBACK_FILE_NAME, OP_FLAG_WRITE, FILE_TYPE_STATIC, sizeof(*marker));
  if (fd < 0) {
    return fd;
  }
  status_t status = pfs_write(fd, marker, sizeof(*marker));
  if (status == (int)sizeof(*marker)) {
    status = pfs_close(fd);
  } else {
    pfs_close_and_remove(fd);
    status = (status >= 0) ? E_INTERNAL : status;
  }
  return status;
}

static void prv_recover_interrupted_import(void) {
  int fd = pfs_open(PERSIST_ROLLBACK_FILE_NAME, OP_FLAG_READ, 0, 0);
  if (fd < 0) {
    return;
  }
  PersistRollbackMarker marker;
  const bool valid = pfs_get_file_size(fd) == sizeof(marker) &&
                     pfs_read(fd, &marker, sizeof(marker)) == (int)sizeof(marker) &&
                     prv_marker_is_valid(&marker);
  pfs_close(fd);
  if (!valid) {
    PBL_LOG_WRN("Removing corrupt persist rollback marker");
    pfs_remove(PERSIST_ROLLBACK_FILE_NAME);
    return;
  }

  char filename[PERSIST_FILE_NAME_MAX_LENGTH];
  if (PASSED(prv_get_file_name(filename, sizeof(filename), &marker.uuid))) {
    // A valid marker proves this file was created only for an unfinished import.
    if (PASSED(pfs_remove(filename))) {
      s_inventory_generation++;
    }
  }
  pfs_remove(PERSIST_ROLLBACK_FILE_NAME);
}

status_t persist_service_delete_file(const Uuid *uuid) {
  char name[PERSIST_FILE_NAME_MAX_LENGTH];

  status_t status = prv_get_file_name(name, sizeof(name), uuid);
  if (FAILED(status)) {
    return status;
  }
  prv_lock();
  PersistStore *store = prv_find_open_store(uuid);
  if (store && (store->usage_count || store->backup_ref_count)) {
    prv_unlock();
    return E_BUSY;
  }
  status = pfs_remove(name);
  if (PASSED(status)) {
    s_inventory_generation++;
  }
  prv_unlock();
  return status;
}

static bool prv_bad_persist_file_filter(const char *filename) {
  return is_app_file_name(filename) &&
         strcmp(filename + APP_FILE_NAME_PREFIX_LENGTH, "persist") == 0;
}

size_t persist_service_get_max_size(void) {
  return PERSIST_STORAGE_MAX_SPACE;
}

// Persist files used to be named "ps%06d", where the id was allocated by the
// legacy persist_map (the "pmap" file), a UUID->id table. They are now named
// "ps<uuid-hex>" directly. The following migrates existing files to the new
// scheme and removes the now-unused pmap.
// TODO: remove this migration once all devices have upgraded.

#define LEGACY_PMAP_FILE_NAME "pmap"
#define LEGACY_PERSIST_FILE_NAME_MAX_LENGTH sizeof("ps000001")
#define LEGACY_PMAP_EOF_ID ((int)(~0))

typedef struct PACKED {
  uint16_t version;
} LegacyPersistMapHeader;

typedef struct PACKED {
  int id;
  Uuid uuid;
} LegacyPersistMapField;

static status_t prv_copy_file(const char *from, const char *to) {
  int from_fd = pfs_open(from, OP_FLAG_READ, 0, 0);
  if (from_fd < 0) {
    return from_fd;
  }

  size_t size = pfs_get_file_size(from_fd);
  int to_fd = pfs_open(to, OP_FLAG_WRITE, FILE_TYPE_STATIC, size);
  if (to_fd < 0) {
    pfs_close(from_fd);
    return to_fd;
  }

  status_t rv = S_SUCCESS;
  const size_t chunk_size = 128;
  uint8_t *buf = kernel_malloc(chunk_size);
  if (buf == NULL) {
    rv = E_OUT_OF_MEMORY;
  } else {
    size_t remaining = size;
    while (remaining > 0) {
      size_t n = MIN(remaining, chunk_size);
      if ((rv = pfs_read(from_fd, buf, n)) != (int)n) {
        rv = (rv >= 0) ? E_INTERNAL : rv;
        break;
      }
      if ((rv = pfs_write(to_fd, buf, n)) != (int)n) {
        rv = (rv >= 0) ? E_INTERNAL : rv;
        break;
      }
      remaining -= n;
    }
    kernel_free(buf);
  }

  pfs_close(from_fd);
  pfs_close(to_fd);
  return rv;
}

static void prv_migrate_legacy_persist_files(void) {
  int fd = pfs_open(LEGACY_PMAP_FILE_NAME, OP_FLAG_READ, 0, 0);
  if (fd < 0) {
    // No legacy map, nothing to migrate.
    return;
  }

  pfs_seek(fd, sizeof(LegacyPersistMapHeader), FSeekSet);

  LegacyPersistMapField field;
  while (pfs_read(fd, (uint8_t *)&field, sizeof(field)) == (int)sizeof(field)) {
    if (field.id == LEGACY_PMAP_EOF_ID) {
      break;
    }

    char old_name[LEGACY_PERSIST_FILE_NAME_MAX_LENGTH];
    snprintf(old_name, sizeof(old_name), "ps%06d", field.id);

    char new_name[PERSIST_FILE_NAME_MAX_LENGTH];
    if (FAILED(prv_get_file_name(new_name, sizeof(new_name), &field.uuid))) {
      continue;
    }

    const bool new_file_existed = prv_file_exists(new_name);
    if (PASSED(prv_copy_file(old_name, new_name))) {
      if (!new_file_existed) {
        s_inventory_generation++;
      }
      pfs_remove(old_name);
    }
  }

  pfs_close(fd);
  pfs_remove(LEGACY_PMAP_FILE_NAME);
}

// Designed to be called once during reset
void persist_service_init(void) {
  s_mutex = mutex_create();
  s_inventory_generation = 0;

  prv_recover_interrupted_import();
  prv_migrate_legacy_persist_files();

  // Find and delete any AppInstallId-indexed persist files. Due to PBL-16663
  // (affecting FW 3.0-dp5 thru -dp7), the AppInstallId in the file name may not
  // correspond to the app that the persist file originally belonged to. Since
  // we can't be sure that the persist files correspond to the current
  // AppInstallId, the safest thing to do is to simply blow them away.
  // TODO: remove this code before FW 3.0-golden.
  PFSFileListEntry *bad_file_list = pfs_create_file_list(
      prv_bad_persist_file_filter);
  PFSFileListEntry *iter = bad_file_list;
  while (iter) {
    pfs_remove(iter->name);
    iter = (PFSFileListEntry *)iter->list_node.next;
  }
  pfs_delete_file_list(bad_file_list);
}

// Return a pointer to the store for the given UUID. Each task that uses persist
// must call persist_service_client_open() to create/open the store during its
// startup and persist_service_client_close() during its shutdown.
//
// The SettingsFile is opened/created lazily. A persist file will not be
// created for an app unless it calls a persist function.
//
// The persist service mutex is locked when this function is called. It will
// only be unlocked after a call to persist_service_unlock(). While the global
// persist service mutex is currently used, the API is designed such that a
// per-file mutex could be used without altering the callers.
SettingsFile * persist_service_lock_and_get_store(const Uuid *uuid) {
  prv_lock();
  PersistStore *store = prv_find_open_store(uuid);
  PBL_ASSERTN(store);
  if (!store->file_open) {
    PBL_ASSERTN(PASSED(prv_open_store_file(store, true)));
  }
  return &store->file;
}

void persist_service_unlock_store(SettingsFile *store) {
  prv_unlock();
}

static bool prv_store_file_filter(ListNode *node, void *data) {
  PersistStore *store = (PersistStore *)node;
  return store->file_open && &store->file == data;
}

void persist_service_store_did_change(SettingsFile *file) {
  PersistStore *store = (PersistStore *)list_find(s_client_stores, prv_store_file_filter, file);
  PBL_ASSERTN(store);
  store->mutation_generation++;
}

// Create a store for a client of the given UUID it doesn't already exist. If it
// exists already (another client with the same UUID is running), then just
// increment its usage count. This is called by the process startup code
// (app_state_init() or worker_state_init()).
void persist_service_client_open(const Uuid *uuid) {
  prv_lock();
  {
    PersistStore *store = prv_find_open_store(uuid);
    if (store) {
      store->usage_count++;
    } else {
      store = kernel_malloc_check(sizeof(*store));
      *store = (PersistStore) {
        .uuid = *uuid,
        .usage_count = 1,
        .file_open = false,
      };
      s_client_stores = list_insert_before(s_client_stores, &store->list_node);
    }
  }
  prv_unlock();
}

// Release the store for the given UUID. Called by ProcessManager to clean up
// after a task exists. If there are no other processes using the same store, it
// will be freed
void persist_service_client_close(const Uuid *uuid) {
  prv_lock();
  {
    PersistStore *store = prv_find_open_store(uuid);
    PBL_ASSERTN(store &&
                list_contains(s_client_stores, &store->list_node) &&
                store->usage_count >= 1);

    if (--store->usage_count == 0) {
      prv_free_store_if_unused(store);
    }
  }
  prv_unlock();
}

static int prv_hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

static bool prv_parse_persist_filename(const char *filename, Uuid *uuid_out) {
  if (strlen(filename) != PERSIST_FILE_NAME_MAX_LENGTH - 1 || filename[0] != 'p' ||
      filename[1] != 's') {
    return false;
  }
  uint8_t *bytes = (uint8_t *)uuid_out;
  for (size_t i = 0; i < UUID_SIZE; ++i) {
    const int high = prv_hex_value(filename[2 + i * 2]);
    const int low = prv_hex_value(filename[3 + i * 2]);
    if (high < 0 || low < 0) {
      return false;
    }
    bytes[i] = (high << 4) | low;
  }
  return true;
}

uint32_t persist_backup_inventory_get_generation(void) {
  prv_lock();
  const uint32_t generation = s_inventory_generation;
  prv_unlock();
  return generation;
}

status_t persist_backup_inventory_each(PersistBackupInventoryCallback callback, void *context) {
  if (!callback) {
    return E_INVALID_ARGUMENT;
  }
  prv_lock();
  PFSFileListEntry *entries = pfs_create_file_list(NULL);
  for (PFSFileListEntry *entry = entries; entry;
       entry = (PFSFileListEntry *)entry->list_node.next) {
    Uuid uuid;
    if (prv_parse_persist_filename(entry->name, &uuid) && !callback(&uuid, context)) {
      break;
    }
  }
  pfs_delete_file_list(entries);
  prv_unlock();
  return S_SUCCESS;
}

static SettingsFileEachCursorResult prv_export_record_cb(SettingsFile *file,
                                                         SettingsRecordInfo *info, void *context) {
  ExportPageContext *page_context = context;
  if (info->key_len != sizeof(uint32_t) || info->val_len == 0 ||
      info->val_len > PERSIST_BACKUP_VALUE_MAX_LENGTH) {
    page_context->status = E_ERROR;
    return SettingsFileEachCursorResult_NotConsumedAndStop;
  }
  uint32_t key;
  uint8_t value[PERSIST_BACKUP_VALUE_MAX_LENGTH];
  info->get_key(file, &key, sizeof(key));
  info->get_val(file, value, info->val_len);
  if (!page_context->callback(key, value, info->val_len, page_context->context)) {
    return SettingsFileEachCursorResult_NotConsumedAndStop;
  }
  return --page_context->remaining ? SettingsFileEachCursorResult_ConsumedAndContinue
                                   : SettingsFileEachCursorResult_ConsumedAndStop;
}

status_t persist_backup_export_open(const Uuid *uuid, PersistBackupExport **export_out) {
  if (!uuid || !export_out) {
    return E_INVALID_ARGUMENT;
  }
  prv_lock();
  PersistStore *store = prv_find_open_store(uuid);
  if (!store) {
    char filename[PERSIST_FILE_NAME_MAX_LENGTH];
    status_t status = prv_get_file_name(filename, sizeof(filename), uuid);
    if (FAILED(status) || !prv_file_exists(filename)) {
      prv_unlock();
      return FAILED(status) ? status : E_DOES_NOT_EXIST;
    }
    store = kernel_malloc(sizeof(*store));
    if (!store) {
      prv_unlock();
      return E_OUT_OF_MEMORY;
    }
    *store = (PersistStore){.uuid = *uuid};
    s_client_stores = list_insert_before(s_client_stores, &store->list_node);
  }
  status_t status = prv_open_store_file(store, false);
  if (FAILED(status)) {
    prv_free_store_if_unused(store);
    prv_unlock();
    return status;
  }
  PersistBackupExport *export = kernel_malloc(sizeof(*export));
  if (!export) {
    prv_free_store_if_unused(store);
    prv_unlock();
    return E_OUT_OF_MEMORY;
  }
  store->backup_ref_count++;
  *export = (PersistBackupExport){
      .store = store,
      .generation = store->mutation_generation,
  };
  *export_out = export;
  prv_unlock();
  return S_SUCCESS;
}

status_t persist_backup_export_page(PersistBackupExport *export, uint32_t max_records,
                                    PersistBackupRecordCallback callback, void *context,
                                    bool *done_out) {
  if (!export || !callback || !done_out || max_records == 0) {
    return E_INVALID_ARGUMENT;
  }
  prv_lock();
  if (export->store->mutation_generation != export->generation) {
    prv_unlock();
    return E_AGAIN;
  }
  ExportPageContext page_context = {
      .remaining = max_records,
      .callback = callback,
      .context = context,
      .status = S_SUCCESS,
  };
  uint32_t next_cursor = export->cursor;
  status_t status =
      settings_file_each_cursor(&export->store->file, export->cursor, prv_export_record_cb,
                                &page_context, &next_cursor, done_out);
  if (PASSED(status) && FAILED(page_context.status)) {
    status = page_context.status;
  }
  if (PASSED(status)) {
    export->cursor = next_cursor;
  }
  prv_unlock();
  return status;
}

void persist_backup_export_close(PersistBackupExport *export) {
  if (!export) {
    return;
  }
  prv_lock();
  PBL_ASSERTN(export->store->backup_ref_count > 0);
  export->store->backup_ref_count--;
  prv_free_store_if_unused(export->store);
  kernel_free(export);
  prv_unlock();
}

static void prv_update_import_crc(PersistBackupImport *import, uint32_t key, const uint8_t *value,
                                  size_t value_len) {
  const uint8_t record_header[] = {
      (uint8_t)(key >> 24), (uint8_t)(key >> 16),      (uint8_t)(key >> 8),
      (uint8_t)key,         (uint8_t)(value_len >> 8), (uint8_t)value_len,
  };
  import->crc = crc32(import->crc, record_header, sizeof(record_header));
  import->crc = crc32(import->crc, value, value_len);
}

static void prv_abort_import(PersistBackupImport *import) {
  char filename[PERSIST_FILE_NAME_MAX_LENGTH];
  settings_file_close(&import->file);
  if (PASSED(prv_get_file_name(filename, sizeof(filename), &import->uuid)) &&
      PASSED(pfs_remove(filename))) {
    s_inventory_generation++;
  }
  pfs_remove(PERSIST_ROLLBACK_FILE_NAME);
  s_active_import = NULL;
  kernel_free(import);
}

status_t persist_backup_import_begin(const Uuid *uuid, uint32_t expected_record_count,
                                     uint32_t expected_value_bytes, uint32_t expected_crc,
                                     PersistBackupImport **import_out) {
  if (!uuid || !import_out) {
    return E_INVALID_ARGUMENT;
  }
  if (expected_value_bytes > PERSIST_STORAGE_MAX_SPACE) {
    return E_RANGE;
  }
  prv_lock();
  if (s_active_import || prv_find_open_store(uuid)) {
    prv_unlock();
    return E_BUSY;
  }
  char filename[PERSIST_FILE_NAME_MAX_LENGTH];
  status_t status = prv_get_file_name(filename, sizeof(filename), uuid);
  if (FAILED(status) || prv_file_exists(filename)) {
    prv_unlock();
    return FAILED(status) ? status : E_BUSY;
  }
  if (prv_file_exists(PERSIST_ROLLBACK_FILE_NAME)) {
    prv_unlock();
    return E_BUSY;
  }
  PersistBackupImport *import = kernel_malloc(sizeof(*import));
  if (!import) {
    prv_unlock();
    return E_OUT_OF_MEMORY;
  }
  PersistRollbackMarker marker = {
      .magic = PERSIST_ROLLBACK_MARKER_MAGIC,
      .uuid = *uuid,
      .record_count = expected_record_count,
      .value_bytes = expected_value_bytes,
      .crc = expected_crc,
  };
  marker.marker_crc = prv_marker_crc(&marker);
  status = prv_write_marker(&marker);
  if (FAILED(status)) {
    kernel_free(import);
    prv_unlock();
    return status;
  }
  *import = (PersistBackupImport){
      .uuid = *uuid,
      .expected_record_count = expected_record_count,
      .expected_value_bytes = expected_value_bytes,
      .expected_crc = expected_crc,
      .crc = CRC32_INIT,
  };
  status = settings_file_open_growable(&import->file, filename, PERSIST_STORAGE_MAX_SPACE,
                                       PERSIST_STORAGE_INITIAL_ALLOC);
  if (FAILED(status)) {
    if (PASSED(pfs_remove(filename))) {
      s_inventory_generation++;
    }
    pfs_remove(PERSIST_ROLLBACK_FILE_NAME);
    kernel_free(import);
    prv_unlock();
    return status;
  }
  s_inventory_generation++;
  s_active_import = import;
  *import_out = import;
  prv_unlock();
  return S_SUCCESS;
}

status_t persist_backup_import_put(PersistBackupImport *import, uint32_t key, const uint8_t *value,
                                   size_t value_len) {
  if (!import || !value || value_len == 0 || value_len > PERSIST_BACKUP_VALUE_MAX_LENGTH) {
    return E_INVALID_ARGUMENT;
  }
  prv_lock();
  if (import != s_active_import) {
    prv_unlock();
    return E_INVALID_OPERATION;
  }
  if (import->record_count == import->expected_record_count ||
      value_len > import->expected_value_bytes - import->value_bytes ||
      settings_file_exists(&import->file, &key, sizeof(key))) {
    prv_unlock();
    return E_INVALID_ARGUMENT;
  }
  status_t status = settings_file_set(&import->file, &key, sizeof(key), value, value_len);
  if (PASSED(status)) {
    prv_update_import_crc(import, key, value, value_len);
    import->record_count++;
    import->value_bytes += value_len;
  }
  prv_unlock();
  return status;
}

status_t persist_backup_import_commit(PersistBackupImport *import) {
  if (!import) {
    return E_INVALID_ARGUMENT;
  }
  prv_lock();
  if (import != s_active_import) {
    prv_unlock();
    return E_INVALID_OPERATION;
  }
  if (import->record_count != import->expected_record_count ||
      import->value_bytes != import->expected_value_bytes) {
    prv_unlock();
    return E_INVALID_ARGUMENT;
  }
  if (import->crc != import->expected_crc) {
    prv_unlock();
    return E_ERROR;
  }
  status_t status = pfs_remove(PERSIST_ROLLBACK_FILE_NAME);
  if (PASSED(status)) {
    settings_file_close(&import->file);
    s_active_import = NULL;
    kernel_free(import);
  }
  prv_unlock();
  return status;
}

void persist_backup_import_abort(PersistBackupImport *import) {
  if (!import) {
    return;
  }
  prv_lock();
  if (import == s_active_import) {
    prv_abort_import(import);
  }
  prv_unlock();
}

#if UNITTEST
void persist_backup_import_test_interrupt(PersistBackupImport *import) {
  if (!import) {
    return;
  }
  prv_lock();
  if (import == s_active_import) {
    settings_file_close(&import->file);
    s_active_import = NULL;
    kernel_free(import);
  }
  prv_unlock();
}

void persist_service_test_recover_interrupted_import(void) {
  prv_lock();
  PBL_ASSERTN(!s_active_import);
  prv_recover_interrupted_import();
  prv_unlock();
}
#endif
