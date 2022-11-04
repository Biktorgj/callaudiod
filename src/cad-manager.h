/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "callaudio-dbus.h"

#include <glib-object.h>
#include <gudev/gudev.h>

G_BEGIN_DECLS

#define CAD_TYPE_MANAGER (cad_manager_get_type())

typedef struct _CadManager {
    CallAudioDbusCallAudioSkeleton parent;
    GUdevClient *udev;
} CadManager;

G_DECLARE_FINAL_TYPE(CadManager, cad_manager, CAD, MANAGER,
                     CallAudioDbusCallAudioSkeleton);

CadManager *cad_manager_get_default(void);

gboolean scan_bt_devices(CadManager *manager);
G_END_DECLS
