/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Persist service
//!
//! The persist service manages persistent app key-value stores. A persistent
//! store is simply a SettingsFile identified by the app's UUID. The service
//! manages the creation, opening and deletion of persist stores so that an app
//! and its worker can both access the same file through a single file handle
//! and SettingsFile state object.
//!
//! The persist service makes no attempt to make SettingsFile reentrant; it is
//! the caller's responsibility to enforce mutual exclusion and prevent
//! concurrent access to the SettingsFile.

#include <stdint.h>
#include <stddef.h>

#include "pbl/util/uuid.h"
#include "system/status_codes.h"

typedef struct SettingsFile SettingsFile;
typedef struct PersistBackupExport PersistBackupExport;
typedef struct PersistBackupImport PersistBackupImport;

//! Maximum persisted value length, shared with the SDK persist contract.
#define PERSIST_BACKUP_VALUE_MAX_LENGTH 256

//! Called for each valid persist store while the persist service lock is held.
typedef bool (*PersistBackupInventoryCallback)(const Uuid *uuid, void *context);
//! Called for each exported record while the persist service lock is held.
typedef bool (*PersistBackupRecordCallback)(uint32_t key, const uint8_t *value, size_t value_len,
                                            void *context);

//! Initialize the persist service.
void persist_service_init(void);

//! Get the per-app persistent storage capacity in bytes.
size_t persist_service_get_max_size(void);

//! Lock and get the persist store for the given app.
SettingsFile *persist_service_lock_and_get_store(const Uuid *uuid);

//! Unlock the given persist store.
void persist_service_unlock_store(SettingsFile *store);

//! Call during each process's startup.
void persist_service_client_open(const Uuid *uuid);

//! Call once after proces exits to clean it up.
void persist_service_client_close(const Uuid *uuid);

//! Deletes the app's persist file.
status_t persist_service_delete_file(const Uuid *uuid);

//! Notify the service of a successful app persist mutation while its lock is held.
void persist_service_store_did_change(SettingsFile *store);

//! Get the generation that changes when a persist file is created or deleted.
uint32_t persist_backup_inventory_get_generation(void);
//! Enumerate valid persist filenames. callback must be non-NULL and executes while locked.
status_t persist_backup_inventory_each(PersistBackupInventoryCallback callback, void *context);

//! Pin an existing store for a paged export. uuid and export_out must be non-NULL.
status_t persist_backup_export_open(const Uuid *uuid, PersistBackupExport **export_out);
//! Export up to max_records. callback and done_out must be non-NULL.
//! Returns E_AGAIN if the store changed since open or E_ERROR for malformed records.
status_t persist_backup_export_page(PersistBackupExport *export, uint32_t max_records,
                                    PersistBackupRecordCallback callback, void *context,
                                    bool *done_out);
//! Release an export pin.
void persist_backup_export_close(PersistBackupExport *export);

//! Begin an absent-store import with the expected canonical record totals and CRC.
//! uuid and import_out must be non-NULL. Returns E_BUSY for an existing or active target.
status_t persist_backup_import_begin(const Uuid *uuid, uint32_t expected_record_count,
                                     uint32_t expected_value_bytes, uint32_t expected_crc,
                                     PersistBackupImport **import_out);
//! Add one nonempty persist record to an active import. value must be non-NULL and at most 256
//! bytes.
status_t persist_backup_import_put(PersistBackupImport *import, uint32_t key, const uint8_t *value,
                                   size_t value_len);
//! Validate totals and CRC, then clear the rollback marker.
status_t persist_backup_import_commit(PersistBackupImport *import);
//! Remove an incomplete target store and its rollback marker.
void persist_backup_import_abort(PersistBackupImport *import);

#if UNITTEST
//! Simulate power loss after an import record has been persisted.
void persist_backup_import_test_interrupt(PersistBackupImport *import);
//! Run the same rollback recovery path used during service initialization.
void persist_service_test_recover_interrupted_import(void);
#endif
