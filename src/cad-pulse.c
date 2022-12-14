/*
 * Copyright (C) 2018, 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pulse/def.h>
#define G_LOG_DOMAIN "callaudiod-pulse"

#include "cad-manager.h"
#include "cad-pulse.h"

#include <glib/gi18n.h>
#include <glib-object.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <alsa/use-case.h>

#include <string.h>
#include <stdio.h>

#define APPLICATION_NAME "CallAudio"
#define APPLICATION_ID   "org.mobian-project.CallAudio"

#define SINK_CLASS "sound"
#define CARD_BUS_PATH_PREFIX "platform-"
#define CARD_FORM_FACTOR "internal"
#define CARD_MODEM_CLASS "modem"
#define CARD_MODEM_NAME "Modem"
#define PA_BT_DRIVER "module-bluez5-device.c"
#define PA_BT_PREFERRED_PROFILE "handsfree_head_unit"
#define PA_BT_PREFERRED_PORT "Bluetooth"
#define PA_MAIN_CARD_BT_PROFILE "Voice Call BT"

struct _CadPulse
{
    GObject parent_instance;

    GObject *manager;

    pa_glib_mainloop  *loop;
    pa_context        *ctx;

    int card_id;
    int sink_id;
    int source_id;
    
    int bluetooth_source_port_id;
    int bluetooth_sink_port_id;
    int external_card_id;
    int external_sink_id;
    int external_source_id;
    int internal_sink_id;
    int internal_source_id;

    int external_established_loopback;
    gchar *external_card_name;
    gboolean external_card_connected;

    gboolean has_voice_profile;
    gchar *speaker_port;
    gchar *earpiece_port;

    GHashTable *sink_ports;
    GHashTable *source_ports;

    CallAudioMode audio_mode;
    CallAudioSpeakerState speaker_state;
    CallAudioMicState mic_state;
    CallAudioBluetoothState bt_audio;
};

G_DEFINE_TYPE(CadPulse, cad_pulse, G_TYPE_OBJECT);

typedef struct _CadPulseOperation {
    CadPulse *pulse;
    CadOperation *op;
    guint value;
} CadPulseOperation;

static void pulseaudio_cleanup(CadPulse *self);
static gboolean pulseaudio_connect(CadPulse *self);
static gboolean init_pulseaudio_objects(CadPulse *self);

/******************************************************************************
 * Source management
 *
 * The following functions take care of monitoring and configuring the default
 * source (input)
 ******************************************************************************/

static const gchar *get_available_source_port(const pa_source_info *source, const gchar *exclude)
{
    pa_source_port_info *available_port = NULL;
    guint i;

    g_debug("looking for available input excluding '%s'", exclude);

    for (i = 0; i < source->n_ports; i++) {
        pa_source_port_info *port = source->ports[i];

        if ((exclude && strcmp(port->name, exclude) == 0) ||
            port->available == PA_PORT_AVAILABLE_NO) {
            continue;
        }

        if (!available_port || port->priority > available_port->priority)
            available_port = port;
    }

    if (available_port) {
        g_debug("found available input '%s'", available_port->name);
        return available_port->name;
    }

    g_warning("no available input found!");

    return NULL;
}

static void change_source_info(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;
    gboolean change = FALSE;
    guint i;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    if (info->index != self->source_id)
        return;

    for (i = 0; i < info->n_ports; i++) {
        pa_source_port_info *port = info->ports[i];

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            enum pa_port_available available;
            available = GPOINTER_TO_INT(g_hash_table_lookup(self->source_ports, port->name));
            if (available != port->available) {
                g_hash_table_insert(self->source_ports, g_strdup(port->name),
                                    GINT_TO_POINTER(port->available));
                change = TRUE;
            }
        }
    }

    if (change) {
        target_port = get_available_source_port(info, NULL);
        if (target_port) {
            op = pa_context_set_source_port_by_index(ctx, self->source_id,
                                                   target_port, NULL, NULL);
            if (op)
                pa_operation_unref(op);
        }
    }
}

static void process_new_source(CadPulse *self, const pa_source_info *info)
{
    const gchar *prop;
    int i;

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, SINK_CLASS) != 0)
        return;
    if (info->monitor_of_sink != PA_INVALID_INDEX)
        return;
    if (info->card != self->card_id || self->source_id != -1)
        return;

    self->source_id = info->index;
    if (self->source_ports)
        g_hash_table_destroy(self->source_ports);
    self->source_ports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < info->n_ports; i++) {
        pa_source_port_info *port = info->ports[i];

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            g_hash_table_insert (self->source_ports,
                                 g_strdup(port->name),
                                 GINT_TO_POINTER(port->available));
        }
    }

    g_debug("SOURCE: idx=%u name='%s'", info->index, info->name);
}

static void init_source_info(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    process_new_source(self, info);
    if (self->source_id < 0 || self->source_id != info->index)
        return;

    op = pa_context_set_default_source(ctx, info->name, NULL, NULL);
    if (op)
        pa_operation_unref(op);

    if (self->mic_state == CALL_AUDIO_MIC_UNKNOWN) {
        if (info->mute)
            self->mic_state = CALL_AUDIO_MIC_OFF;
        else
            self->mic_state = CALL_AUDIO_MIC_ON;
        g_object_set(self->manager, "mic-state", self->mic_state, NULL);
    }

    target_port = get_available_source_port(info, NULL);
    if (target_port) {
        op = pa_context_set_source_port_by_index(ctx, self->source_id,
                                                 target_port, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }

    self->bt_audio = 0;
    g_object_set(self->manager, "bt-audio-state", self->bt_audio, NULL);

}

/******************************************************************************
 * Sink management
 *
 * The following functions take care of monitoring and configuring the default
 * sink (output)
 ******************************************************************************/

static const gchar *get_available_sink_port(const pa_sink_info *sink, const gchar *exclude)
{
    pa_sink_port_info *available_port = NULL;
    guint i;

    g_debug("looking for available output excluding '%s'", exclude);

    for (i = 0; i < sink->n_ports; i++) {
        pa_sink_port_info *port = sink->ports[i];

        if ((exclude && strcmp(port->name, exclude) == 0) ||
            port->available == PA_PORT_AVAILABLE_NO) {
            continue;
        }

        if (!available_port || port->priority > available_port->priority)
            available_port = port;
    }

    if (available_port) {
        g_debug("found available output '%s'", available_port->name);
        return available_port->name;
    }

    g_warning("no available output found!");

    return NULL;
}

static void change_sink_info(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;
    gboolean change = FALSE;
    guint i;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }

    if (info->index != self->sink_id)
        return;

    for (i = 0; i < info->n_ports; i++) {
        pa_sink_port_info *port = info->ports[i];

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            enum pa_port_available available;
            available = GPOINTER_TO_INT(g_hash_table_lookup(self->sink_ports, port->name));
            if (available != port->available) {
                g_hash_table_insert(self->sink_ports, g_strdup(port->name),
                                    GINT_TO_POINTER(port->available));
                change = TRUE;
            }
        }
    }

    if (change) {
        target_port = get_available_sink_port(info, NULL);
        if (target_port) {
            op = pa_context_set_sink_port_by_index(ctx, self->sink_id,
                                                   target_port, NULL, NULL);
            if (op)
                pa_operation_unref(op);
        }
    }
}

static void process_new_sink(CadPulse *self, const pa_sink_info *info)
{
    const gchar *prop;
    guint i;

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, SINK_CLASS) != 0)
        return;
    if (info->card != self->card_id || self->sink_id != -1)
        return;
    self->sink_id = info->index;
    g_message("Sink ID: %i", self->sink_id);
    if (self->sink_ports)
        g_hash_table_destroy(self->sink_ports);
    self->sink_ports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_debug("SINK: idx=%u name='%s'", info->index, info->name);

    for (i = 0; i < info->n_ports; i++) {
        pa_sink_port_info *port = info->ports[i];

        if (strstr(port->name, SND_USE_CASE_DEV_SPEAKER) != NULL) {
            if (self->speaker_port) {
                if (strcmp(port->name, self->speaker_port) != 0) {
                    g_free(self->speaker_port);
                    self->speaker_port = g_strdup(port->name);
                }
            } else {
                self->speaker_port = g_strdup(port->name);
            }
        } else if (strstr(port->name, SND_USE_CASE_DEV_EARPIECE) != NULL) {
            if (self->earpiece_port) {
                if (strcmp(port->name, self->earpiece_port) != 0) {
                    g_free(self->earpiece_port);
                    self->earpiece_port = g_strdup(port->name);
                }
            } else {
                self->earpiece_port = g_strdup(port->name);
            }
        }

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            g_hash_table_insert (self->sink_ports,
                                 g_strdup(port->name),
                                 GINT_TO_POINTER(port->available));
        }
    }

    g_debug("SINK:   speaker_port='%s'", self->speaker_port);
    g_debug("SINK:   earpiece_port='%s'", self->earpiece_port);
}

static void init_sink_info(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }

    process_new_sink(self, info);
    if (self->sink_id < 0 || self->sink_id != info->index)
        return;

    op = pa_context_set_default_sink(ctx, info->name, NULL, NULL);
    if (op)
        pa_operation_unref(op);

    if (self->speaker_state == CALL_AUDIO_SPEAKER_UNKNOWN) {
        self->speaker_state = CALL_AUDIO_SPEAKER_OFF;

        switch (self->audio_mode) {
        case CALL_AUDIO_MODE_CALL:
            if (g_strcmp0(info->active_port->name, self->speaker_port) == 0) {
                self->speaker_state = CALL_AUDIO_SPEAKER_ON;
                g_object_set(self->manager, "speaker-state", self->speaker_state, NULL);
                /*
                 * callaudiod likely restarted after being killed during a call
                 * during which the speaker was enabled. End processing here so
                 * we keep the current routing and don't unexpectedly mess with
                 * the call setup.
                 */
                return;
            }
            break;
        case CALL_AUDIO_MODE_UNKNOWN:
            /*
             * Let's see if we can guess the current mode:
             * - if current port is earpiece, we're likely in call mode
             * - otherwise we're either in default mode, or call mode with
             *   speaker enabled. Let's settle for the former as both situations
             *   are technically equivalent.
             *
             * Note: this code path is only used when the card doesn't have a
             * voice profile, otherwise things are easier to deal with.
             */
            if (g_strcmp0(info->active_port->name, self->earpiece_port) == 0) {
                self->audio_mode = CALL_AUDIO_MODE_CALL;
                g_object_set(self->manager, "audio-mode", self->audio_mode, NULL);
                /*
                 * Don't touch routing as we're likely in the middle of a call,
                 * see above.
                 */
                return;
            } else {
                self->audio_mode = CALL_AUDIO_MODE_DEFAULT;
                g_object_set(self->manager, "audio-mode", self->audio_mode, NULL);
            }
            break;
        case CALL_AUDIO_MODE_DEFAULT:
        default:
            break;
        }

        g_object_set(self->manager, "speaker-state", self->speaker_state, NULL);
    }

    target_port = get_available_sink_port(info, NULL);
    if (target_port) {
        g_debug("  Using sink port '%s'", target_port);
        op = pa_context_set_sink_port_by_index(ctx, self->sink_id,
                                               target_port, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
}

/******************************************************************************
 * Card management
 *
 * The following functions take care of gathering information about the default
 * sound card
 ******************************************************************************/

static void init_card_info(pa_context *ctx, const pa_card_info *info, int eol, void *data)
{
    CadPulse *self = data;
    pa_operation *op;
    const gchar *prop;
    gboolean has_speaker = FALSE;
    gboolean has_earpiece = FALSE;
    guint i;

    if (eol != 0) {
        if (self->card_id < 0) {
            g_critical("No suitable card found, retrying in 3s...");
            g_timeout_add_seconds(3, G_SOURCE_FUNC(init_pulseaudio_objects), self);
        }
        return;
    }

    if (!info) {
        g_critical("PA returned no card info (eol=%d)", eol);
        return;
    }

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_BUS_PATH);
    if (prop && !g_str_has_prefix(prop, CARD_BUS_PATH_PREFIX))
        return;
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_FORM_FACTOR);
    if (prop && strcmp(prop, CARD_FORM_FACTOR) != 0)
        return;
    prop = pa_proplist_gets(info->proplist, "alsa.card_name");
    if (prop && strcmp(prop, CARD_MODEM_NAME) == 0)
        return;
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, CARD_MODEM_CLASS) == 0)
        return;

    for (i = 0; i < info->n_ports; i++) {
        pa_card_port_info *port = info->ports[i];

        if (strstr(port->name, SND_USE_CASE_DEV_SPEAKER) != NULL) {
            has_speaker = TRUE;
        } else if (strstr(port->name, SND_USE_CASE_DEV_EARPIECE) != NULL ||
                   strstr(port->name, SND_USE_CASE_DEV_HANDSET)  != NULL) {
            has_earpiece = TRUE;
        }
    }
    if (!has_speaker || !has_earpiece) {
        g_message("Card '%s' lacks speaker and/or earpiece port, skipping...",
                  info->name);
        return;
    }

    self->card_id = info->index;

    g_debug("CARD: idx=%u name='%s'", info->index, info->name);

    for (i = 0; i < info->n_profiles; i++) {
        pa_card_profile_info2 *profile = info->profiles2[i];

        if (strstr(profile->name, SND_USE_CASE_VERB_VOICECALL) != NULL) {
            self->has_voice_profile = TRUE;
            if (info->active_profile2 == profile)
                self->audio_mode = CALL_AUDIO_MODE_CALL;
            else
                self->audio_mode = CALL_AUDIO_MODE_DEFAULT;
            break;
        }
    }

    // We were able determine the current mode, set the corresponding D-Bus property
    if (self->audio_mode != CALL_AUDIO_MODE_UNKNOWN)
        g_object_set(self->manager, "audio-mode", self->audio_mode, NULL);

    g_debug("CARD:   %s voice profile", self->has_voice_profile ? "has" : "doesn't have");

    /* Found a suitable card, let's prepare the sink/source */
    op = pa_context_get_sink_info_list(self->ctx, init_sink_info, self);
    if (op)
        pa_operation_unref(op);
    op = pa_context_get_source_info_list(self->ctx, init_source_info, self);
    if (op)
        pa_operation_unref(op);
}

/******************************************************************************
 * PulseAudio management
 *
 * The following functions configure the PulseAudio connection and monitor the
 * state of PulseAudio objects
 ******************************************************************************/

static void init_module_info(pa_context *ctx, const pa_module_info *info, int eol, void *data)
{
    pa_operation *op;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no module info (eol=%d)", eol);
        return;
    }

    g_debug("MODULE: idx=%u name='%s'", info->index, info->name);

    if (strcmp(info->name, "module-switch-on-port-available") == 0) {
        g_debug("MODULE: unloading '%s'", info->name);
        op = pa_context_unload_module(ctx, info->index, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
}

static gboolean init_pulseaudio_objects(CadPulse *self)
{
    pa_operation *op;

    self->card_id = self->sink_id = self->source_id = -1;
    self->external_card_id = self->external_sink_id = self->external_source_id = -1;
    self->sink_ports = self->source_ports = NULL;

    op = pa_context_get_card_info_list(self->ctx, init_card_info, self);
    if (op)
        pa_operation_unref(op);
    op = pa_context_get_module_info_list(self->ctx, init_module_info, self);
    if (op)
        pa_operation_unref(op);

    return G_SOURCE_REMOVE;
}

static void changed_cb(pa_context *ctx, pa_subscription_event_type_t type, uint32_t idx, void *data)
{
    CadPulse *self = data;
    pa_subscription_event_type_t kind = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    pa_operation *op = NULL;

    switch (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK:
        if (idx == self->sink_id && kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            g_debug("sink %u removed", idx);
            self->sink_id = -1;
            g_hash_table_destroy(self->sink_ports);
            self->sink_ports = NULL;
        } else if (kind == PA_SUBSCRIPTION_EVENT_NEW) {
            g_debug("new sink %u", idx);
            op = pa_context_get_sink_info_by_index(ctx, idx, init_sink_info, self);
            if (op)
                pa_operation_unref(op);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        if (idx == self->source_id && kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            g_debug("source %u removed", idx);
            self->source_id = -1;
            g_hash_table_destroy(self->source_ports);
            self->source_ports = NULL;
        } else if (kind == PA_SUBSCRIPTION_EVENT_NEW) {
            g_debug("new source %u", idx);
            op = pa_context_get_source_info_by_index(ctx, idx, init_source_info, self);
            if (op)
                pa_operation_unref(op);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_CARD:
        if (idx == self->card_id && kind == PA_SUBSCRIPTION_EVENT_CHANGE) {
            g_debug("card %u changed", idx);
            if (self->sink_id != -1) {
                op = pa_context_get_sink_info_by_index(ctx, self->sink_id,
                                                       change_sink_info, self);
                if (op)
                    pa_operation_unref(op);
            }
            if (self->source_id != -1) {
                op = pa_context_get_source_info_by_index(ctx, self->source_id,
                                                         change_source_info, self);
                if (op)
                    pa_operation_unref(op);
            }
        }
        break;
    default:
        break;
    }
}

static void pulse_state_cb(pa_context *ctx, void *data)
{
    CadPulse *self = data;
    pa_context_state_t state;

    state = pa_context_get_state(ctx);
    switch (state) {
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        g_debug("PA not ready");
        break;
    case PA_CONTEXT_FAILED:
        g_critical("Error in PulseAudio context: %s", pa_strerror(pa_context_errno(ctx)));
        pulseaudio_cleanup(self);
        g_idle_add(G_SOURCE_FUNC(pulseaudio_connect), self);
        break;
    case PA_CONTEXT_TERMINATED:
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(ctx, changed_cb, self);
        pa_context_subscribe(ctx,
                             PA_SUBSCRIPTION_MASK_SINK  | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_CARD,
                             NULL, self);
        g_debug("PA is ready, initializing cards list");
        init_pulseaudio_objects(self);
        break;
    default:
        g_return_if_reached();
    }
}

static void pulseaudio_cleanup(CadPulse *self)
{
    if (self->ctx) {
        pa_context_disconnect(self->ctx);
        pa_context_unref(self->ctx);
        self->ctx = NULL;
    }
}

static gboolean pulseaudio_connect(CadPulse *self)
{
    pa_proplist *props;
    int err;

    /* Meta data */
    props = pa_proplist_new();
    g_assert(props != NULL);

    err = pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, APPLICATION_NAME);
    err = pa_proplist_sets(props, PA_PROP_APPLICATION_ID, APPLICATION_ID);

    if (!self->loop)
        self->loop = pa_glib_mainloop_new(NULL);
    if (!self->loop)
        g_error ("Error creating PulseAudio main loop");

    if (!self->ctx)
        self->ctx = pa_context_new(pa_glib_mainloop_get_api(self->loop), APPLICATION_NAME);
    if (!self->ctx)
        g_error ("Error creating PulseAudio context");

    pa_context_set_state_callback(self->ctx, (pa_context_notify_cb_t)pulse_state_cb, self);
    err = pa_context_connect(self->ctx, NULL, PA_CONTEXT_NOFAIL, 0);
    if (err < 0)
        g_error ("Error connecting to PulseAudio context: %s", pa_strerror(err));

    return G_SOURCE_REMOVE;
}

/******************************************************************************
 * GObject base functions
 ******************************************************************************/

static void constructed(GObject *object)
{
    GObjectClass *parent_class = g_type_class_peek(G_TYPE_OBJECT);
    CadPulse *self = CAD_PULSE(object);

    pulseaudio_connect(self);

    parent_class->constructed(object);
}


static void dispose(GObject *object)
{
    GObjectClass *parent_class = g_type_class_peek(G_TYPE_OBJECT);
    CadPulse *self = CAD_PULSE(object);

    if (self->speaker_port)
        g_free(self->speaker_port);
    if (self->earpiece_port)
        g_free(self->earpiece_port);

    pulseaudio_cleanup(self);

    if (self->loop) {
        pa_glib_mainloop_free(self->loop);
        self->loop = NULL;
    }

    parent_class->dispose(object);
}

static void cad_pulse_class_init(CadPulseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->constructed = constructed;
    object_class->dispose = dispose;
}

static void cad_pulse_init(CadPulse *self)
{
    self->manager = G_OBJECT(cad_manager_get_default());
    self->audio_mode = CALL_AUDIO_MODE_UNKNOWN;
    self->speaker_state = CALL_AUDIO_SPEAKER_UNKNOWN;
    self->mic_state = CALL_AUDIO_MIC_UNKNOWN;
}

CadPulse *cad_pulse_get_default(void)
{
    static CadPulse *pulse = NULL;

    if (pulse == NULL) {
        g_debug("initializing pulseaudio backend...");
        pulse = g_object_new(CAD_TYPE_PULSE, NULL);
        g_object_add_weak_pointer(G_OBJECT(pulse), (gpointer *)&pulse);
        cad_pulse_find_bt_audio_capabilities();
    }

    return pulse;
}

/******************************************************************************
 * Commands management
 *
 * The following functions handle external requests to switch mode, output port
 * or microphone status
 ******************************************************************************/

static void operation_complete_cb(pa_context *ctx, int success, void *data)
{
    CadPulseOperation *operation = data;

    g_debug("operation returned %d", success);

    if (operation) {
        if (operation->op) {
            operation->op->success = (gboolean)!!success;
            if (operation->op->callback)
                operation->op->callback(operation->op);

            if (operation->op->success) {
                guint new_value = GPOINTER_TO_UINT(operation->value);

                switch (operation->op->type) {
                case CAD_OPERATION_SELECT_MODE:
                    if (operation->pulse->audio_mode != new_value) {
                        operation->pulse->audio_mode = new_value;
                        g_object_set(operation->pulse->manager, "audio-mode", new_value, NULL);
                    }
                    break;
                case CAD_OPERATION_ENABLE_SPEAKER:
                    if (operation->pulse->speaker_state != new_value) {
                        operation->pulse->speaker_state = new_value;
                        g_object_set(operation->pulse->manager, "speaker-state", new_value, NULL);
                    }
                    break;
                case CAD_OPERATION_MUTE_MIC:
                    /*
                     * "Mute mic" operation's value is TRUE (1) for muting the mic,
                     * so ensure mic_state carries the right value.
                     */
                    new_value = new_value ? CALL_AUDIO_MIC_OFF : CALL_AUDIO_MIC_ON;
                    if (operation->pulse->mic_state != new_value) {
                        operation->pulse->mic_state = new_value;
                        g_object_set(operation->pulse->manager, "mic-state", new_value, NULL);
                    }
                    break;
                case CAD_OPERATION_SWITCH_BT_AUDIO:
                    /*
                     * "Switch to bluetooth" depends on a couple of things:
                     *  1. udev has previously reported that a bluetooth device
                     * has been added.
                     *  2. The device has been queried from pulseaudio and it's an
                     * audio device that can serve both as a sink and a source
                     *
                     */
                    if (operation->pulse->bt_audio > 0 &&
                        operation->pulse->bt_audio != new_value) {
                        operation->pulse->bt_audio = new_value;
                        g_object_set(operation->pulse->manager, "bt-audio-state", new_value, NULL);
                    }
                default:
                    break;
                }
            }

            free(operation->op);
        }

        free(operation);
    }
}

static void set_card_profile(pa_context *ctx, const pa_card_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_card_profile_info2 *profile;
    pa_operation *op = NULL;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no card info (eol=%d)", eol);
        return;
    }

    if (info->index != operation->pulse->card_id)
        return;

    profile = info->active_profile2;

    if (strcmp(profile->name, SND_USE_CASE_VERB_VOICECALL) == 0 && operation->value == 0) {
        g_debug("switching to default profile");
        op = pa_context_set_card_profile_by_index(ctx, operation->pulse->card_id,
                                                  SND_USE_CASE_VERB_HIFI,
                                                  operation_complete_cb, operation);
    } else if (strcmp(profile->name, SND_USE_CASE_VERB_HIFI) == 0 && operation->value == 1) {
        g_debug("switching to voice profile");
        op = pa_context_set_card_profile_by_index(ctx, operation->pulse->card_id,
                                                  SND_USE_CASE_VERB_VOICECALL,
                                                  operation_complete_cb, operation);
        if (operation->pulse->external_card_id != -1) {
            if (op) {
                pa_operation_unref(op);
            }
            op = pa_context_set_card_profile_by_index(ctx, operation->pulse->external_card_id,
                                                  PA_BT_PREFERRED_PROFILE,
                                                  NULL, NULL);
        }
    }
    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(ctx, 1, operation);
    }
}

static void set_output_port(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_operation *op = NULL;
    const gchar *target_port;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }

    if (info->card != operation->pulse->card_id || info->index != operation->pulse->sink_id)
        return;

    if (operation->op && operation->op->type == CAD_OPERATION_SELECT_MODE) {
        /*
         * When switching to voice call mode, we want to switch to any port
         * other than the speaker; this makes sure we use the headphones if they
         * are connected, and the earpiece otherwise.
         * When switching back to normal mode, the highest priority port is to
         * be selected anyway.
         */
        if (operation->value == CALL_AUDIO_MODE_CALL)
            target_port = get_available_sink_port(info, operation->pulse->speaker_port);
        else
            target_port = get_available_sink_port(info, NULL);
    } else {
        /*
         * When forcing speaker output, we simply select the speaker port.
         * When disabling speaker output, we want the highest priority port
         * other than the speaker, so that we use the headphones if connected,
         * and the earpiece otherwise.
         */
        if (operation->value)
            target_port = operation->pulse->speaker_port;
        else
            target_port = get_available_sink_port(info, operation->pulse->speaker_port);
    }

    g_debug("active port is '%s', target port is '%s'", info->active_port->name, target_port);

    if (strcmp(info->active_port->name, target_port) != 0) {
        g_debug("switching to target port '%s'", target_port);
        op = pa_context_set_sink_port_by_index(ctx, operation->pulse->sink_id,
                                               target_port,
                                               operation_complete_cb, operation);
    }

    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(ctx, 1, operation);
    }
}

/**
 * cad_pulse_select_mode:
 * @mode:
 * @cad_op:
 *
 */
void cad_pulse_select_mode(CallAudioMode mode, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_SELECT_MODE);
    operation->pulse = cad_pulse_get_default();
    operation->op = cad_op;
    operation->value = mode;
    cad_pulse_find_bt_audio_capabilities();

    if (mode != CALL_AUDIO_MODE_CALL) {
        /*
         * When ending a call, we want to make sure the mic doesn't stay muted
         */
        CadOperation *unmute_op = g_new0(CadOperation, 1);
        unmute_op->type = CAD_OPERATION_MUTE_MIC;

        cad_pulse_mute_mic(FALSE, unmute_op);

        /*
         * If the card has a dedicated voice profile, disable speaker so it
         * doesn't get automatically enabled for next call.
         */
        if (operation->pulse->has_voice_profile) {
            CadOperation *disable_speaker_op = g_new0(CadOperation, 1);
            disable_speaker_op->type = CAD_OPERATION_ENABLE_SPEAKER;

            cad_pulse_enable_speaker(FALSE, disable_speaker_op);
        }
    }

    if (operation->pulse->has_voice_profile) {
      /*
       * The pinephone f.e. has a voice profile
       */
        g_debug("card has voice profile, using it");
        op = pa_context_get_card_info_by_index(operation->pulse->ctx,
                                               operation->pulse->card_id,
                                               set_card_profile, operation);
    } else {
        if (operation->pulse->sink_id < 0) {
            g_warning("card has no voice profile and no usable sink");
            goto error;
        }
        g_debug("card doesn't have voice profile, switching output port");

        op = pa_context_get_sink_info_by_index(operation->pulse->ctx,
                                               operation->pulse->sink_id,
                                               set_output_port, operation);
    }

    if (op)
        pa_operation_unref(op);

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        if (cad_op->callback)
            cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

void cad_pulse_enable_speaker(gboolean enable, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_ENABLE_SPEAKER);

    operation->pulse = cad_pulse_get_default();

    if (operation->pulse->sink_id < 0) {
        g_warning("card has no usable sink");
        goto error;
    }

    operation->op = cad_op;
    operation->value = (guint)enable;

    op = pa_context_get_sink_info_by_index(operation->pulse->ctx,
                                           operation->pulse->sink_id,
                                           set_output_port, operation);
    if (op)
        pa_operation_unref(op);

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        if (cad_op->callback)
            cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

void cad_pulse_mute_mic(gboolean mute, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_MUTE_MIC);

    operation->pulse = cad_pulse_get_default();

    if (operation->pulse->source_id < 0) {
        g_warning("card has no usable source");
        goto error;
    }

    operation->op = cad_op;
    operation->value = (guint)mute;

    if (operation->pulse->mic_state == CALL_AUDIO_MIC_OFF && !operation->value) {
        g_debug("mic is muted, unmuting...");
        op = pa_context_set_source_mute_by_index(operation->pulse->ctx,
                                                 operation->pulse->source_id, 0,
                                                 operation_complete_cb, operation);
    } else if (operation->pulse->mic_state == CALL_AUDIO_MIC_ON && operation->value) {
        g_debug("mic is active, muting...");
        op = pa_context_set_source_mute_by_index(operation->pulse->ctx,
                                                 operation->pulse->source_id, 1,
                                                 operation_complete_cb, operation);
    }

    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(operation->pulse->ctx, 1, operation);
    }

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        if (cad_op->callback)
            cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

CallAudioMode cad_pulse_get_audio_mode(void)
{
    CadPulse *self = cad_pulse_get_default();
    return self->audio_mode;
}

CallAudioSpeakerState cad_pulse_get_speaker_state(void)
{
    CadPulse *self = cad_pulse_get_default();
    return self->speaker_state;
}

CallAudioMicState cad_pulse_get_mic_state(void)
{
    CadPulse *self = cad_pulse_get_default();
    return self->mic_state;
}

CallAudioBluetoothState cad_pulse_get_bt_audio_state(void) 
{
    CadPulse *self = cad_pulse_get_default();
    return self->bt_audio;
}

static void unload_loopback_callback(pa_context *ctx, const pa_module_info *info, int eol, void *data)
{
    pa_operation *op = NULL;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }
    // This is horrible, but as stated... YOLO!!

    if (strcmp(info->name, "module-loopback") == 0) {
        g_message("Unloading '%s'", info->name);
        op = pa_context_unload_module(ctx, info->index, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
    return;
}
static void get_source_id_callback(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulse *self = data;
    g_message("%s: eol %i", __func__, eol);
    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }
    g_message("Card #%i. Source ID: %i: %s", info->card, info->index, info->name);

    if (info->monitor_of_sink != PA_INVALID_INDEX) {
        g_message("Source is a monitor of another sink. We can't use this");
        return;
    }

    if (info->card == self->external_card_id) {
        g_message("Source belongs to our bluetooth device, *SAVE IT*");
        self->external_source_id = info->index;
    } else if (info->card == self->card_id) {
        g_message("Source is from the internal card!");
        self->internal_source_id = info->index;
    }
    return;
}

static void get_sink_id_callback(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulse *self = data;
    g_message("%s: eol %i", __func__, eol);

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }
    g_message("Card #%i. Sink ID: %i: %s", info->card, info->index, info->name);

    if (info->card == self->external_card_id) {
        g_message("Sink belongs to our bluetooth device");
        self->external_sink_id = info->index;
    } else if (info->card == self->card_id) {
        g_message("Sink is from the internal card!");
        self->internal_sink_id = info->index;
    }
    return;
}

static void get_card_info_callback(pa_context *c, const pa_card_info *info, int is_last, void *data) {
    CadPulse *self = data;
    pa_operation *op;
    int i;
    if (is_last < 0) {
        g_message("Failed to get card information: %s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (is_last) {
        g_message("Is last card!");
        return;
    }
    g_message("%u: %s using %s\n", info->index, info->name, info->driver);
    /* Check the driver used by PulseAudio, and try to match the available
       profiles. We could modify this to allow for USB-C headsets, but 
       I don't have any to try */
    if (strcmp(info->driver, PA_BT_DRIVER) == 0) {
        g_message("We got a bluetooth audio device!");
        self->external_card_id = info->index;
        self->external_card_name =  g_strdup(info->name);
        // Find the sink and source ports
        op = pa_context_get_sink_info_list(c, get_sink_id_callback, self);
        if (op)
            pa_operation_unref(op);
            
        op = pa_context_get_source_info_list(c, get_source_id_callback, self);
        if (op)
        pa_operation_unref(op);

        for (i = 0; i < info->n_profiles; i++) {
            pa_card_profile_info2 *profile = info->profiles2[i];
            if (strstr(profile->name, PA_BT_PREFERRED_PROFILE) != NULL) {
            g_message("%s has a headset profile, making it available", info->name);
            self->bt_audio = 1; // AVAILABLE
            self->external_card_connected = TRUE;
            }
        }
    }

}

/* Pieces shamelessly stolen from wys */
void cad_pulse_enable_bt_audio(gboolean enable, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;
    gchar *loopback_bt_source_arg, *loopback_int_source_arg;
    g_message("***** %s ******", __func__);
    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_SWITCH_BT_AUDIO);

    operation->pulse = cad_pulse_get_default();

/*    if (operation->pulse->sink_id < 0) {
        g_warning("Audio isn't even ready yet");
        goto error;
    }*/
    if (enable && operation->pulse->external_card_id < 0) {
        g_warning("No bluetooth adapter connected");
        goto error;
    }

    if (enable && (operation->pulse->external_sink_id < 0 ||
         operation->pulse->external_source_id < 0)) {
            g_warning("Bluetooth adapter has no sink or source");
            goto error;
    }
    operation->op = cad_op;
    operation->value = (guint)enable;
    g_message("Requested Bluetooth switch, enable %u", enable);
    /* TODO: Check if we're actually in call */

    /* If card is correct and everything is ready,
        1. Switch to handsfree in the bluetooth device (external_card_id)
        2. Switch to Bluetooth device in the main card
        3. Establish the loopback config
    */

    /* YOLO */
    if (enable) {
    // Switch to headset profile in bluetooth
    g_message("Switching to headset mode in bluetooth");
    op = pa_context_set_card_profile_by_index(operation->pulse->ctx, 
                                            operation->pulse->external_card_id,
                                            PA_BT_PREFERRED_PROFILE, NULL, NULL);
    if (op)
        pa_operation_unref(op);

    // Switch the main card to our special profile
    op = pa_context_set_card_profile_by_index(operation->pulse->ctx, operation->pulse->card_id,
                                            PA_MAIN_CARD_BT_PROFILE, NULL, NULL);
    if (op)
        pa_operation_unref(op);

    // Now recheck sink and source ids, since they'll have canged when switching the profiles
    op = pa_context_get_card_info_list(operation->pulse->ctx, get_card_info_callback, operation->pulse);
    if (op)
        pa_operation_unref(op);

    /* Need to do it twice */
    // It's peanut-butter-loopback time
    loopback_bt_source_arg = g_strdup_printf("source=%i sink=%i", operation->pulse->external_source_id, operation->pulse->internal_sink_id); 
    loopback_int_source_arg = g_strdup_printf("source=%i sink=%i", operation->pulse->internal_source_id, operation->pulse->external_sink_id); 
    g_message("From BT to alsa: %s", loopback_bt_source_arg);
    g_message("From Alsa to BT: %s", loopback_int_source_arg);

    op = pa_context_load_module (operation->pulse->ctx,
                            "module-loopback",
                            loopback_bt_source_arg,
                            NULL,
                            NULL);
    if (op)
        pa_operation_unref(op);
    // Last one we report... I totally shouldn't be doing this
    op = pa_context_load_module (operation->pulse->ctx,
                            "module-loopback",
                            loopback_int_source_arg,
                            NULL,
                            NULL);
    if (op)
        pa_operation_unref(op);
    } else {
        //    op = pa_context_get_module_info_list(self->ctx, init_module_info, self);

        op = pa_context_get_module_info_list(operation->pulse->ctx, unload_loopback_callback, NULL);
        if (op)
            pa_operation_unref(op);
    }

    operation_complete_cb(operation->pulse->ctx, 1, operation);
/*    op = pa_context_get_sink_info_by_index(operation->pulse->ctx,
                                           operation->pulse->sink_id,
                                           set_output_port, operation);
    if (op)
        pa_operation_unref(op);
*/
    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        if (cad_op->callback)
            cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

/* TODO: We need to cleanup after ourselves too... */
gboolean cad_pulse_find_bt_audio_capabilities(void) 
{
    CadPulse *self = cad_pulse_get_default();
    pa_operation *op;

    g_message("Scan bluetooth devices with audio support as headset");
   // op = pa_context_get_card_info_list(self->ctx, search_alternative_cards, self);
    op = pa_context_get_card_info_list(self->ctx, get_card_info_callback, self);
    if (op)
        pa_operation_unref(op);

    if (self->external_card_connected) {
        g_message("External card config updated!");
        self->external_card_connected = FALSE;
        /* TODO: If in-call, automatically trigger the profile change */
    } else {
        g_message("No external cards found");
        self->bt_audio = 0;
        self->external_card_id = -1;
        self->external_source_id = -1;
        self->external_sink_id = -1;
    }
    return G_SOURCE_REMOVE;
}

