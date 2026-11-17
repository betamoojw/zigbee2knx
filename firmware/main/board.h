// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// What differs between the TUL32 and the TULX32: one source tree, two builds. The TULX32 build is chosen by
// CONFIG_Z2K_BOARD_TULX32 (sdkconfig.tulx). Plain numbers only, so that the host tests (test/host) can include this
// without the ESP-IDF drivers.
//
// Pins (TUL32: the board's bring-up firmware; TULX32: the TULX15 schematic):
//   KNX TX -> NCN5130 RXD GPIO4, KNX RX <- NCN5130 TXD GPIO5, on UART1; LED (blue, active low) GPIO8 on both.
//   Button S1: GPIO9 (BOOT) on the TUL32, GPIO14 (net "OTA", pull-up R7) on the TULX32.
//   Console: USB-Serial-JTAG on the TUL32, UART0 on the TULX32 (service header J1, CP2102N adapter).
// Storage: the TUL32 carries this firmware's own partition table (partitions.csv). The table of the TULX32 as shipped
// is frozen at delivery (partitions_tulx.csv, the recovery system lives in factory): there is neither zb_storage nor
// devsrv, only nvs (20 KiB, shared with the recovery system) and actor_db (32 KiB) of type NVS, and storage (128 KiB)
// as LittleFS. This firmware uses them as follows:
//   - the device records and their templates go to actor_db: 32 devices of four classes took 645 NVS entries
//     (measured 2026-11-14), actor_db has about 880 usable; a fifth class would need the templates in LittleFS
//   - Zigbee shares nvs with the recovery system. Nothing in this firmware may erase nvs there: neither on an NVS
//     error at the start nor through "zbreset" (esp_zigbee_factory_reset() "clears all the datasets", its header does
//     not say whether that is more than its own keys).
#pragma once
#include "sdkconfig.h"

#define BOARD_KNX_UART    1
#define BOARD_KNX_TX_PIN  4
#define BOARD_KNX_RX_PIN  5
#define BOARD_LED_PIN     8   // active low

#if defined(CONFIG_Z2K_BOARD_TULX32) && CONFIG_Z2K_BOARD_TULX32
#define BOARD_NAME            "TULX32"
#define BOARD_BTN_PIN         14              // S1, active low
#define BOARD_DEVSRV_PART     "actor_db"
#define BOARD_ZB_PART         "nvs"
#define BOARD_NVS_ERASABLE    0               // nvs holds the recovery system's settings: never erase it
#else
#define BOARD_NAME            "TUL32"
#define BOARD_BTN_PIN         9               // S1 (BOOT), active low
#define BOARD_DEVSRV_PART     "devsrv"
#define BOARD_ZB_PART         "zb_storage"
#define BOARD_NVS_ERASABLE    1
#endif
