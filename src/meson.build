#
# Copyright (C) 2019 Purism SPC
# Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

cad_deps = [
    dependency('alsa'),
    dependency('gobject-2.0'),
    dependency('gio-unix-2.0'),
    dependency('libpulse'),
    dependency('libpulse-mainloop-glib'),
]

executable (
    'callaudiod',
    config_h,
    generated_dbus_sources,
    libcallaudio_enum_sources,
    [
        'callaudiod.c', 'callaudiod.h',
        'cad-manager.c', 'cad-manager.h',
        'cad-pulse.c', 'cad-pulse.h',
    ],
    dependencies : cad_deps,
    include_directories : include_directories('..', '../libcallaudio'),
    install : true
)
