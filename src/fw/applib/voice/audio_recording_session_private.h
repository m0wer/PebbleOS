/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/event_service_client.h"
#include "audio_recording_session.h"
#include "voice_window.h"

struct AudioRecordingSession {
  VoiceWindow *voice_window;
  AudioRecordingSessionStatusCallback callback;
  void *context;
  bool in_progress;
  EventServiceInfo dictation_result_sub;
  EventServiceInfo app_focus_sub;
};
