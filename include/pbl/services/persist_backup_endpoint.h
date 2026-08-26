/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct CommSession CommSession;
typedef struct PebbleCommSessionEvent PebbleCommSessionEvent;

void persist_backup_endpoint_init(void);
void persist_backup_endpoint_protocol_msg_callback(CommSession *session, const uint8_t *data,
                                                   size_t length);
void persist_backup_endpoint_handle_comm_session_event(const PebbleCommSessionEvent *event);
