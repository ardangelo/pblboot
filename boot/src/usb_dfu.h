/*
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BOOT_SRC_USB_DFU_H_
#define BOOT_SRC_USB_DFU_H_

#include <stdbool.h>

/**
 * @brief Initialize USB DFU support.
 *
 * @retval 0 on success.
 * @retval -errno negative error code on failure.
 */
int pb_usb_dfu_init(void);

/**
 * @brief Check whether this boot should enter USB DFU.
 *
 * Tests and clears the force-DFU boot bit before checking the END CALL key.
 *
 * @retval true if USB DFU should run.
 * @retval false if normal boot should continue.
 */
bool pb_usb_dfu_requested(void);

/**
 * @brief Run USB DFU until manifest-triggered reset or, when allowed, timeout.
 *
 * @param allow_timeout Allow idle/transfer timeouts only for healthy internal firmware.
 * @retval 0 if DFU timed out and boot may continue.
 * @retval -errno negative error code on setup failure.
 */
int pb_usb_dfu_run(bool allow_timeout);

#endif /* BOOT_SRC_USB_DFU_H_ */
