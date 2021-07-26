/*
 * Copyright (C) 2018, 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "callaudiod-pulse"

#include "cad-pulse.h"

#include "libcallaudio.h"

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

struct _CadPulse
{
    GObject parent_instance;

    pa_glib_mainloop  *loop;
    pa_context        *ctx;

    snd_use_case_mgr_t *ucm;

    int card_id;
    int sink_id;
    int source_id;

    gchar *card_name;
    gboolean has_voice_profile;
    gchar *speaker_port;

    GHashTable *sink_ports;
    GHashTable *source_ports;

    CallAudioMode current_mode;
};

G_DEFINE_TYPE(CadPulse, cad_pulse, G_TYPE_OBJECT);

typedef struct _CadPulseOperation {
    CadPulse *pulse;
    CadOperation *op;
    guint value;
} CadPulseOperation;

static void pulseaudio_cleanup(CadPulse *self);
static gboolean pulseaudio_connect(CadPulse *self);

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
    if (self->source_id < 0)
        return;

    target_port = get_available_source_port(info, NULL);
    if (target_port) {
        op = pa_context_set_source_port_by_index(ctx, self->source_id,
                                                 target_port, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
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
        }

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            g_hash_table_insert (self->sink_ports,
                                 g_strdup(port->name),
                                 GINT_TO_POINTER(port->available));
        }
    }

    g_debug("SINK:   speaker_port='%s'", self->speaker_port);
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
    if (self->sink_id < 0)
        return;

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
    const gchar *prop;
    guint i;
    int err;

    if (eol != 0)
        return;

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
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, CARD_MODEM_CLASS) == 0)
        return;

    self->card_id = info->index;
    self->card_name = g_strdup_printf("hw:%s", pa_proplist_gets(info->proplist, "alsa.card"));
    err = snd_use_case_mgr_open(&self->ucm, pa_proplist_gets(info->proplist, "alsa.card_name"));
    if (err < 0)
        g_warning("Unable to open UCM for card '%s'", self->card_name);

    g_debug("CARD: idx=%u name='%s'", info->index, info->name);

    for (i = 0; i < info->n_profiles; i++) {
        pa_card_profile_info2 *profile = info->profiles2[i];

        if (strstr(profile->name, SND_USE_CASE_VERB_VOICECALL) != NULL) {
            self->has_voice_profile = TRUE;
            if (info->active_profile2 == profile)
                self->current_mode = CALL_AUDIO_MODE_CALL;
            break;
        }
    }

    g_debug("CARD:   %s voice profile", self->has_voice_profile ? "has" : "doesn't have");
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

 static void init_pulseaudio_objects(CadPulse *self)
 {
     pa_operation *op;

     self->card_id = self->sink_id = self->source_id = -1;
     self->sink_ports = self->source_ports = NULL;

     op = pa_context_get_card_info_list(self->ctx, init_card_info, self);
     if (op)
         pa_operation_unref(op);
     op = pa_context_get_module_info_list(self->ctx, init_module_info, self);
     if (op)
         pa_operation_unref(op);
     op = pa_context_get_sink_info_list(self->ctx, init_sink_info, self);
     if (op)
         pa_operation_unref(op);
     op = pa_context_get_source_info_list(self->ctx, init_source_info, self);
     if (op)
         pa_operation_unref(op);
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
            g_debug("new sink %u", idx);
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
}

CadPulse *cad_pulse_get_default(void)
{
    static CadPulse *pulse = NULL;

    if (pulse == NULL) {
        g_debug("initializing pulseaudio backend...");
        pulse = g_object_new(CAD_TYPE_PULSE, NULL);
        g_object_add_weak_pointer(G_OBJECT(pulse), (gpointer *)&pulse);
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
            if (operation->op->type == CAD_OPERATION_SELECT_MODE &&
                operation->op->success) {
                operation->pulse->current_mode = operation->value;
            }

            operation->op->callback(operation->op);
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

    if (operation->op->type == CAD_OPERATION_SELECT_MODE) {
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

static void mute_using_ucm(CadPulse *self, const pa_source_info *info, guint value)
{
    char *profile_name;
    char *port_name;
    char *ucm_query;
    const char *ctl_name;
    int err;
    guint i;
    snd_ctl_t *ctl;
    snd_ctl_elem_id_t *ctl_id;
    snd_ctl_elem_info_t *ctl_info;
    snd_ctl_elem_value_t *ctl_value;
    // Those allocate data on the stack
    snd_ctl_elem_id_alloca(&ctl_id);
    snd_ctl_elem_info_alloca(&ctl_info);
    snd_ctl_elem_value_alloca(&ctl_value);

    if (self->has_voice_profile && self->current_mode == CALL_AUDIO_MODE_CALL)
        profile_name = SND_USE_CASE_VERB_VOICECALL;
    else
        profile_name = SND_USE_CASE_VERB_HIFI;

    // PulseAudio prefixes the UCM device name with "[In] " (5 chars long)
    port_name = g_strdup(info->active_port->name + 5);

    ucm_query = snd_use_case_identifier("=CaptureSwitch/%s/%s", port_name, profile_name);
    err = snd_use_case_get(self->ucm, ucm_query, &ctl_name);
    if (err < 0) {
        g_warning("Unable to get CaptureSwitch from UCM (%s)", ucm_query);
        return;
    }
    free(ucm_query);

    // Populate the ctl_id
    err = snd_use_case_parse_ctl_elem_id(ctl_id, "CaptureSwitch", ctl_name);
    if (err < 0) {
        g_warning("Unable to parse CaptureSwitch '%s'", ctl_name);
        return;
    }

    err = snd_ctl_open(&ctl, self->card_name, 0);
    if (err < 0) {
        g_warning("Unable to open control for card '%s'", self->card_name);
        return;
    }

    // Retrieve full information about the control so we can change its value
    snd_ctl_elem_info_set_id(ctl_info, ctl_id);
    err = snd_ctl_elem_info(ctl, ctl_info);
    if (err < 0) {
        g_warning("Unable to get control info for '%s'", ctl_name);
        snd_ctl_close(ctl);
        return;
    }
    snd_ctl_elem_info_get_id(ctl_info, ctl_id);
    snd_ctl_elem_value_set_id(ctl_value, ctl_id);

    // Set value for all channels
    for (i = 0; i < info->channel_map.channels; i++)
        snd_ctl_elem_value_set_boolean(ctl_value, i, value);

    // Write to the actual hardware control
    err = snd_ctl_elem_write(ctl, ctl_value);
    if (err < 0) {
        g_warning("Unable to set value %u for '%s'", value, ctl_name);
    }

    snd_ctl_close(ctl);
}

static void set_mic_mute(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_operation *op = NULL;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    if (info->card != operation->pulse->card_id || info->index != operation->pulse->source_id)
        return;

    if (info->mute && !operation->value) {
        g_debug("mic is muted, unmuting...");
        op = pa_context_set_source_mute_by_index(ctx, operation->pulse->source_id, 0,
                                                 operation_complete_cb, operation);
    } else if (!info->mute && operation->value) {
        g_debug("mic is active, muting...");
        op = pa_context_set_source_mute_by_index(ctx, operation->pulse->source_id, 1,
                                                 operation_complete_cb, operation);
    }
    mute_using_ucm(operation->pulse, info, !operation->value);

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
 * */
void cad_pulse_select_mode(guint mode, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }
    if (!operation) {
        g_critical("%s: unable to allocate memory", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_SELECT_MODE);

    operation->pulse = cad_pulse_get_default();
    operation->op = cad_op;
    operation->value = mode;

    if (mode != CALL_AUDIO_MODE_CALL) {
        /*
         * When ending a call, we want to make sure the mic doesn't stay muted
         */
        CadPulseOperation *unmute_op = g_new0(CadPulseOperation, 1);

        unmute_op->pulse = operation->pulse;
        unmute_op->value = FALSE;

        op = pa_context_get_source_info_by_index(unmute_op->pulse->ctx,
                                                 unmute_op->pulse->source_id,
                                                 set_mic_mute, unmute_op);
        if (op)
            pa_operation_unref(op);
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
    if (!operation) {
        g_critical("%s: unable to allocate memory", __func__);
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
    if (!operation) {
        g_critical("%s: unable to allocate memory", __func__);
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

    op = pa_context_get_source_info_by_index(operation->pulse->ctx,
                                             operation->pulse->source_id,
                                             set_mic_mute, operation);
    if (op)
        pa_operation_unref(op);

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}
