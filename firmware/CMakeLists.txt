# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
#
# zigbee2knx firmware: Zigbee coordinator, KNX TP (NCN5130), KNX device server and Berry on one ESP32-C6.
# The Berry VM comes from components/berry, which ESP-IDF picks up by itself.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zigbee2knx)
# -Os makes GCC see type punning in Berry's be_byteslib.c (-Werror=strict-aliasing). The vendored component stays
# as it is; the warning is switched off for it here only.
idf_component_get_property(berry_lib berry COMPONENT_LIB)
target_compile_options(${berry_lib} PRIVATE -Wno-strict-aliasing)
