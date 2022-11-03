/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <gudev/gudev.h>
#include "cad-manager.h"

void udev_init (CadManager *manager);
void udev_destroy (CadManager *manager);
