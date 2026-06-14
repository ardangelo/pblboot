/*
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU OPT_MCU_NRF5X
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_DFU 1
#define CFG_TUD_DFU_RUNTIME 0

#define CFG_TUD_DFU_XFER_BUFSIZE 256
#define CFG_TUD_MEM_ALIGN __attribute__((aligned(4)))

#endif /* TUSB_CONFIG_H */
