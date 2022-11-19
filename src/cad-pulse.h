/*
 * Copyright (C) 2018, 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "libcallaudio.h"
#include "cad-operation.h"
#include <stdint.h>

#include <glib-object.h>

G_BEGIN_DECLS

#define CAD_TYPE_PULSE (cad_pulse_get_type())

G_DECLARE_FINAL_TYPE(CadPulse, cad_pulse, CAD, PULSE, GObject);

enum {
    CAD_PULSE_DEVICE_TYPE_INTERNAL = 0,
    CAD_PULSE_DEVICE_TYPE_MODEM,
    CAD_PULSE_DEVICE_TYPE_BT,
    CAD_PULSE_DEVICE_TYPE_USB,
    CAD_PULSE_DEVICE_TYPE_MAX = 255,
};

enum {
    CAD_PULSE_DEVICE_VERB_EARPIECE = 0,
    CAD_PULSE_DEVICE_VERB_HEADSET = 1,
    CAD_PULSE_DEVICE_VERB_SPEAKER = 2,
    CAD_PULSE_DEVICE_VERB_HEADPHONES = 3,

    CAD_PULSE_DEVICE_VERB_MODEM_PASSTHRU = 19,
    CAD_PULSE_DEVICE_VERB_AUTO = 20,

};
CadPulse *cad_pulse_get_default(void);
void cad_pulse_select_mode(CallAudioMode mode, CadOperation *op);
void cad_pulse_enable_speaker(gboolean enable, CadOperation *op);
void cad_pulse_mute_mic(gboolean mute, CadOperation *op);

CallAudioMode cad_pulse_get_audio_mode(void);
CallAudioMicState cad_pulse_get_mic_state(void);
uint32_t cad_pulse_output_device_state(void);
GVariant *cad_pulse_get_available_devices(void);
void cad_pulse_set_output(uint32_t device_id, guint device_verb, guint audio_mode);

void cad_pulse_set_output_device(uint32_t device_id, guint device_verb, guint audio_mode, CadOperation *cad_op);
void cad_pulse_switch_speaker(gboolean enable, CadOperation *cad_op);

G_END_DECLS
