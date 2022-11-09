/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "callaudiod-manager"

#include "callaudiod.h"
#include "cad-manager.h"
#include "cad-pulse.h"

#include "libcallaudio.h"
#include "udev.h"
#include <gio/gio.h>
#include <glib-unix.h>

static void cad_manager_call_audio_iface_init(CallAudioDbusCallAudioIface *iface);

G_DEFINE_TYPE_WITH_CODE(CadManager, cad_manager,
                        CALL_AUDIO_DBUS_TYPE_CALL_AUDIO_SKELETON,
                        G_IMPLEMENT_INTERFACE(CALL_AUDIO_DBUS_TYPE_CALL_AUDIO,
                                              cad_manager_call_audio_iface_init));

static void complete_command_cb(CadOperation *op)
{
    if (!op)
        return;

    if (op->success) {
        switch (op->type) {
        case CAD_OPERATION_SELECT_MODE:
            call_audio_dbus_call_audio_complete_select_mode(op->object, op->invocation, op->success);
            break;
        case CAD_OPERATION_ENABLE_SPEAKER:
            call_audio_dbus_call_audio_complete_enable_speaker(op->object, op->invocation, op->success);
            break;
        case CAD_OPERATION_MUTE_MIC:
            call_audio_dbus_call_audio_complete_mute_mic(op->object, op->invocation, op->success);
            break;
        case CAD_OPERATION_SWITCH_OUTPUT:
            call_audio_dbus_call_audio_complete_bt_audio(op->object, op->invocation, op->success);
            break;
        default:
            g_critical("unknown operation %d", op->type);
            break;
        }
    } else {
        g_dbus_method_invocation_return_error(op->invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_FAILED,
                                              "Operation failed");
    }
}

static gboolean cad_manager_handle_select_mode(CallAudioDbusCallAudio *object,
                                               GDBusMethodInvocation *invocation,
                                               guint mode)
{
    CadOperation *op;

    switch ((CallAudioMode)mode) {
    case CALL_AUDIO_MODE_DEFAULT:
    case CALL_AUDIO_MODE_CALL:
        break;
    case CALL_AUDIO_MODE_UNKNOWN:
    default:
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_INVALID_ARGS,
                                              "Invalid mode %u", mode);
        return FALSE;
    }

    op = g_new(CadOperation, 1);
    if (!op) {
        g_critical("Unable to allocate memory for select mode operation");
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_NO_MEMORY,
                                              "Failed to allocate operation");
        return FALSE;
    }

    op->type = CAD_OPERATION_SELECT_MODE;
    op->value = GUINT_TO_POINTER(mode);
    op->object = object;
    op->invocation = invocation;
    op->callback = complete_command_cb;

    g_debug("Select mode: %u", mode);
    cad_pulse_select_mode(mode, op);

    return TRUE;
}

static CallAudioMode
cad_manager_get_audio_mode(CallAudioDbusCallAudio *object)
{
    return cad_pulse_get_audio_mode();
}

static gboolean cad_manager_handle_enable_speaker(CallAudioDbusCallAudio *object,
                                                  GDBusMethodInvocation *invocation,
                                                  gboolean enable)
{
    CadOperation *op;

    op = g_new(CadOperation, 1);
    if (!op) {
        g_critical("Unable to allocate memory for speaker operation");
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_NO_MEMORY,
                                              "Failed to allocate operation");
        return FALSE;
    }

    op->type = CAD_OPERATION_ENABLE_SPEAKER;
    op->value = GUINT_TO_POINTER(enable ? CALL_AUDIO_SPEAKER_ON : CALL_AUDIO_SPEAKER_OFF);
    op->object = object;
    op->invocation = invocation;
    op->callback = complete_command_cb;

    g_debug("Enable speaker: %d", enable);
    cad_pulse_enable_speaker(enable, op);

    return TRUE;
}

static CallAudioSpeakerState
cad_manager_get_speaker_state(CallAudioDbusCallAudio *object)
{
    return cad_pulse_get_speaker_state();
}

static gboolean cad_manager_handle_mute_mic(CallAudioDbusCallAudio *object,
                                            GDBusMethodInvocation *invocation,
                                            gboolean mute)
{
    CadOperation *op;

    op = g_new(CadOperation, 1);
    if (!op) {
        g_critical("Unable to allocate memory for mic operation");
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_NO_MEMORY,
                                              "Failed to allocate operation");
        return FALSE;
    }

    op->type = CAD_OPERATION_MUTE_MIC;
    op->value = GUINT_TO_POINTER(mute ? CALL_AUDIO_MIC_OFF : CALL_AUDIO_MIC_ON);
    op->object = object;
    op->invocation = invocation;
    op->callback = complete_command_cb;

    g_debug("Mute mic: %d", mute);
    cad_pulse_mute_mic(mute, op);

    return TRUE;
}

static CallAudioMicState
cad_manager_get_mic_state(CallAudioDbusCallAudio *object)
{
    return cad_pulse_get_mic_state();
}


static gboolean cad_manager_handle_switch_bt_audio(CallAudioDbusCallAudio *object,
                                                  GDBusMethodInvocation *invocation,
                                                  gboolean enable)
{
    CadOperation *op;

    op = g_new(CadOperation, 1);
    if (!op) {
        g_critical("Unable to allocate memory for speaker operation");
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_NO_MEMORY,
                                              "Failed to allocate operation");
        return FALSE;
    }

    op->type = CAD_OPERATION_SWITCH_OUTPUT;
    op->value = GUINT_TO_POINTER(enable ? CALL_AUDIO_SPEAKER_ON : CALL_AUDIO_SPEAKER_OFF);
    op->object = object;
    op->invocation = invocation;
    op->callback = complete_command_cb;

    g_message("Enable BT audio: %d", enable);
    cad_pulse_enable_bt_audio(enable, op);

    return TRUE;
}

static CallAudioBluetoothState
cad_manager_get_bt_audio_state(CallAudioDbusCallAudio *object)
{
    return cad_pulse_get_bt_audio_state();
}

static void cad_manager_call_audio_iface_init(CallAudioDbusCallAudioIface *iface)
{
    iface->handle_select_mode = cad_manager_handle_select_mode;
    iface->get_audio_mode = cad_manager_get_audio_mode;
    iface->handle_enable_speaker = cad_manager_handle_enable_speaker;
    iface->get_speaker_state = cad_manager_get_speaker_state;
    iface->handle_mute_mic = cad_manager_handle_mute_mic;
    iface->get_mic_state = cad_manager_get_mic_state;
    iface->handle_bt_audio = cad_manager_handle_switch_bt_audio;
    iface->get_bt_audio_state = cad_manager_get_bt_audio_state;
}

static void cad_manager_class_init(CadManagerClass *klass)
{
}

static void cad_manager_init(CadManager *self)
{
}

CadManager *cad_manager_get_default(void)
{
    static CadManager *manager;

    if (manager == NULL) {
        g_debug("initializing manager...");
        manager = g_object_new(CAD_TYPE_MANAGER, NULL);
        g_object_add_weak_pointer(G_OBJECT(manager), (gpointer *)&manager);
        udev_init(manager);
        scan_bt_devices(manager);
    }

    return manager;
}

gboolean scan_bt_devices(CadManager *manager)
{
    guint ret;

    g_message("Bluetooth rescan triggered");

    ret = cad_pulse_find_bt_audio_capabilities();
    g_message("Find audio returned %i", ret);
    /*
     * We need to query here all pulseaudio cards
     * that are using the bluez module. We should
     * do this in cad_pulse
     */
    return G_SOURCE_REMOVE;
}