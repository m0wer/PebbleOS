/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

//! @file voice/audio_recording_session.h
//! Defines the interface to the raw audio recording session API.
//! @addtogroup Microphone
//! @{
//!   @addtogroup AudioRecordingSession Audio Recording Session
//! A recording session streams Speex-encoded microphone audio to the companion app while the
//! standard voice recording interaction is visible. The companion app delivers the audio only to
//! the initiating app's PebbleKit JS using these events:
//! - `voicerecordingstart`: codec and stream metadata
//! - `voicerecordingdata`: a base64-encoded audio chunk
//! - `voicerecordingend`: terminal success or failure state

typedef struct AudioRecordingSession AudioRecordingSession;

typedef enum {
  //! Recording completed successfully.
  AudioRecordingSessionStatusSuccess,
  //! User canceled the recording interaction.
  AudioRecordingSessionStatusFailureUserCanceled,
  //! User canceled after an error occurred.
  AudioRecordingSessionStatusFailureUserCanceledWithError,
  //! Too many errors occurred and the interaction exited.
  AudioRecordingSessionStatusFailureSystemAborted,
  //! No speech was detected and the interaction exited.
  AudioRecordingSessionStatusFailureNoSpeechDetected,
  //! No Bluetooth connection was available.
  AudioRecordingSessionStatusFailureConnectivityError,
  //! Voice recording is disabled for this user.
  AudioRecordingSessionStatusFailureDisabled,
  //! Voice recording failed due to an internal error.
  AudioRecordingSessionStatusFailureInternalError,
  //! The companion could not process the recording.
  AudioRecordingSessionStatusFailureRecognizerError,
} AudioRecordingSessionStatus;

//! Called when a recording session completes or fails.
typedef void (*AudioRecordingSessionStatusCallback)(AudioRecordingSession *session,
                                                    AudioRecordingSessionStatus status,
                                                    void *context);

//! Create a raw audio recording session.
//! @param callback          recording session status handler (must be valid)
//! @param callback_context  context pointer for status handler
//! @return handle to the recording session, or NULL if recording is unavailable or an error occurs.
AudioRecordingSession *audio_recording_session_create(AudioRecordingSessionStatusCallback callback,
                                                      void *callback_context);

//! Destroy a recording session. A session in progress is terminated.
void audio_recording_session_destroy(AudioRecordingSession *session);

//! Start the recording session and display the voice recording interaction.
//! @return recording session status
AudioRecordingSessionStatus audio_recording_session_start(AudioRecordingSession *session);

//! Stop the current recording session. No status callback is received after stopping.
//! @return recording session status
AudioRecordingSessionStatus audio_recording_session_stop(AudioRecordingSession *session);

//!   @} // end addtogroup AudioRecordingSession
//! @} // end addtogroup Microphone
