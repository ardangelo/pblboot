/*
 * Copyright (c) 2025 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BOOT_SRC_FIRMWARE_H_
#define BOOT_SRC_FIRMWARE_H_

/**
 * @brief Initialize the firmware module
 *
 * @retval 0 on success
 * @retval -errno negative error code on failure
 */
int pb_firmware_init(void);

#ifdef CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE
#include <stdint.h>

/** Check the persistent marker, retained update bit, and internal image vectors. */
int pb_firmware_check_internal(void);

/** Validate a complete scratch download without changing flash or boot bits. */
int pb_firmware_validate_download(uint32_t received_length);
#endif

/**
 * @brief Load the PRF firmware
 *
 * @retval 0 on success
 * @retval -errno negative error code on failure
 */
int pb_firmware_load_prf(void);

/**
 * @brief Install a pending staged image (on NRF52), then boot internal firmware.
 *        Invalid input falls back to the existing internal image or SAFE.
 *
 * @retval 0 on success
 * @retval -errno negative error code on failure
 */
int pb_firmware_load(void);

#endif /* BOOT_SRC_FIRMWARE_H_ */
