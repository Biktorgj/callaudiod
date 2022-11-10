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

typedef struct _CallAudioAsyncData {
    CallAudioCallback cb;
    gpointer user_data;
} CallAudioAsyncData;

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
    if (_initted)
        return TRUE;

    _proxy = call_audio_dbus_call_audio_proxy_new_for_bus_sync(
                                    CALLAUDIO_DBUS_TYPE,
                                    G_DBUS_PROXY_FLAGS_NONE,
                                    CALLAUDIO_DBUS_NAME,
                                    CALLAUDIO_DBUS_PATH, NULL, error);
    if (!_proxy)
        return FALSE;

    g_object_add_weak_pointer(G_OBJECT(_proxy), (gpointer *)&_proxy);

    _initted = TRUE;
    return TRUE;
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

/**
 * call_audio_deinit:
 *
 * Uninitialize the library when no longer used. Usually called
 * on program shutdown.
 */
void call_audio_deinit(void)
{
    _initted = FALSE;
    g_clear_object(&_proxy);
}

static void select_mode_done(GObject *object, GAsyncResult *result, gpointer data)
{
    CallAudioDbusCallAudio *proxy = CALL_AUDIO_DBUS_CALL_AUDIO(object);
    CallAudioAsyncData *async_data = data;
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

    if (async_data && async_data->cb)
        async_data->cb(ret && success, error, async_data->user_data);
    g_free(async_data);
}

/**
 * call_audio_select_mode_async:
 * @mode: Audio mode to select
 * @cb: Function to be called when operation completes
 * @data: User data to be passed to the callback function after completion. This
 *        data is owned by the caller, which is responsible for freeing it.
 *
 * Select the audio mode to use.
 */
gboolean call_audio_select_mode_async(CallAudioMode     mode,
                                      CallAudioCallback cb,
                                      gpointer          data)
{
    CallAudioAsyncData *async_data = g_new0(CallAudioAsyncData, 1);

    if (!_initted || !async_data)
        return FALSE;

    async_data->cb = cb;
    async_data->user_data = data;

    call_audio_dbus_call_audio_call_select_mode(_proxy, mode, NULL,
                                                select_mode_done, async_data);

    return TRUE;
}

/**
 * call_audio_select_mode:
 * @mode: Audio mode to select
 * @error: The error that will be set if the audio mode could not be selected.
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

/**
 * call_audio_get_audio_mode:
 *
 * Returns: The selected #CallAudioMode.
 */
CallAudioMode call_audio_get_audio_mode(void)
{
    if (!_initted)
        return CALL_AUDIO_MODE_UNKNOWN;

    return call_audio_dbus_call_audio_get_audio_mode(_proxy);
}

static void enable_speaker_done(GObject *object, GAsyncResult *result, gpointer data)
{
    CallAudioDbusCallAudio *proxy = CALL_AUDIO_DBUS_CALL_AUDIO(object);
    CallAudioAsyncData *async_data = data;
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

    if (async_data && async_data->cb)
        async_data->cb(ret && success, error, async_data->user_data);
    g_free(async_data);
}

/**
 * call_audio_enable_speaker_async:
 * @enable: Desired speaker state
 * @cb: Function to be called when operation completes
 * @data: User data to be passed to the callback function after completion. This
 *        data is owned by the caller, which is responsible for freeing it.
 *
 * Enable or disable speaker output.
 */
gboolean call_audio_enable_speaker_async(gboolean          enable,
                                         CallAudioCallback cb,
                                         gpointer          data)
{
    CallAudioAsyncData *async_data = g_new0(CallAudioAsyncData, 1);

    if (!_initted || !async_data)
        return FALSE;

    async_data->cb = cb;
    async_data->user_data = data;

    call_audio_dbus_call_audio_call_enable_speaker(_proxy, enable, NULL,
                                                   enable_speaker_done, async_data);

    return TRUE;
}

/**
 * call_audio_get_speaker_state:
 *
 * Returns: %CALL_AUDIO_SPEAKER_ON if the speaker is on, %CALL_AUDIO_SPEAKER_OFF if it is off or
 * %CALL_AUDIO_SPEAKER_UNKNOWN if the state is not known.
 */
CallAudioSpeakerState call_audio_get_speaker_state(void)
{
    if (!_initted)
        return CALL_AUDIO_SPEAKER_UNKNOWN;

    return call_audio_dbus_call_audio_get_speaker_state(_proxy);
}

/**
 * call_audio_enable_speaker:
 * @enable: Desired speaker state
 * @error: The error that will be set if the audio mode could not be selected.
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
    CallAudioAsyncData *async_data = data;
    GError *error = NULL;
    gboolean success = 0;
    gboolean ret;

    g_return_if_fail(CALL_AUDIO_DBUS_IS_CALL_AUDIO(proxy));

    ret = call_audio_dbus_call_audio_call_mute_mic_finish(proxy, &success,
                                                          result, &error);
    if (!ret || !success)
        g_warning("MuteMic failed with code %d: %s", success, error->message);

    g_debug("%s: D-bus call returned %d (success=%d)", __func__, ret, success);

    if (async_data && async_data->cb)
        async_data->cb(ret && success, error, async_data->user_data);
    g_free(async_data);
}

/**
 * call_audio_mute_mic_async:
 * @mute: %TRUE to mute the microphone, or %FALSE to unmute it
 * @cb: Function to be called when operation completes
 * @data: User data to be passed to the callback function after completion. This
 *        data is owned by the caller, which is responsible for freeing it.
 *
 * Mute or unmute microphone.
 */
gboolean call_audio_mute_mic_async(gboolean          mute,
                                   CallAudioCallback cb,
                                   gpointer          data)
{
    CallAudioAsyncData *async_data = g_new0(CallAudioAsyncData, 1);

    if (!_initted || !async_data)
        return FALSE;

    async_data->cb = cb;
    async_data->user_data = data;

    call_audio_dbus_call_audio_call_mute_mic(_proxy, mute, NULL,
                                             mute_mic_done, async_data);

    return TRUE;
}

/**
 * call_audio_mute_mic:
 * @mute: %TRUE to mute the microphone, or %FALSE to unmute it
 * @error: The error that will be set if the audio mode could not be selected.
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

/**
 * call_audio_get_mic_state:
 *
 * Returns: %CALL_AUDIO_MIC_ON if the microphone is on, %CALL_AUDIO_MIC_OFF if it is off or
 * %CALL_AUDIO_MIC_UNKNOWN if the state is not known.
 */
CallAudioMicState call_audio_get_mic_state(void)
{
    if (!_initted)
        return CALL_AUDIO_MIC_UNKNOWN;

    return call_audio_dbus_call_audio_get_mic_state(_proxy);
}

GVariant *call_audio_get_available_devices(void)
{
    return call_audio_dbus_call_audio_get_available_devices(_proxy);
}

/* SET OUTPUT DEVICES */

static void output_device_done(GObject *object, GAsyncResult *result, gpointer data)
{
    CallAudioDbusCallAudio *proxy = CALL_AUDIO_DBUS_CALL_AUDIO(object);
    CallAudioAsyncData *async_data = data;
    GError *error = NULL;
    gboolean ret;
    gboolean success = 0;

    g_return_if_fail(CALL_AUDIO_DBUS_IS_CALL_AUDIO(proxy));

    ret = call_audio_dbus_call_audio_call_output_device_finish(proxy, &success,
                                                          result, &error);
    if (!ret || !success)
        g_warning("Output device set failed with code %d: %s", success, error->message);

    g_debug("%s: D-bus call returned %d (success=%d)", __func__, ret, success);

    if (async_data && async_data->cb)
        async_data->cb(ret && success, error, async_data->user_data);
    g_free(async_data);
}

/**
 * call_audio_switch_bluetooth_audio_async:
 * @mute: %TRUE to mute the microphone, or %FALSE to unmute it
 * @cb: Function to be called when operation completes
 * @data: User data to be passed to the callback function after completion. This
 *        data is owned by the caller, which is responsible for freeing it.
 *
 * Mute or unmute microphone.
 */
gboolean call_audio_output_device_async(guint          id,
                                        guint          verb,
                                        gchar         *name,
                                   CallAudioCallback cb,
                                   gpointer          data)
{
    CallAudioAsyncData *async_data = g_new0(CallAudioAsyncData, 1);

    if (!_initted || !async_data)
        return FALSE;

    async_data->cb = cb;
    async_data->user_data = data;

    call_audio_dbus_call_audio_call_output_device(_proxy, id, verb, name, NULL,
                                             output_device_done, async_data);

    return TRUE;
}

/**
 * call_audio_mute_mic:
 * @mute: %TRUE to mute the microphone, or %FALSE to unmute it
 * @error: The error that will be set if the audio mode could not be selected.
 *
 * Mute or unmute microphone. This function is synchronous, and will return
 * only once the operation has been executed.
 *
 * Returns: %TRUE if successful, or %FALSE on error.
 */
gboolean call_audio_output_device(guint          id,
                                        guint          verb,
                                        gchar         *name,
                                         GError **error)
{
    gboolean success = FALSE;
    gboolean ret;

    if (!_initted)
        return FALSE;

    ret = call_audio_dbus_call_audio_call_output_device_sync(_proxy, id, verb, name, &success,
                                                        NULL, error);
    if (error && *error)
        g_critical("Couldn't switch audio path: %s", (*error)->message);

    g_debug("%s %s: success=%d", ret ? "succeeded" : "failed", __func__, success);

    return (ret && success);
}

/**
 * call_audio_output_device_state:
 *
 * Returns the ID of the active output device in use
 */
CallAudioOutputDeviceOperationState call_audio_output_device_state(void)
{
    if (!_initted)
        return CALL_AUDIO_ACTION_FAILED;

    return call_audio_dbus_call_audio_get_output_device_state(_proxy);
}

/* SET OUTPUT DEVICES */
