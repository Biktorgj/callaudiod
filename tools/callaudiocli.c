/*
 * Copyright (C) 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "libcallaudio.h"
#include "libcallaudio-enums.h"

#include <glib.h>

int main (int argc, char *argv[0])
{
    g_autoptr(GOptionContext) opt_context = NULL;
    g_autoptr(GError) err = NULL;
    int mode = -1;
    int mic = -1;
    gboolean status = FALSE;

    const GOptionEntry options [] = {
        {"select-mode", 'm', 0, G_OPTION_ARG_INT, &mode, "Select mode", NULL},
        {"mute-mic", 'u', 0, G_OPTION_ARG_INT, &mic, "Mute microphone", NULL},
        {"status", 'S', 0, G_OPTION_ARG_NONE, &status, "Print status", NULL},
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };

    opt_context = g_option_context_new("- A helper tool for callaudiod");
    g_option_context_add_main_entries(opt_context, options, NULL);
    if (!g_option_context_parse(opt_context, &argc, &argv, &err)) {
        g_warning("%s", err->message);
        return 1;
    }

    if (!call_audio_init(&err)) {
        g_print ("Failed to init libcallaudio: %s\n", err->message);
        return 1;
    }

    /* If there's nothing else to be done, print the current status */
    if (mode == -1 && mic == -1)
        status = TRUE;

    if (mode == CALL_AUDIO_MODE_DEFAULT || mode == CALL_AUDIO_MODE_CALL) {
        if (!call_audio_select_mode(mode, &err)) {
            return 1;
        }
    }

    if (mic == 0 || mic == 1) {
        if (!call_audio_mute_mic((gboolean)mic, &err)) {
            return 1;
        }
    }

    if (status) {
        CallAudioMode audio_mode = call_audio_get_audio_mode();
        CallAudioMicState mic_state = call_audio_get_mic_state();
        const char *string_audio = g_enum_to_string(CALL_TYPE_AUDIO_MODE, audio_mode);
        const char *string_mic = g_enum_to_string(CALL_TYPE_AUDIO_MIC_STATE, mic_state);
        GVariant *available_outputs = call_audio_get_available_devices();
        guint dev_id;
        guint dev_verb;
        gchar *card_name;
        GVariantIter *iter;
        g_print("Selected mode: %s\n"
                "Mic muted: %s\n",
                string_audio, string_mic);
        g_print("Available outputs:\n");
        g_variant_get (available_outputs, "a(uus)", &iter);
        while (g_variant_iter_loop (iter, "(uus)", &dev_id, &dev_verb, &card_name))
        {
        g_print ("%i %s (verb %i)", dev_id, card_name, dev_verb);
        }
    g_variant_iter_free (iter);
    g_print("END\n");
    }

    call_audio_deinit ();
    return 0;
}
