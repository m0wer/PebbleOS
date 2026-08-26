# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_library()
pbl_library_sources(
  endpoint.c
  confirmation.c
)
pbl_service_gettext(persist_backup_endpoint confirmation.c)
