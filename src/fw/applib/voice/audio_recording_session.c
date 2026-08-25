/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "audio_recording_session.h"
#include "audio_recording_session_private.h"
#include "voice_window_private.h"

#include "applib/applib_malloc.auto.h"
#include "applib/voice/voice_window.h"
#include "process_management/app_install_manager.h"
#include "syscall/syscall.h"

#include <pbl/logging/logging.h>

#ifdef CONFIG_MIC

static void prv_stop_session(AudioRecordingSession *session) {
  session->in_progress = false;
  event_service_client_unsubscribe(&session->dictation_result_sub);
  if (pebble_task_get_current() == PebbleTask_App) {
    event_service_client_unsubscribe(&session->app_focus_sub);
  }
}

static void prv_handle_recording_result(PebbleEvent *event, void *context) {
  AudioRecordingSession *session = context;
  const AudioRecordingSessionStatus status = (AudioRecordingSessionStatus)event->dictation.result;

  session->in_progress = false;
  voice_window_reset(session->voice_window);

  // The callback may destroy the session, so do not dereference it afterwards.
  session->callback(session, status, session->context);
}

static void prv_app_focus_handler(PebbleEvent *event, void *context) {
  AudioRecordingSession *session = context;
  if (event->app_focus.in_focus) {
    event_service_client_subscribe(&session->dictation_result_sub);
    voice_window_regain_focus(session->voice_window);
  } else {
    event_service_client_unsubscribe(&session->dictation_result_sub);
    voice_window_lose_focus(session->voice_window);
  }
}

#endif

AudioRecordingSession *audio_recording_session_create(AudioRecordingSessionStatusCallback callback,
                                                      void *context) {
#ifdef CONFIG_MIC
  if (!callback) {
    return NULL;
  }

  const bool from_app = (pebble_task_get_current() == PebbleTask_App) &&
                        !app_install_id_from_system(sys_process_manager_get_current_process_id());
  if (from_app && !sys_system_pp_has_capability(CommSessionVoiceApiSupport)) {
    PBL_LOG_WRN(
        "No phone connected or phone app does not support app-initiated recording sessions");
    return NULL;
  }

  AudioRecordingSession *session = applib_type_malloc(AudioRecordingSession);
  if (!session) {
    return NULL;
  }

  VoiceWindow *voice_window = voice_window_create(NULL, 0, VoiceEndpointSessionTypeRecording);
  if (!voice_window) {
    applib_free(session);
    return NULL;
  }

  *session = (AudioRecordingSession){
      .callback = callback,
      .context = context,
      .voice_window = voice_window,
      .dictation_result_sub =
          (EventServiceInfo){
              .type = PEBBLE_DICTATION_EVENT,
              .handler = prv_handle_recording_result,
              .context = session,
          },
  };

  if (pebble_task_get_current() == PebbleTask_App) {
    session->app_focus_sub = (EventServiceInfo){
        .type = PEBBLE_APP_DID_CHANGE_FOCUS_EVENT,
        .handler = prv_app_focus_handler,
        .context = session,
    };
  }
  return session;
#else
  return NULL;
#endif
}

void audio_recording_session_destroy(AudioRecordingSession *session) {
#ifdef CONFIG_MIC
  if (!session) {
    return;
  }

  prv_stop_session(session);
  voice_window_destroy(session->voice_window);
  applib_free(session);
#endif
}

AudioRecordingSessionStatus audio_recording_session_start(AudioRecordingSession *session) {
#ifdef CONFIG_MIC
  if (!session || session->in_progress) {
    return AudioRecordingSessionStatusFailureInternalError;
  }

  const AudioRecordingSessionStatus status =
      (AudioRecordingSessionStatus)voice_window_push(session->voice_window);
  if (status != AudioRecordingSessionStatusSuccess) {
    return status;
  }

  session->in_progress = true;
  event_service_client_subscribe(&session->dictation_result_sub);
  if (pebble_task_get_current() == PebbleTask_App) {
    event_service_client_subscribe(&session->app_focus_sub);
  }
  return status;
#else
  return AudioRecordingSessionStatusFailureInternalError;
#endif
}

AudioRecordingSessionStatus audio_recording_session_stop(AudioRecordingSession *session) {
#ifdef CONFIG_MIC
  if (!session || !session->in_progress) {
    return AudioRecordingSessionStatusFailureInternalError;
  }

  prv_stop_session(session);
  voice_window_pop(session->voice_window);
  return AudioRecordingSessionStatusSuccess;
#else
  return AudioRecordingSessionStatusFailureInternalError;
#endif
}
