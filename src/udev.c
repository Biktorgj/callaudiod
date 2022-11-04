/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "udev.h"
#include "cad-manager.h"
#include <string.h>

static void udev_event_cb(GUdevClient *client, gchar *action, GUdevDevice *device, gpointer data)
{   
    CadManager *manager = data;
    /* Give it a second so pulseaudio has a chance of knowing it */
    if (strcmp(action, "add") == 0) {
        /*
         * Modem is probably executing a FW upgrade, make sure we don't interrupt it
         */
        g_message("Bluetooth device added: %s:",g_udev_device_get_name(device));
    } else if (strcmp(action, "removed") != 0) {
        g_message("Bluetooth device removed: %s", g_udev_device_get_name(device));
    } else {
        g_message("Bluetooth device change, unknown action %s", action);
    }
    /* We wait for a few seconds to give time to PA to populate the card */
    g_timeout_add_seconds(5, G_SOURCE_FUNC(scan_bt_devices), manager);
    return;
}

void udev_init (CadManager *manager)
{
    const char * const subsystems[] = { "bluetooth", NULL };

    manager->udev = g_udev_client_new(subsystems);
    g_signal_connect(manager->udev, "uevent", G_CALLBACK(udev_event_cb), manager);

    return;
}

void udev_destroy (CadManager *manager)
{
    if (manager->udev) {
        g_object_unref(manager->udev);
        manager->udev = NULL;
    }
}
