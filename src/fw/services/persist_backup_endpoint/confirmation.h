/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

typedef void (*PersistBackupConfirmationCallback)(bool approved, void *context);

void persist_backup_confirmation_request(bool is_import, PersistBackupConfirmationCallback callback,
                                         void *context);
void persist_backup_confirmation_cancel(void *context);
