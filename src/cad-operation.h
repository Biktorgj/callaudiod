/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "callaudio-dbus.h"
#include <glib-object.h>

/**
 * CadOperationType:
 * @CAD_OPERATION_SELECT_MODE: Selecting an audio mode (default mode, voice call mode)
 * @CAD_OPERATION_ENABLE_SPEAKER: Enable or disable the loudspeaker
 * @CAD_OPERATION_MUTE_MIC: Mute or unmute the microphone
 *
 * Enum values to indicate the operation to be performed.
 */
typedef enum {
    CAD_OPERATION_SELECT_MODE = 0,
    CAD_OPERATION_ENABLE_SPEAKER,
    CAD_OPERATION_MUTE_MIC,
    CAD_OPERATION_SWITCH_OUTPUT,
} CadOperationType;

typedef struct _CadOperation CadOperation;

typedef void (*CadOperationCallback)(CadOperation *op);

struct _CadOperation {
    CadOperationType type;
    gpointer value;
    CallAudioDbusCallAudio *object;
    GDBusMethodInvocation *invocation;
    CadOperationCallback callback;
    gboolean success;
};
