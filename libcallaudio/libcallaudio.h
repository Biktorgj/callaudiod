/*
 * Copyright (C) 2020 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "libcallaudio-enums.h"

#include <glib.h>

G_BEGIN_DECLS

/**
 * CallAudioMode:
 * @CALL_AUDIO_MODE_DEFAULT: Default mode (used for music, alarms, ringtones...)
 * @CALL_AUDIO_MODE_CALL: Voice call mode
 * @CALL_AUDIO_MODE_UNKNOWN: Mode unknown
 *
 * Enum values to indicate the mode to be selected.
 */

typedef enum {
  CALL_AUDIO_MODE_DEFAULT = 0,
  CALL_AUDIO_MODE_CALL,
  CALL_AUDIO_MODE_UNKNOWN = 255
} CallAudioMode;

/**
 * CallAudioSpeakerState:
 * @CALL_AUDIO_SPEAKER_OFF: Speaker disabled
 * @CALL_AUDIO_SPEAKER_ON: Speaker enabled
 * @CALL_AUDIO_SPEAKER_UNKNOWN: Unknown
 */

typedef enum {
  CALL_AUDIO_SPEAKER_OFF = 0,
  CALL_AUDIO_SPEAKER_ON = 1,
  CALL_AUDIO_SPEAKER_UNKNOWN = 255
} CallAudioSpeakerState;

/**
 * CallAudioMicState:
 * @CALL_AUDIO_MIC_OFF: Mic disabled
 * @CALL_AUDIO_MIC_ON: Mic enabled
 * @CALL_AUDIO_MIC_UNKNOWN: Unknown
 */

typedef enum {
  CALL_AUDIO_MIC_OFF = 0,
  CALL_AUDIO_MIC_ON,
  CALL_AUDIO_MIC_UNKNOWN = 255
} CallAudioMicState;

typedef enum {
  CALL_AUDIO_BT_UNAVAILABLE = 0,
  CALL_AUDIO_BT_AVAILABLE,
  CALL_AUDIO_BT_ENABLED,
  CALL_AUDIO_BT_UNKNOWN = 255
} CallAudioBluetoothState;

typedef enum {
  CALL_AUDIO_ACTION_FAILED = 0,
  CALL_AUDIO_ACTION_SUCCEEDED,
  CALL_AUDIO_ACTION_MAX = 255
} CallAudioOutputDeviceOperationState;

typedef void (*CallAudioCallback)(gboolean success,
                                  GError *error,
                                  gpointer data);

gboolean call_audio_init     (GError **error);
gboolean call_audio_is_inited(void);
void     call_audio_deinit   (void);

gboolean call_audio_select_mode      (CallAudioMode mode, GError **error);
gboolean call_audio_select_mode_async(CallAudioMode     mode,
                                      CallAudioCallback cb,
                                      gpointer          data);
CallAudioMode call_audio_get_audio_mode(void);

gboolean call_audio_enable_speaker      (gboolean enable, GError **error);
gboolean call_audio_enable_speaker_async(gboolean          enable,
                                         CallAudioCallback cb,
                                         gpointer          data);
CallAudioSpeakerState call_audio_get_speaker_state(void);

gboolean call_audio_mute_mic      (gboolean mute, GError **error);
gboolean call_audio_mute_mic_async(gboolean          enable,
                                   CallAudioCallback cb,
                                   gpointer          data);
CallAudioMicState call_audio_get_mic_state(void);

/* Get available devices */
GVariant *call_audio_get_available_devices(void);

/* Set output device */
gboolean call_audio_output_device      (guint          id,
                                        guint          verb,
                                        gchar         *name,
                                         GError **error);
gboolean call_audio_output_device_async(guint          id,
                                        guint          verb,
                                        gchar         *name,
                                   CallAudioCallback cb,
                                   gpointer          data);
CallAudioOutputDeviceOperationState call_audio_output_device_state(void);

G_END_DECLS
