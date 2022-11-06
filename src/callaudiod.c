/*
 * Copyright (C) 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "callaudiod"

#include "callaudiod.h"
#include "cad-manager.h"
#include "cad-pulse.h"
#include "config.h"

#include <glib.h>
#include <glib-unix.h>

static GMainLoop *main_loop = NULL;

static gboolean quit_cb(gpointer user_data)
{
    g_info("Caught signal, shutting down...");

    if (main_loop)
        g_idle_add((GSourceFunc)g_main_loop_quit, main_loop);
    else
        exit(0);

    return FALSE;
}


static void bus_acquired_cb(GDBusConnection *connection, const gchar *name,
                            gpointer user_data)
{
    CadManager *manager = cad_manager_get_default();

    g_debug("Bus acquired, creating manager...");

    g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(manager),
                                     connection, CALLAUDIO_DBUS_PATH, NULL);
}


static void name_acquired_cb(GDBusConnection *connection, const gchar *name,
                             gpointer user_data)
{
    g_debug("Service name '%s' was acquired", name);
}

static void name_lost_cb(GDBusConnection *connection, const gchar *name,
                         gpointer user_data)
{
    if (!name) {
      g_warning("Could not get the session bus. Make sure "
                "the message bus daemon is running!");
    } else {
        if (connection)
            g_warning("Could not acquire the '%s' service name", name);
        else
            g_debug("DBus connection close");
    }

    g_main_loop_quit(main_loop);
}

int main(int argc, char **argv)
{
    g_unix_signal_add(SIGTERM, quit_cb, NULL);
    g_unix_signal_add(SIGINT, quit_cb, NULL);

    main_loop = g_main_loop_new(NULL, FALSE);

    g_message("**** Callaudiod 0.1.4 Start ****");
    // Initialize the PulseAudio backend
    cad_pulse_get_default();
    g_bus_own_name(CALLAUDIO_DBUS_TYPE, CALLAUDIO_DBUS_NAME,
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   bus_acquired_cb, name_acquired_cb, name_lost_cb,
                   NULL, NULL);

    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);

    return 0;
}
