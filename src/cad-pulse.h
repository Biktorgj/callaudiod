/*
 * Copyright (C) 2018, 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "libcallaudio.h"
#include "cad-operation.h"

#include <glib-object.h>

G_BEGIN_DECLS

#define CAD_TYPE_PULSE (cad_pulse_get_type())

G_DECLARE_FINAL_TYPE(CadPulse, cad_pulse, CAD, PULSE, GObject);

CadPulse *cad_pulse_get_default(void);
void cad_pulse_select_mode(CallAudioMode mode, CadOperation *op);
void cad_pulse_enable_speaker(gboolean enable, CadOperation *op);
void cad_pulse_mute_mic(gboolean mute, CadOperation *op);

CallAudioMode cad_pulse_get_audio_mode(void);
CallAudioSpeakerState cad_pulse_get_speaker_state(void);
CallAudioMicState cad_pulse_get_mic_state(void);
CallAudioBluetoothState cad_pulse_get_bt_audio_state(void);
void cad_pulse_enable_bt_audio(gboolean enable, CadOperation *cad_op);
gboolean cad_pulse_find_bt_audio_capabilities(void);
G_END_DECLS
