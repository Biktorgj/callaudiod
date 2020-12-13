/*
 * Copyright (C) 2020 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "libcallaudio.h"
#include "callaudiod.h"
#include "callaudio-dbus.h"

/**
 * SECTION:libcallaudio
 * @Short_description: Call audio control library
 * @Title: libcallaudio
 *
 * To use the library call #call_audio_init().
 * After initializing the library you can send audio routing requests using the
 * library functions.
 * When your application finishes call #call_audio_deinit() to free resources:
 *
 * |[<!-- language="C" -->
 *    #include <libcallaudio.h>
 *
 *    int main(void)
 *    {
 *       g_autoptr(GError) *err = NULL;
 *       if (call_audio_init(&err)) {
 *         g_error("%s", err->message);
 *       }
 *       ...
 *       call_audio_deinit();
 *       return 0;
 *    }
 * ]|
 */

static CallAudioDbusCallAudio *_proxy;
static gboolean               _initted;
static guint                  _watch_dbus;

#define N_RETRIES 3

static void dbus_watch_name_appeared(GDBusConnection *connection,
                                     const gchar     *name,
                                     const gchar     *name_owner,
                                     gpointer         user_data);

static void dbus_watch_name_vanished(GDBusConnection *connection,
                                     const gchar     *name,
                                     gpointer         user_data);

static gboolean call_audio_do_init(GError **error)
{
    if (_initted)
        return TRUE;

    _proxy = call_audio_dbus_call_audio_proxy_new_for_bus_sync(
                                                             CALLAUDIO_DBUS_TYPE,0, CALLAUDIO_DBUS_NAME,
                                                             CALLAUDIO_DBUS_PATH, NULL, error);
    if (!_proxy)
        return FALSE;

    g_object_add_weak_pointer(G_OBJECT(_proxy), (gpointer *)&_proxy);

    _initted = TRUE;
    return TRUE;
}

/**
 * call_audio_init:
 * @error: Error information
 *
 * Initialize libcallaudio. This must be called before any other functions.
 *
 * Returns: %TRUE if successful, or %FALSE on error.
 */
gboolean call_audio_init(GError **error)
{
    gboolean success = call_audio_do_init(error);
    _watch_dbus = g_bus_watch_name(CALLAUDIO_DBUS_TYPE, CALLAUDIO_DBUS_NAME,
                                   G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   dbus_watch_name_appeared,
                                   dbus_watch_name_vanished,
                                   NULL, NULL);

    return success;
}

/**
 * call_audio_is_inited:
 *
 * Query whether libcallaudio has been initialized before. This can be used to
 * ensure library calls will perform actual actions.
 *
 * Returns: %TRUE if libcallaudio has been initialized, %FALSE otherwise.
 */
gboolean call_audio_is_inited(void)
{
    return _initted;
}

static void call_audio_reset(void)
{
    _initted = FALSE;
    g_clear_object(&_proxy);
    _proxy = NULL;
}
/**
 * call_audio_deinit:
 *
 * Uninitialize the library when no longer used. Usually called
 * on program shutdown.
 */
void call_audio_deinit(void)
{
    call_audio_reset();
    g_return_if_fail (_watch_dbus == 0);

    g_bus_unwatch_name(_watch_dbus);
    _watch_dbus = 0;
}

static void select_mode_done(GObject *object, GAsyncResult *result, gpointer data)
{
    CallAudioDbusCallAudio *proxy = CALL_AUDIO_DBUS_CALL_AUDIO(object);
    CallAudioCallback cb = data;
    GError *error = NULL;
    gboolean success = FALSE;
    gboolean ret;

    g_return_if_fail(CALL_AUDIO_DBUS_IS_CALL_AUDIO(proxy));

    ret = call_audio_dbus_call_audio_call_select_mode_finish(proxy, &success,
                                                             result, &error);
    if (!ret || !success) {
        g_warning("SelectMode failed with code %d: %s", success, error->message);
    }

    g_debug("%s: D-bus call returned %d (success=%d)", __func__, ret, success);

    if (cb)
        cb(ret && success, error);
}

/**
 * call_audio_select_mode_async:
 * @mode: Audio mode to select
 * @cb: Function to be called when operation completes
 *
 * Select the audio mode to use.
 */
gboolean call_audio_select_mode_async(CallAudioMode mode, CallAudioCallback cb)
{
    if (!_initted)
        return FALSE;

    call_audio_dbus_call_audio_call_select_mode(_proxy, mode, NULL,
                                                select_mode_done, cb);

    return TRUE;
}

/**
 * call_audio_select_mode:
 * @mode: Audio mode to select
 *
 * Select the audio mode to use. This function is synchronous, and will return
 * only once the operation has been executed.
 *
 * Returns: %TRUE if successful, or %FALSE on error.
 */
gboolean call_audio_select_mode(CallAudioMode mode, GError **error)
{
    gboolean success = FALSE;
    gboolean ret;

    if (!_initted)
        return FALSE;

    ret = call_audio_dbus_call_audio_call_select_mode_sync(_proxy, mode, &success,
                                                           NULL, error);
    if (error && *error)
        g_critical("Couldn't set mode %u: %s", mode, (*error)->message);

    g_debug("SelectMode %s: success=%d", ret ? "succeeded" : "failed", success);

    return (ret && success);
}

static void enable_speaker_done(GObject *object, GAsyncResult *result, gpointer data)
{
    CallAudioDbusCallAudio *proxy = CALL_AUDIO_DBUS_CALL_AUDIO(object);
    CallAudioCallback cb = data;
    GError *error = NULL;
    gboolean success = FALSE;
    gboolean ret;

    g_return_if_fail(CALL_AUDIO_DBUS_IS_CALL_AUDIO(proxy));

    ret = call_audio_dbus_call_audio_call_enable_speaker_finish(proxy, &success,
                                                                result, &error);
    if (!ret || !success) {
        g_warning("EnableSpeaker failed with code %d: %s", success, error->message);
    }

    g_debug("%s: D-bus call returned %d (success=%d)", __func__, ret, success);

    if (cb)
        cb(ret && success, error);
}

/**
 * call_audio_enable_speaker_async:
 * @enable: Desired speaker state
 * @cb: Function to be called when operation completes
 *
 * Enable or disable speaker output.
 */
gboolean call_audio_enable_speaker_async(gboolean enable, CallAudioCallback cb)
{
    if (!_initted)
        return FALSE;

    call_audio_dbus_call_audio_call_enable_speaker(_proxy, enable, NULL,
                                                   enable_speaker_done, cb);

    return TRUE;
}

/**
 * call_audio_enable_speaker:
 * @enable: Desired speaker state
 *
 * Enable or disable speaker output. This function is synchronous, and will
 * return only once the operation has been executed.
 *
 * Returns: %TRUE if successful, or %FALSE on error.
 */
gboolean call_audio_enable_speaker(gboolean enable, GError **error)
{
    gboolean success = FALSE;
    gboolean ret;

    if (!_initted)
        return FALSE;

    ret = call_audio_dbus_call_audio_call_enable_speaker_sync(_proxy, enable, &success,
                                                              NULL, error);
    if (error && *error)
        g_critical("Couldn't enable speaker: %s", (*error)->message);

    g_debug("EnableSpeaker %s: success=%d", ret ? "succeeded" : "failed", success);

    return (ret && success);
}

static void mute_mic_done(GObject *object, GAsyncResult *result, gpointer data)
{
    CallAudioDbusCallAudio *proxy = CALL_AUDIO_DBUS_CALL_AUDIO(object);
    CallAudioCallback cb = data;
    GError *error = NULL;
    gboolean success = 0;
    gboolean ret;

    g_return_if_fail(CALL_AUDIO_DBUS_IS_CALL_AUDIO(proxy));

    ret = call_audio_dbus_call_audio_call_mute_mic_finish(proxy, &success,
                                                          result, &error);
    if (!ret || !success)
        g_warning("MuteMic failed with code %d: %s", success, error->message);

    g_debug("%s: D-bus call returned %d (success=%d)", __func__, ret, success);

    if (cb)
        cb(ret && success, error);
}

/**
 * call_audio_mute_mic_async:
 * @mute: %TRUE to mute the microphone, or %FALSE to unmute it
 * @cb: Function to be called when operation completes
 *
 * Mute or unmute microphone.
 */
gboolean call_audio_mute_mic_async(gboolean mute, CallAudioCallback cb)
{
    if (!_initted)
        return FALSE;

    call_audio_dbus_call_audio_call_mute_mic(_proxy, mute, NULL,
                                             mute_mic_done, cb);

    return TRUE;
}

/**
 * call_audio_mute_mic:
 * @mute: %TRUE to mute the microphone, or %FALSE to unmute it
 *
 * Mute or unmute microphone. This function is synchronous, and will return
 * only once the operation has been executed.
 *
 * Returns: %TRUE if successful, or %FALSE on error.
 */
gboolean call_audio_mute_mic(gboolean mute, GError **error)
{
    gboolean success = FALSE;
    gboolean ret;

    if (!_initted)
        return FALSE;

    ret = call_audio_dbus_call_audio_call_mute_mic_sync(_proxy, mute, &success,
                                                        NULL, error);
    if (error && *error)
        g_critical("Couldn't mute mic: %s", (*error)->message);

    g_debug("MuteMic %s: success=%d", ret ? "succeeded" : "failed", success);

    return (ret && success);
}

/* DBus watching */
static void dbus_watch_name_appeared(GDBusConnection *connection,
                                     const gchar     *name,
                                     const gchar     *name_owner,
                                     gpointer         user_data)
{
    g_autoptr (GError) error = NULL;

    g_assert (!_initted);
    g_assert(!_proxy);

    call_audio_do_init(&error);
}

static void dbus_watch_name_vanished(GDBusConnection *connection,
                                     const gchar     *name,
                                     gpointer         user_data)
{
    g_assert(_initted);
    g_assert(_proxy);

    call_audio_reset();
    /* TODO try to reactivate callaudiod somehow (for N_RETRIES)*/
}
