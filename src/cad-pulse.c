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
#include <stdint.h>

#define APPLICATION_NAME "CallAudio"
#define APPLICATION_ID   "org.mobian-project.CallAudio"

#define SINK_CLASS "sound"
#define CARD_FORM_FACTOR "internal"
#define CARD_MODEM_CLASS "modem"
#define CARD_MODEM_NAME "Modem"
#define CARD_MODEM_ALT_NAME "LTE"
#define PA_BT_DRIVER "module-bluez5-device.c"
#define PA_USB_DRIVER "snd_usb_audio"
#define PA_BT_PREFERRED_PROFILE "handsfree_head_unit"
#define PA_BT_A2DP_PROFILE "a2dp_sink"

#define MODEM_LOOPBACK_CAPTURE "Line In"
#define MODEM_LOOPBACK_PLAYBACK "Line Out"

#define SND_UNKNOWN_PLAYBACK "output"
#define SND_UNKNOWN_CAPTURE "input"

typedef struct _Port {
    gboolean available;
    gchar *port;
    guint verb;
} Port;

typedef struct _Ports {
    Port *earpiece; // Earpiece & handset
    Port *headset;
    Port *headphones;
    Port *speaker;
    Port *primary_mic;
    Port *headset_mic;
    Port *headphones_mic;
    /* Line in & out for modem audio if required on device
     * Needs specific verbs on UCM
     */
    Port *passthru_in;
    Port *passthru_out;
} Ports;

typedef struct _AudioCard
{
   uint32_t card_id;

   gchar *card_name;
   gchar *card_description;
   guint device_type;

   gboolean needs_loopback;
   gboolean has_voice_profile;

   Ports *ports;
   
   int sink_id;
   int source_id;
   gchar *sink_name;
   gchar *source_name;
} AudioCard;

struct _CadPulse
{
    GObject parent_instance;

    GObject *manager;

    pa_glib_mainloop  *loop;
    pa_context        *ctx;

    CallAudioMode audio_mode;
    CallAudioMicState mic_state;

    /* Redo */
    gboolean loopback_enabled;
    guint total_external_cards; // Total external devices, including modem
    gboolean modem_has_usb_audio; // If it has we'll only need hifi
    gboolean call_audio_external_needs_pass_thru; // do we have a helper in alsa ucm for modem loopback?
    
    GArray * cards;
    AudioCard *primary_card;
    AudioCard *modem_card;
    uint32_t current_active_dev;
    guint current_active_verb;
    gboolean syncing_sources;
    gboolean syncing_sinks;
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

static AudioCard *get_card(uint32_t card_id) {
    AudioCard *card = NULL;
    CadPulse *self = cad_pulse_get_default();
    if (self->primary_card && card_id == self->primary_card->card_id) {
        card = self->primary_card;
        return card;
    }

    if (self->modem_has_usb_audio && self->modem_card &&
                   card_id == self->modem_card->card_id) {
        card = self->modem_card;
        return card;
    }
    for (int i = 0; i < self->total_external_cards; i++) {
        card = g_array_index( self->cards, AudioCard *, i );
        if (card && card_id == card->card_id) {
            return card;
        }
    }
    return NULL;
}

static void init_source_info(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    AudioCard *card;
    CadPulse *self = cad_pulse_get_default();

    if (eol != 0) {
        self->syncing_sources = FALSE;
        return;
    }

    self->syncing_sources = TRUE;
    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    card = get_card(info->card);

    if (!card) {
        g_critical("Can't find any card for this source, bailing out (card id %i)", info->card);
        return;
    }

    if (info->monitor_of_sink != PA_INVALID_INDEX) {
        g_message(" - Source %s is a monitor of another sink (card id %i, source %i is monitor of sink %i)", info->name, info->card, info->index, info->monitor_of_sink);
        return;
    }

    card->source_id = info->index;
    card->source_name = g_strdup(info->name);

    card->ports->primary_mic->available = FALSE;
    card->ports->headset_mic->available = FALSE;
    card->ports->headphones_mic->available = FALSE;
    card->ports->passthru_in->available = FALSE;
    for (int i = 0; i < info->n_ports; i++) {
        pa_source_port_info *port = info->ports[i];
        g_message(" - Source %i (%s)-> Port %s, available: %i", info->index, info->name, port->name, port->available);
        if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADSET, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headset_mic->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADPHONES, -1)) != NULL&&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headphones_mic->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_MIC, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headset_mic->available = TRUE;            
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_UNKNOWN_PLAYBACK, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headset_mic->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_UNKNOWN_CAPTURE, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->primary_mic->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(MODEM_LOOPBACK_CAPTURE, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->passthru_in->available = TRUE;
        } 
    }
}

/******************************************************************************
 * Sink management
 *
 * The following functions take care of monitoring and configuring the default
 * sink (output)
 ******************************************************************************/

static void init_sink_info(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    AudioCard *card;
    CadPulse *self = cad_pulse_get_default();

    if (eol != 0) {
        self->syncing_sinks = FALSE;
        return;
    }

    self->syncing_sinks = TRUE;
    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }
    
    card = get_card(info->card);

    if (!card) {
        g_message("Can't find any card for this sink, bailing out (card id %i)", info->card);
        return;
    }

    card->sink_id = info->index;
    card->sink_name = g_strdup(info->name);

    /* 
        * Note we can get here both from init_card_info or update_card_info
        * Headphones/ headset could have been available earlier so we reset
        * the availability state of all devices, and if they're still there
        * we'll enable them again
        */
    card->ports->speaker->available = FALSE;
    card->ports->earpiece->available = FALSE;
    card->ports->headset->available = FALSE;
    card->ports->headset->available = FALSE;
    card->ports->headphones->available = FALSE;
    card->ports->passthru_out->available = FALSE;

    for (int i = 0; i < info->n_ports; i++) {
        pa_sink_port_info *port = info->ports[i];
        g_message(" - Sink %i (%s)-> Port %s, available: %i", info->index, info->name, port->name, port->available);
        if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_SPEAKER, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->speaker->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_EARPIECE, -1)) != NULL&&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->earpiece->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADSET, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headset->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HANDSET, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->earpiece->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADPHONES, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headphones->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_UNKNOWN_PLAYBACK, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headset->available = TRUE;
        } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_UNKNOWN_CAPTURE, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->headset->available = TRUE;
        }  else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(MODEM_LOOPBACK_PLAYBACK, -1)) != NULL &&
            (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
            card->ports->passthru_out->available = TRUE;
        } 
    }
    return;    
}

/******************************************************************************
 * Card management
 *
 * The following functions take care of gathering information about the default
 * sound card
 ******************************************************************************/

static void sync_audio_mode_path(pa_context *c, int success, void *userdata) 
{
    pa_operation *op;
    CadPulse *self = cad_pulse_get_default();
    int retries = 10;

    if (success) {
        g_message("Profile change succeeded");
    } else {
        g_message("Profile change failed");
    }
    self->syncing_sinks = TRUE;
    self->syncing_sources = TRUE;
    /* Every time we switch profiles our sink and source IDs change!*/
    op = pa_context_get_sink_info_list(self->ctx, init_sink_info, self);
    if (op)
        pa_operation_unref(op);

    op = pa_context_get_source_info_list(self->ctx, init_source_info, self);
    if (op)
        pa_operation_unref(op);
    
    while((self->syncing_sinks || self->syncing_sources) && retries >0) {
        g_message("WAITING: Trigger audio path sync: %i", retries);
        sleep(0.5);
        retries--;
    }
    g_message("DOING: Trigger audio path sync");
    cad_pulse_set_output(self->current_active_dev,self->current_active_verb, self->audio_mode);

}
static void set_card_profile(uint32_t card_id, gchar *card_profile) 
{
    CadPulse *self = cad_pulse_get_default();
    pa_operation *op;

    g_message("Setting profile %s in card %u", card_profile, card_id);
    op = pa_context_set_card_profile_by_index(self->ctx, card_id,
                                        card_profile,
                                        sync_audio_mode_path, NULL);
    if (op)
        pa_operation_unref(op);

}

static void update_card_info(pa_context *ctx, const pa_card_info *info, int eol, void *data)
{
    CadPulse *self = data;
    AudioCard *this_card = NULL;
    pa_operation *op;
    if (eol != 0 ) {
        if (!self->primary_card) {
            g_critical("No primary card found, retrying in 3s...");
            g_timeout_add_seconds(3, G_SOURCE_FUNC(init_pulseaudio_objects), self);
        }
        return;
    }
   
    if (!info) {
        g_critical("%s: PA returned no card info (eol=%d)", __func__, eol);
        return;
    }

    this_card = get_card(info->index);

    if (!this_card) {
        g_message("Error retrieving card configuration (card id %i)", info->index);
        return;
    }

    g_message("Card %i updated (%s)", this_card->card_id, this_card->card_description);
    
    // Set an invalid sink and source to be processed later
    // Sinks and sources change with every profile switch
    this_card->sink_id = -1;
    this_card->source_id = -1;

    op = pa_context_get_sink_info_list(self->ctx, init_sink_info, self);
    if (op)
        pa_operation_unref(op);
    op = pa_context_get_source_info_list(self->ctx, init_source_info, self);
    if (op)
        pa_operation_unref(op);

    if (self->audio_mode !=CALL_AUDIO_MODE_DEFAULT)
        g_object_set(self->manager, "available-devices", cad_pulse_get_available_devices(), NULL);

}

static void init_card_info(pa_context *ctx, const pa_card_info *info, int eol, void *data)
{
    CadPulse *self = data;
    pa_operation *op;
    const gchar *prop;
    guint i;
    
    /* Initialize this card */
    AudioCard *this_card = g_new0(AudioCard, 1);
    this_card->ports = g_new0(Ports, 1);
    this_card->ports->earpiece = g_new0(Port, 1);
    this_card->ports->headset = g_new0(Port, 1);
    this_card->ports->headphones = g_new0(Port, 1);
    this_card->ports->speaker = g_new0(Port, 1);
    this_card->ports->primary_mic = g_new0(Port, 1);
    this_card->ports->headset_mic = g_new0(Port, 1);
    this_card->ports->headphones_mic = g_new0(Port, 1);
    this_card->ports->passthru_in = g_new0(Port, 1);
    this_card->ports->passthru_out = g_new0(Port, 1);

    if (eol != 0 ) {
        if (!self->primary_card) {
            g_critical("No suitable card found, retrying in 3s...");
            g_timeout_add_seconds(3, G_SOURCE_FUNC(init_pulseaudio_objects), self);
        }
        return;
    }
   
    if (!info) {
        g_critical("%s: PA returned no card info (eol=%d)", __func__, eol);
        return;
    }

    if (self->primary_card) {
        if (info->index == self->primary_card->card_id) {
            return;
        }
    } else if (self->total_external_cards > 0){
        for (i = 0; i < self->total_external_cards; i++) {
            AudioCard * card = g_array_index( self->cards, AudioCard*, i );
            if (card->card_id == info->index) {
                g_message("No need to add %s again", card->card_name);
                return;
            }
        }
    }

    this_card->card_id = info->index;
    this_card->card_name = g_strdup(info->name);
    prop = pa_proplist_gets(info->proplist, "device.description");
    if (prop) {
        this_card->card_description = g_strdup(prop);
    } else {
        g_message("No description for the card");
        this_card->card_description = g_strdup(info->name);
    }
    
    g_message("Card %i: %s, friendly name %s", this_card->card_id, this_card->card_name, this_card->card_description);
    
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_FORM_FACTOR);
    if (prop && strcmp(prop, CARD_FORM_FACTOR) == 0) {
        g_message(" - Card form factor is internal");
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_INTERNAL;
    } else {
        g_message(" - Card form factor is external");
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_EXTERNAL;
    }

    prop = pa_proplist_gets(info->proplist, "alsa.card_name");
    if (prop && strcmp(prop, CARD_MODEM_NAME) == 0) {
        g_message(" - Card %s is a modem", this_card->card_name);
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_MODEM;
    }

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, CARD_MODEM_CLASS) == 0) {
        g_message(" - Card %s is a modem", this_card->card_name);
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_MODEM;
    }
    
    prop = pa_proplist_gets(info->proplist, "alsa.card_name");
    if (prop && strstr(prop, CARD_MODEM_ALT_NAME) != NULL) {
        g_message(" - Card %s is a modem", this_card->card_name);
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_MODEM;
    }

    if (strcmp(info->driver, PA_BT_DRIVER) == 0) {
        g_message(" - Card %s is a bluetooth device", this_card->card_name);
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_BT;
    }

    if (strcmp(info->driver, PA_USB_DRIVER) == 0) {
        g_message(" - Card %s is a USB device", this_card->card_name);
        this_card->device_type = CAD_PULSE_DEVICE_TYPE_USB;
    }
    
    for (i = 0; i < info->n_ports; i++) {
        pa_card_port_info *port = info->ports[i];
        g_message(" - Card port %s", port->name);
            if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_SPEAKER, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->speaker->available = TRUE;
                this_card->ports->speaker->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_EARPIECE, -1)) != NULL&&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->earpiece->available = TRUE;
                this_card->ports->earpiece->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADSET, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->headset->available = TRUE;
                this_card->ports->headset->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HANDSET, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->earpiece->available = TRUE;
                this_card->ports->earpiece->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADPHONES, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->headphones->available = TRUE;
                this_card->ports->headphones->port = g_strdup(port->name);
            }  else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(MODEM_LOOPBACK_PLAYBACK, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->passthru_out->available = TRUE;
                this_card->ports->passthru_out->port = g_strdup(port->name);
                self->call_audio_external_needs_pass_thru = TRUE;
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADSET, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->headset_mic->available = TRUE;
                this_card->ports->headset_mic->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_HEADPHONES, -1)) != NULL&&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->headphones_mic->available = TRUE;
                this_card->ports->headphones_mic->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_USE_CASE_DEV_MIC, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->primary_mic->available = TRUE;
                this_card->ports->primary_mic->port = g_strdup(port->name);
            } else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(MODEM_LOOPBACK_CAPTURE, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->passthru_in->available = TRUE;
                this_card->ports->passthru_in->port = g_strdup(port->name);
                self->call_audio_external_needs_pass_thru = TRUE;
            }  else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_UNKNOWN_PLAYBACK, -1)) != NULL &&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->headphones->available = TRUE;
                this_card->ports->headphones->port = g_strdup(port->name);
            }    else if (strstr(g_ascii_strdown(port->name, -1), g_ascii_strdown(SND_UNKNOWN_CAPTURE, -1)) != NULL&&
                (port->available == PA_PORT_AVAILABLE_UNKNOWN || port->available == PA_PORT_AVAILABLE_YES))  {
                this_card->ports->headphones_mic->available = TRUE;
                this_card->ports->headphones_mic->port = g_strdup(port->name);
            }
    }
    /* 
     * If card is primary and we're using a VoiceCall profile, set the property
     * Useful if callaudiod crashed mid call and was restarted 
     */
    if (this_card->device_type == CAD_PULSE_DEVICE_TYPE_INTERNAL) {
        for (i = 0; i < info->n_profiles; i++) {
            pa_card_profile_info2 *profile = info->profiles2[i];

            if (strstr(profile->name, SND_USE_CASE_VERB_VOICECALL) != NULL) {
                this_card->has_voice_profile = TRUE;
                if (info->active_profile2 == profile)
                    self->audio_mode = CALL_AUDIO_MODE_CALL;
                else
                    self->audio_mode = CALL_AUDIO_MODE_DEFAULT;
                break;
            }
        }
    }

    // We were able determine the current mode, set the corresponding D-Bus property
    if (self->audio_mode != CALL_AUDIO_MODE_UNKNOWN)
        g_object_set(self->manager, "audio-mode", self->audio_mode, NULL);

    g_debug("CARD: %s voice profile", this_card->has_voice_profile ? "has" : "doesn't have");
    // Set an invalid sink and source to be processed later
    // Sinks and sources change with every profile switch
    this_card->sink_id = -1;
    this_card->source_id = -1;

    /* Found a suitable card, let's prepare the sink/source */
    /* Get sink and source will retrieve all sinks and sources for *all* cards! 
     * We need to store our new object into the cardarray before they run
    */
    if (this_card->device_type == CAD_PULSE_DEVICE_TYPE_INTERNAL) {
        g_message(" - Setting %s as the primary card", this_card->card_name);
        self->primary_card = this_card;
    } else if (this_card->device_type == CAD_PULSE_DEVICE_TYPE_MODEM) {
        g_message(" - Setting %s as a modem", this_card->card_name);
        self->modem_card = this_card;
        self->modem_has_usb_audio = TRUE;
    } else {
        g_message("- Setting %s as a secondary card", this_card->card_name); 
        // Add it to the card array
        self->cards = g_array_append_val(self->cards, this_card);
        self->total_external_cards++;
    }

    g_message("External cards found: %u", self->total_external_cards);
    op = pa_context_get_sink_info_list(self->ctx, init_sink_info, self);
    if (op)
        pa_operation_unref(op);
    op = pa_context_get_source_info_list(self->ctx, init_source_info, self);
    if (op)
        pa_operation_unref(op);

  if (self->audio_mode != CALL_AUDIO_MODE_DEFAULT)
    g_object_set(self->manager, "available-devices", cad_pulse_get_available_devices(), NULL);
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
    self->total_external_cards = 0;
    self->cards = g_array_new(FALSE, FALSE, sizeof(AudioCard *));
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
        if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            g_message("sink %u removed", idx);
        } else if (kind == PA_SUBSCRIPTION_EVENT_NEW) {
            g_message("new sink %u", idx);
            op = pa_context_get_sink_info_by_index(ctx, idx, init_sink_info, self);
            if (op)
                pa_operation_unref(op);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            g_message("source %u removed", idx);
        } else if (kind == PA_SUBSCRIPTION_EVENT_NEW) {
            g_message("new source %u", idx);
            op = pa_context_get_source_info_by_index(ctx, idx, init_source_info, self);
            if (op)
                pa_operation_unref(op);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_CARD:
    if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
        for (int j = 0 ; j < self->total_external_cards; j++) {
            AudioCard *card = g_array_index( self->cards, AudioCard*, j);
            if (card && card->card_id == idx) {
                g_message("Removing card %s", card->card_name);
                self->total_external_cards--;
                g_array_remove_index(self->cards, j);

                cad_pulse_set_output(self->primary_card->card_id, CAD_PULSE_DEVICE_VERB_AUTO, self->audio_mode);
                break;
            }
        } 
    } else if (kind == PA_SUBSCRIPTION_EVENT_NEW ) {
        g_message("New card added, rescanning...");
        op = pa_context_get_card_info_list(self->ctx, init_card_info, self);
        if (op)
            pa_operation_unref(op);
    
    } 
        break;
    default:
        break;
    }

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

    if (strcmp(info->name, "module-loopback") == 0) {
        g_debug("Unloading '%s'", info->name);
        op = pa_context_unload_module(ctx, info->index, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
    return;
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

    /* TODO: Add cleanup here*/
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
    self->mic_state = CALL_AUDIO_MIC_UNKNOWN;
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
                    cad_pulse_set_output(operation->pulse->current_active_dev, operation->pulse->current_active_verb, new_value);

                    break;
                case CAD_OPERATION_ENABLE_SPEAKER:
                        /*
                        * Keeping this for compatibility with existing Phosh 
                        * builds. This should be removed as it makes no sense
                        * to have two operations doing the same
                        */
                        g_object_set(operation->pulse->manager, "speaker-state", new_value, NULL);
                        g_object_set(operation->pulse->manager, "available-devices", cad_pulse_get_available_devices(), NULL);
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
                case CAD_OPERATION_OUTPUT_DEVICE:
                    /*
                     * Output device & verb has already been switched
                     * We trigger get_available_devices() to resync the 
                     * available devices list
                     */
                    g_object_set(operation->pulse->manager, "output-device-state", new_value, NULL);
                    g_object_set(operation->pulse->manager, "available-devices", cad_pulse_get_available_devices(), NULL);
  
                    break;
                default:
                    break;
                }
            }
            free(operation->op);
        }
        free(operation);
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
    AudioCard *card = NULL;

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

    if (!operation->pulse->primary_card) {
        g_critical("Primary card not found");
        goto error;
    }

    switch (operation->value) {
        case CALL_AUDIO_MODE_UNKNOWN:
            g_critical("** Unknown call state");
            /* We need to do reset & recovery here */
            break;

        case CALL_AUDIO_MODE_DEFAULT:
            g_message("** Switching to HiFi profile");
            if (operation->pulse->loopback_enabled) {
                op = pa_context_get_module_info_list(operation->pulse->ctx, unload_loopback_callback, NULL);
                if (op)
                    pa_operation_unref(op);

                operation->pulse->loopback_enabled = FALSE;
            }
            /* ATTEMPT 3: Let's take it by the hand and guide it */
            // Switch to A2DP / USB C headset
            if (operation->pulse->total_external_cards > 0) {
                card = g_array_index( operation->pulse->cards, AudioCard*, operation->pulse->total_external_cards-1);
                if (!card) {
                    g_critical("%s: Couldn't retrieve the external card data", __func__);
                    break;
                }

                if (card->device_type == CAD_PULSE_DEVICE_TYPE_BT) {
                    set_card_profile(card->card_id, PA_BT_A2DP_PROFILE);
                }
            }

            set_card_profile(operation->pulse->primary_card->card_id, SND_USE_CASE_VERB_HIFI);
            
            operation->pulse->current_active_dev = operation->pulse->primary_card->card_id;
            operation->pulse->current_active_verb = CAD_PULSE_DEVICE_VERB_AUTO;

            // Switch to A2DP / USB C headset
            if (operation->pulse->total_external_cards > 0) {
                card = g_array_index( operation->pulse->cards, AudioCard*, operation->pulse->total_external_cards-1);
                if (!card) {
                    g_critical("%s: Couldn't retrieve the external card data", __func__);
                    return;
                }

                operation->pulse->current_active_dev = card->card_id;
            }
            break;

        case CALL_AUDIO_MODE_CALL:
            /*
             * We're either in modem call or SIP.
             * If it's a modem call and we have a VoiceCall profile we set it
             * Otherwise the process is the same for both:
             *  Find the last connected card, and if there's one, set a loopback
             *  Otherwise go with the primary card
            */
            if (operation->pulse->primary_card->has_voice_profile && !operation->pulse->modem_has_usb_audio) {
                g_message("** Switching to VoiceCall profile");
                set_card_profile(operation->pulse->primary_card->card_id, SND_USE_CASE_VERB_VOICECALL);
            }
        /* Fall through */
        case CALL_AUDIO_MODE_SIP:
            if (operation->pulse->total_external_cards > 0) {
                card = g_array_index( operation->pulse->cards, AudioCard*, operation->pulse->total_external_cards-1 );
                if (!card) {
                    g_message("Card disappeared!");
                    return;
                }
                if (card->device_type == CAD_PULSE_DEVICE_TYPE_BT) {
                    g_message("** BT Handler: Switching %s to %s", card->card_description, PA_BT_PREFERRED_PROFILE);
                    set_card_profile(card->card_id, PA_BT_PREFERRED_PROFILE);
                }
            }
            
            g_message("** Using primary card as an output");
            operation->pulse->current_active_dev = operation->pulse->primary_card->card_id;
            operation->pulse->current_active_verb = CAD_PULSE_DEVICE_VERB_EARPIECE;
            cad_pulse_set_output(operation->pulse->primary_card->card_id, CAD_PULSE_DEVICE_VERB_EARPIECE, mode);

            break;
        default:
            g_critical("%s: Unknown operation requested: %u", __func__, operation->value);
            break;
    }

    g_message("%s closing normally", __func__);
    operation_complete_cb(operation->pulse->ctx, 1, operation);
    return;

error:
    g_message("%s failed with error", __func__);
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
    AudioCard *card = NULL;
    
    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_MUTE_MIC);

    operation->pulse = cad_pulse_get_default();
    
    card = get_card(operation->pulse->current_active_dev);
    if (!card) {
        g_critical("%s: Can't find active card", __func__);
        goto error;
    }

    if (card->source_id < 0) {
        g_warning("card has no usable source");
        goto error;
    }

    operation->op = cad_op;
    operation->value = (guint)mute;
    g_message("Current active device : %i Source ID %i", card->card_id, card->source_id);

    if (operation->pulse->mic_state == CALL_AUDIO_MIC_OFF && !operation->value) {
        g_debug("mic is muted, unmuting...");
        op = pa_context_set_source_mute_by_index(operation->pulse->ctx,
                                                 card->source_id, 0,
                                                 operation_complete_cb, operation);
    } else if (operation->pulse->mic_state == CALL_AUDIO_MIC_ON && operation->value) {
        g_debug("mic is active, muting...");
        op = pa_context_set_source_mute_by_index(operation->pulse->ctx,
                                                 card->source_id, 1,
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

CallAudioMicState cad_pulse_get_mic_state(void)
{
    CadPulse *self = cad_pulse_get_default();
    return self->mic_state;
}

uint32_t cad_pulse_output_device_state(void)
{
    CadPulse *self = cad_pulse_get_default();
    return self->current_active_dev;
}

static gboolean is_dev_active(guint dev_id, guint dev_verb) {
    CadPulse *self = cad_pulse_get_default();
    if (dev_id == self->current_active_dev && dev_verb == self->current_active_verb) {
        g_message("[ACTIVE] Device ID %i Verb %i", dev_id, dev_verb);
        return TRUE;
    }

    g_message("[INACTIVE] Device ID %i Verb %i", dev_id, dev_verb);
    return FALSE;
}

GVariant *cad_pulse_get_available_devices(void) 
{
    CadPulse *self = cad_pulse_get_default();
    gchar *tmpcard;
    AudioCard * card;
    GVariant *devices;
    GVariantBuilder *device;
    guint device_type = 0;
    device = g_variant_builder_new(G_VARIANT_TYPE("a(buuus)"));

    if (!self->primary_card) {
        g_message("Primary card is not available yet");
        return NULL;
    }

    if (self->primary_card->ports->earpiece->available) {
        tmpcard = g_strdup_printf("Earpiece");
        g_variant_builder_add(device, "(buuus)", 
                            is_dev_active(self->primary_card->card_id, CAD_PULSE_DEVICE_VERB_EARPIECE), 
                            self->primary_card->card_id, device_type, CAD_PULSE_DEVICE_VERB_EARPIECE, tmpcard);
    }
    if (self->primary_card->ports->headset->available) {
        tmpcard = g_strdup_printf("Headset");
        g_variant_builder_add(device, "(buuus)", 
                            is_dev_active(self->primary_card->card_id, CAD_PULSE_DEVICE_VERB_HEADSET), 
                            self->primary_card->card_id, device_type, CAD_PULSE_DEVICE_VERB_HEADSET, tmpcard);
   }
    if (self->primary_card->ports->speaker->available) {
        tmpcard =  g_strdup_printf("Speaker");
        g_variant_builder_add(device, "(buuus)", 
                            is_dev_active(self->primary_card->card_id, CAD_PULSE_DEVICE_VERB_SPEAKER), 
                            self->primary_card->card_id, device_type, CAD_PULSE_DEVICE_VERB_SPEAKER, tmpcard);
 
    }
    if (self->primary_card->ports->headphones->available) {
       tmpcard = g_strdup_printf("Headphones");
        g_variant_builder_add(device, "(buuus)", 
                            is_dev_active(self->primary_card->card_id, CAD_PULSE_DEVICE_VERB_HEADPHONES), 
                            self->primary_card->card_id, device_type, CAD_PULSE_DEVICE_VERB_HEADPHONES, tmpcard);
    } 

    for (int i = 0; i < self->total_external_cards; i++) {
        card = g_array_index( self->cards, AudioCard*, i );
        if (!card) {
            g_message("Card disappeared!");
            break;
        }
        if (card->device_type == CAD_PULSE_DEVICE_TYPE_BT) {
            device_type = 1;
        } else if (card->device_type == CAD_PULSE_DEVICE_TYPE_USB) {
            device_type = 2;
        }
        if (card->ports->earpiece->available) {
            tmpcard =  g_strdup_printf("%s: Earpiece", card->card_description);
            g_variant_builder_add(device, "(buuus)", 
                                is_dev_active(card->card_id, CAD_PULSE_DEVICE_VERB_EARPIECE), 
                                card->card_id, device_type, CAD_PULSE_DEVICE_VERB_EARPIECE, tmpcard);
        }
        if (card->ports->headset->available) {
            tmpcard =    g_strdup_printf("%s: Headset", card->card_description);
            g_variant_builder_add(device, "(buuus)", 
                                is_dev_active(card->card_id, CAD_PULSE_DEVICE_VERB_HEADSET), 
                                card->card_id, device_type, CAD_PULSE_DEVICE_VERB_HEADSET, tmpcard);
        }
        if (card->ports->speaker->available) {
            tmpcard =   g_strdup_printf("%s: Speaker", card->card_description);
            g_variant_builder_add(device, "(buuus)", 
                                is_dev_active(card->card_id, CAD_PULSE_DEVICE_VERB_SPEAKER), 
                                card->card_id, device_type, CAD_PULSE_DEVICE_VERB_SPEAKER, tmpcard);
        }
        if (card->ports->headphones->available) {
            tmpcard =  g_strdup_printf("%s: Headphones", card->card_description);
            g_variant_builder_add(device, "(buuus)", 
                                is_dev_active(card->card_id, CAD_PULSE_DEVICE_VERB_HEADPHONES), 
                                card->card_id, device_type, CAD_PULSE_DEVICE_VERB_HEADPHONES, tmpcard);
        }
    }
    devices = g_variant_new("a(buuus)", device);
    return devices;
}


void cad_pulse_set_output(uint32_t device_id, guint device_verb, guint audio_mode) {
    CadPulse *self = cad_pulse_get_default();
    pa_operation *op = NULL;
    AudioCard *target_card = NULL;
    gchar *loopback_bt_source_arg, *loopback_int_source_arg;
    g_message("----->>> %s CALLED: Dev %u Verb %u Audio mode %u ", __func__, device_id, device_verb, audio_mode);

    if (!self->primary_card) {
        g_critical("Primary card not found, can't continue");
        return;
    }

    if (self->loopback_enabled) {
        op = pa_context_get_module_info_list(self->ctx, unload_loopback_callback, NULL);
        if (op)
            pa_operation_unref(op);

        self->loopback_enabled = FALSE;
    }

    if (device_id == self->primary_card->card_id) {
        g_message("%s Requesting a verb for the same card", __func__);
        target_card = self->primary_card;
    } else {
        g_message("%s Requesting output to a different card (%i), looking for it...", __func__, device_id);
        for (int i = 0; i < self->total_external_cards; i++) {
            target_card = g_array_index( self->cards, AudioCard *, i );
            if (device_id ==target_card->card_id) {
                g_message("Found it: %s (%s)", target_card->card_description, target_card->card_name);
                break;
            }
        }
    }

    if (!target_card) {
        g_critical("Couldn't find the target card, reverting to the primary card");
        device_id = self->primary_card->card_id;
        target_card = self->primary_card;
        device_verb = CAD_PULSE_DEVICE_VERB_AUTO;
    }

    if (device_verb == CAD_PULSE_DEVICE_VERB_AUTO) {
        switch (audio_mode) {
            case CALL_AUDIO_MODE_DEFAULT:
                if (target_card->ports->speaker->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_SPEAKER;
                } else if (target_card->ports->headphones->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_HEADPHONES;
                } else if (target_card->ports->headset->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_HEADSET;
                } else {
                    g_message("No port to setup in auto mode");
                }
                break;
            case CALL_AUDIO_MODE_CALL:
            /* Fall through */
            case CALL_AUDIO_MODE_SIP:
                if (target_card->ports->earpiece->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_EARPIECE;
                } else if (target_card->ports->headset->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_HEADSET;
                } else if (target_card->ports->headphones->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_HEADPHONES;
                } else if (target_card->ports->speaker->available) {
                    device_verb = CAD_PULSE_DEVICE_VERB_SPEAKER;
                } else {
                    g_message("No port to setup in auto mode");
                }
                break;
            default:
                g_message("Unknown audio mode, falling back to speaker");
                device_verb = CAD_PULSE_DEVICE_VERB_SPEAKER;
                break;
        }
    }
    
    g_message("Target card: %u : Sink ID: %i, Source ID: %i, selected verb: %u", target_card->card_id, target_card->sink_id, target_card->source_id, device_verb);
  
    switch (device_verb) {
        case CAD_PULSE_DEVICE_VERB_EARPIECE: // Earpiece / Handset
            g_message("Primary card: Earpiece %s",target_card->ports->earpiece->port);
            op = pa_context_set_sink_port_by_index(self->ctx, target_card->sink_id,
                                            target_card->ports->earpiece->port,
                                            NULL, NULL);
            if (op)
                pa_operation_unref(op);
            op = pa_context_set_source_port_by_index(self->ctx, target_card->source_id,
                                            target_card->ports->primary_mic->port,
                                            NULL, NULL);
            self->current_active_dev = target_card->card_id;
            self->current_active_verb = device_verb;
            break;
        case CAD_PULSE_DEVICE_VERB_HEADSET: // Headset
            g_message("Target card: Headset: %s", target_card->ports->headset->port);
            op = pa_context_set_sink_port_by_index(self->ctx, target_card->sink_id,
                                            target_card->ports->headset->port,
                                            NULL, NULL);
            if (op)
                pa_operation_unref(op);
            op = pa_context_set_source_port_by_index(self->ctx, target_card->source_id,
                                            target_card->ports->headset_mic->port,
                                            NULL, NULL);
            self->current_active_dev = target_card->card_id;
            self->current_active_verb = device_verb;
            break;
        case CAD_PULSE_DEVICE_VERB_SPEAKER: // Speaker
            g_message("Target card: Speaker%s",target_card->ports->speaker->port);
            op = pa_context_set_sink_port_by_index(self->ctx, target_card->sink_id,
                                            target_card->ports->speaker->port,
                                            NULL, NULL);
            if (op)
                pa_operation_unref(op);
            op = pa_context_set_source_port_by_index(self->ctx, target_card->source_id,
                                            target_card->ports->primary_mic->port,
                                            NULL, NULL);
            self->current_active_dev = target_card->card_id;
            self->current_active_verb = device_verb;
            break;
        case CAD_PULSE_DEVICE_VERB_HEADPHONES: // Headphones: Shall we use the primary or headset mic here?
            // Not all headsets might be detected as headsets...
            g_message("Target card: Heaphones %s",target_card->ports->headphones->port);
            op = pa_context_set_sink_port_by_index(self->ctx, target_card->sink_id,
                                    target_card->ports->headphones->port,
                                    NULL, NULL);
            if (op)
                pa_operation_unref(op);
            op = pa_context_set_source_port_by_index(self->ctx, target_card->source_id,
                                            target_card->ports->headphones_mic->port,
                                            NULL, NULL);
            self->current_active_dev = target_card->card_id;
            self->current_active_verb = device_verb;
            break;
        default:
            g_message("Unknown output verb: %u", device_verb);
            break;
    }
    if (op)
        pa_operation_unref(op);

    /* Now get loopbacks and default sink and source ready */
    switch (audio_mode) {
        case CALL_AUDIO_MODE_UNKNOWN:
        /* Fall through */
        case CALL_AUDIO_MODE_DEFAULT:
            /* We don't care which device it is if we're in normal mode
             * as there's no need to set up anything else in this case
             */
            op = pa_context_set_default_sink(self->ctx, target_card->sink_name, NULL, NULL);
            if (op)
                pa_operation_unref(op);

            op = pa_context_set_default_source(self->ctx, target_card->source_name, NULL, NULL);
            if (op)
                pa_operation_unref(op);
            break;
        case CALL_AUDIO_MODE_CALL:
        /* Fall through */
        case CALL_AUDIO_MODE_SIP:
            /* Librem will always need to use a loopback 
            * Since we know which one is our target card by now,
            * we should only need to loop between it and the modem
            * and everything else should just work (tm)
            */
            if (self->modem_has_usb_audio) {
                g_message("Modem has USB audio: Loopback to whatever");
                if (!target_card->device_type == CAD_PULSE_DEVICE_TYPE_INTERNAL) {
                    self->current_active_dev = target_card->card_id;
                    self->current_active_verb = CAD_PULSE_DEVICE_VERB_HEADPHONES;
                    if (target_card->device_type == CAD_PULSE_DEVICE_TYPE_BT) {
                        self->current_active_verb = CAD_PULSE_DEVICE_VERB_HEADSET;
                    } else if (target_card->device_type == CAD_PULSE_DEVICE_TYPE_USB) {
                        self->current_active_verb = CAD_PULSE_DEVICE_VERB_HEADSET;
                    }
                }
                self->loopback_enabled = TRUE;
                g_message("TARGET: Source %i %s | Sink %i %s", target_card->source_id, target_card->source_name, target_card->sink_id, target_card->sink_name);
                loopback_bt_source_arg = g_strdup_printf("source=%s sink=%s", target_card->source_name, self->modem_card->sink_name); 
                loopback_int_source_arg = g_strdup_printf("source=%s sink=%s", self->modem_card->source_name, target_card->sink_name); 
                g_message("From Modem to card: %s", loopback_bt_source_arg);
                g_message("From card to modem: %s", loopback_int_source_arg);

                op = pa_context_load_module (self->ctx,
                                        "module-loopback",
                                        loopback_bt_source_arg,
                                        NULL,
                                        NULL);
                if (op)
                    pa_operation_unref(op);
                op = pa_context_load_module (self->ctx,
                                        "module-loopback",
                                        loopback_int_source_arg,
                                        NULL,
                                        NULL);
                if (op)
                    pa_operation_unref(op);
            } else if (!target_card->device_type == CAD_PULSE_DEVICE_TYPE_INTERNAL) {
                /* This needs to be splitted too... 
                *   - PinePhone only needs to set the bluetooth profile when using a BT adapter
                *   - PinePhone will need a loopback when using USBC headset
                *   - PinePhonePro will need a loopback for both cases. 
                *   Find if the adapter is_bt and if main card has a bluetooth profile
                *   Otherwise, use the loopbacks
                */
                g_message("INTERNAL MODEM AUDIO AND EXTERNAL HEADSET");
                self->current_active_dev = target_card->card_id;
                self->current_active_verb = CAD_PULSE_DEVICE_VERB_HEADPHONES;
                if (target_card->device_type == CAD_PULSE_DEVICE_TYPE_BT) {
                    self->current_active_verb = CAD_PULSE_DEVICE_VERB_HEADSET;
                } else if (target_card->device_type == CAD_PULSE_DEVICE_TYPE_USB) {
                    self->current_active_verb = CAD_PULSE_DEVICE_VERB_HEADSET;
                }
                g_message("TARGET: Source %i %s | Sink %i %s", target_card->source_id, target_card->source_name, target_card->sink_id, target_card->sink_name);
                /* Only for the PPP or a phone that needs to set up a specific verb in alsa *and* a loopback */
                if (self->call_audio_external_needs_pass_thru) {
                    g_message("**** WARNING: We need to set a specific sink and source for bluetooth in this device PT_IN: %s, PT_OUT: %s", self->primary_card->ports->passthru_in->port, self->primary_card->ports->passthru_out->port );
                    op = pa_context_set_default_sink(self->ctx, self->primary_card->sink_name, NULL, NULL);
                    if (op)
                        pa_operation_unref(op);

                    op = pa_context_set_default_source(self->ctx, self->primary_card->sink_name, NULL, NULL);
                    if (op)
                        pa_operation_unref(op);

                    op = pa_context_set_sink_port_by_index(self->ctx, self->primary_card->sink_id,
                                            self->primary_card->ports->passthru_out->port,
                                            NULL, NULL);
                    if (op)
                        pa_operation_unref(op);
                    op = pa_context_set_source_port_by_index(self->ctx, self->primary_card->source_id,
                                            self->primary_card->ports->passthru_in->port,
                                            NULL, NULL);
                    if (op)
                        pa_operation_unref(op);


                }
                self->loopback_enabled = TRUE;
                loopback_bt_source_arg = g_strdup_printf("source=%s sink=%s", target_card->source_name, self->primary_card->sink_name); 
                loopback_int_source_arg = g_strdup_printf("source=%s sink=%s", self->primary_card->source_name, target_card->sink_name); 
                g_message("From External to internal: %s", loopback_bt_source_arg);
                g_message("From Internal to external: %s", loopback_int_source_arg);

                op = pa_context_load_module (self->ctx,
                                        "module-loopback",
                                        loopback_bt_source_arg,
                                        NULL,
                                        NULL);
                if (op)
                    pa_operation_unref(op);
                op = pa_context_load_module (self->ctx,
                                        "module-loopback",
                                        loopback_int_source_arg,
                                        NULL,
                                        NULL);
                if (op)
                    pa_operation_unref(op);
            } else {
                op = pa_context_set_default_sink(self->ctx, target_card->sink_name, NULL, NULL);
                if (op)
                    pa_operation_unref(op);

                op = pa_context_set_default_source(self->ctx, target_card->sink_name, NULL, NULL);
                if (op)
                    pa_operation_unref(op);
            }

            break;
        default:
            g_message("Unknown audio mode: %u", audio_mode);
            break;
    }


    g_message("%s finishing", __func__);

    g_object_set(self->manager, "available-devices", cad_pulse_get_available_devices(), NULL);
    return;

}

void cad_pulse_set_output_device(uint32_t device_id, guint device_verb, guint audio_mode, CadOperation *cad_op) {
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    g_critical("%s CALLED: %u %u %u ", __func__, device_id, device_verb, audio_mode);

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_OUTPUT_DEVICE);

    operation->pulse = cad_pulse_get_default();

    if (!operation->pulse->primary_card) {
        g_critical("Primary card not found, can't continue");
        return;
    }

    cad_pulse_set_output(device_id, device_verb, audio_mode);

    g_message("%s finishing", __func__);
    operation_complete_cb(operation->pulse->ctx, 1, operation);

    return;

error:
    g_message("%s oops", __func__);
    if (cad_op) {
        cad_op->success = FALSE;
        if (cad_op->callback)
            cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);

}

void cad_pulse_switch_speaker(gboolean enable, CadOperation *cad_op) {
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;
    AudioCard *target_card = NULL;
    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_ENABLE_SPEAKER);

    operation->pulse = cad_pulse_get_default();
    if (!operation->pulse->primary_card) {
        g_critical("Primary card not found, can't continue");
        return;
    }


    if (operation->pulse->current_active_dev == operation->pulse->primary_card->card_id) {
        g_message("%s Requesting a verb for the same card", __func__);
        target_card = operation->pulse->primary_card;
    } else {
        g_message("%s Requesting output to a different card (%i), looking for it...", __func__, operation->pulse->current_active_dev);
        for (int i = 0; i < operation->pulse->total_external_cards; i++) {
            target_card = g_array_index( operation->pulse->cards, AudioCard *, i );
            if (operation->pulse->current_active_dev == target_card->card_id) {
                g_message("Found it: %s (%s)", target_card->card_description, target_card->card_name);
                break;
            }
        }
    }
    if (!target_card) {
        g_critical("Couldn't find the target card, can't continue");
        return;
    }

    if (target_card->device_type == CAD_PULSE_DEVICE_TYPE_INTERNAL) {
        g_message("Target card SINK ID: %i", target_card->sink_id);
        g_message("Target card SOURCE ID: %i", target_card->source_id);
        if (enable && target_card->ports->speaker->available) {
            g_message("Target card: Speaker (%s)",target_card->ports->speaker->port);
            op = pa_context_set_sink_port_by_index(operation->pulse->ctx, target_card->sink_id,
                                            target_card->ports->speaker->port,
                                            NULL, NULL);
            if (op)
                pa_operation_unref(op);
            operation->pulse->current_active_verb = 2;
        } else  if (target_card->ports->earpiece->available) {
            g_message("Target card: Earpiece (%s)",target_card->ports->earpiece->port);
            op = pa_context_set_sink_port_by_index(operation->pulse->ctx, target_card->sink_id,
                                            target_card->ports->earpiece->port,
                                            NULL, NULL);
            if (op)
                pa_operation_unref(op);
            operation->pulse->current_active_verb = 0;
        } else {
            g_critical("No available target port found for card %i", target_card->card_id);
        }
    }

    operation_complete_cb(operation->pulse->ctx, 1, operation);

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
