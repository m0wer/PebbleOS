/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "confirmation.h"

#include "applib/ui/action_bar_layer.h"
#include "applib/ui/dialogs/confirmation_dialog.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "pbl/services/i18n/i18n.h"

typedef enum {
  ConfirmationCommandRequest,
  ConfirmationCommandCancel,
} ConfirmationCommandType;

typedef struct {
  ConfirmationCommandType type;
  bool is_import;
  PersistBackupConfirmationCallback callback;
  void *context;
} ConfirmationCommand;

typedef struct {
  ConfirmationDialog *dialog;
  PersistBackupConfirmationCallback callback;
  void *context;
} PersistBackupConfirmation;

static PersistBackupConfirmation s_confirmation;

static void prv_finish(bool approved) {
  ConfirmationDialog *dialog = s_confirmation.dialog;
  PersistBackupConfirmationCallback callback = s_confirmation.callback;
  void *context = s_confirmation.context;
  s_confirmation = (PersistBackupConfirmation){};
  if (dialog) {
    confirmation_dialog_pop(dialog);
  }
  if (callback) {
    callback(approved, context);
  }
}

static void prv_click_handler(ClickRecognizerRef recognizer, void *context) {
  prv_finish(click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_click_handler);
}

static void prv_process_command(void *data) {
  ConfirmationCommand *command = data;
  if (command->type == ConfirmationCommandCancel) {
    if (s_confirmation.context == command->context) {
      prv_finish(false);
    }
    kernel_free(command);
    return;
  }

  if (s_confirmation.callback) {
    command->callback(false, command->context);
    kernel_free(command);
    return;
  }

  ConfirmationDialog *dialog = confirmation_dialog_create(i18n_get("App Data", command));
  if (!dialog) {
    command->callback(false, command->context);
    i18n_free_all(command);
    kernel_free(command);
    return;
  }
  Dialog *dialog_base = confirmation_dialog_get_dialog(dialog);
  dialog_set_text(dialog_base, i18n_get(command->is_import ? "Restore app data from your phone?"
                                                           : "Share app data with your phone?",
                                        dialog));
  confirmation_dialog_set_click_config_provider(dialog, prv_click_config);
  s_confirmation = (PersistBackupConfirmation){
      .dialog = dialog,
      .callback = command->callback,
      .context = command->context,
  };
  confirmation_dialog_push(dialog, modal_manager_get_window_stack(ModalPriorityGeneric));
  i18n_free_all(command);
  i18n_free_all(dialog);
  kernel_free(command);
}

static void prv_enqueue(ConfirmationCommandType type, bool is_import,
                        PersistBackupConfirmationCallback callback, void *context) {
  ConfirmationCommand *command = kernel_malloc(sizeof(*command));
  if (!command) {
    if (type == ConfirmationCommandRequest && callback) {
      callback(false, context);
    }
    return;
  }
  *command = (ConfirmationCommand){
      .type = type,
      .is_import = is_import,
      .callback = callback,
      .context = context,
  };
  launcher_task_add_callback(prv_process_command, command);
}

void persist_backup_confirmation_request(bool is_import, PersistBackupConfirmationCallback callback,
                                         void *context) {
  prv_enqueue(ConfirmationCommandRequest, is_import, callback, context);
}

void persist_backup_confirmation_cancel(void *context) {
  prv_enqueue(ConfirmationCommandCancel, false, NULL, context);
}
