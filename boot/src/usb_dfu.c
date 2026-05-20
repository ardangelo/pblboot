/*
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_dfu.h"

#include "buttons.h"
#include "watchdog.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usb_dfu.h>
#include <zephyr/usb/usb_device.h>

#include <pb/bootbit.h>

LOG_MODULE_DECLARE(pblboot, CONFIG_PBLBOOT_LOG_LEVEL);

#define SCRATCH_ADDR DT_REG_ADDR(DT_CHOSEN(pb_firmware_scratch))
#define SCRATCH_SIZE DT_REG_SIZE(DT_CHOSEN(pb_firmware_scratch))
#define DFU_ERASE_SIZE 4096U
#define DFU_WRITE_SIZE 4U
#define DFU_TRANSFER_SIZE CONFIG_USB_REQUEST_BUFFER_SIZE
#define DFU_ERASE_SECTOR_COUNT (SCRATCH_SIZE / DFU_ERASE_SIZE)
#define DFU_ERASE_BITMAP_WORDS DIV_ROUND_UP(DFU_ERASE_SECTOR_COUNT, 32U)

BUILD_ASSERT(IS_ENABLED(CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE));
BUILD_ASSERT(IS_POWER_OF_TWO(DFU_ERASE_SIZE));
BUILD_ASSERT(IS_POWER_OF_TWO(DFU_WRITE_SIZE));
BUILD_ASSERT((SCRATCH_SIZE % DFU_ERASE_SIZE) == 0U);
BUILD_ASSERT(DFU_ERASE_SECTOR_COUNT <= 256U);
BUILD_ASSERT((DFU_TRANSFER_SIZE % DFU_WRITE_SIZE) == 0U);

struct usb_dfu_config {
	struct usb_if_descriptor if0;
	struct dfu_runtime_descriptor dfu_descr;
} __packed;

USBD_CLASS_DESCR_DEFINE(primary, 0) struct usb_dfu_config dfu_cfg = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 0,
		.bInterfaceClass = USB_BCC_APPLICATION,
		.bInterfaceSubClass = DFU_SUBCLASS,
		.bInterfaceProtocol = DFU_MODE_PROTOCOL,
		.iInterface = 0,
	},
	.dfu_descr = {
		.bLength = sizeof(struct dfu_runtime_descriptor),
		.bDescriptorType = DFU_FUNC_DESC,
		.bmAttributes = DFU_ATTR_CAN_DNLOAD,
		.wDetachTimeOut = sys_cpu_to_le16(1000U),
		.wTransferSize = sys_cpu_to_le16(DFU_TRANSFER_SIZE),
		.bcdDFUVersion = sys_cpu_to_le16(DFU_VERSION),
	},
};

static const struct device *const external_flash = DEVICE_DT_GET(DT_CHOSEN(pb_external_flash));
static uint32_t erased_sectors[DFU_ERASE_BITMAP_WORDS];
static uint32_t expected_block;
static uint32_t bytes_received;
static enum dfu_state dfu_state = dfuIDLE;
static enum dfu_status dfu_status = statusOK;
static int64_t last_activity_ms;
static bool transfer_active;
static bool manifest_complete;
static bool usb_enabled;
static uint8_t status_response[6];

static void dfu_reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	sys_reboot(SYS_REBOOT_WARM);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, dfu_reboot_work_handler);

static void dfu_interface_config(struct usb_desc_header *head, uint8_t bInterfaceNumber)
{
	ARG_UNUSED(head);

	dfu_cfg.if0.bInterfaceNumber = bInterfaceNumber;
}

static void dfu_state_reset(void)
{
	memset(erased_sectors, 0, sizeof(erased_sectors));
	expected_block = 0U;
	bytes_received = 0U;
	dfu_state = dfuIDLE;
	dfu_status = statusOK;
	transfer_active = false;
	manifest_complete = false;
	last_activity_ms = k_uptime_get();
}

static bool dfu_sector_erased(uint32_t sector)
{
	return (erased_sectors[sector / 32U] & BIT(sector % 32U)) != 0U;
}

static void dfu_sector_mark_erased(uint32_t sector)
{
	erased_sectors[sector / 32U] |= BIT(sector % 32U);
}

static int dfu_erase_for_range(uint32_t offset, uint32_t size)
{
	uint32_t start_sector = offset / DFU_ERASE_SIZE;
	uint32_t end_sector = (offset + size - 1U) / DFU_ERASE_SIZE;
	int ret;

	for (uint32_t sector = start_sector; sector <= end_sector; sector++) {
		if (dfu_sector_erased(sector)) {
			continue;
		}

		ret = flash_erase(external_flash, SCRATCH_ADDR + (sector * DFU_ERASE_SIZE),
				  DFU_ERASE_SIZE);
		if (ret < 0) {
			LOG_ERR("Failed to erase DFU scratch sector %u (err %d)", sector, ret);
			return ret;
		}

		dfu_sector_mark_erased(sector);
		(void)pb_watchdog_feed();
	}

	return 0;
}

static int dfu_flash_write(uint32_t offset, const uint8_t *data, uint32_t size)
{
	uint32_t aligned_size = size & ~(DFU_WRITE_SIZE - 1U);
	int ret;

	if (aligned_size != 0U) {
		ret = flash_write(external_flash, SCRATCH_ADDR + offset, data, aligned_size);
		if (ret < 0) {
			return ret;
		}
	}

	if (aligned_size == size) {
		return 0;
	}

	return flash_write(external_flash, SCRATCH_ADDR + offset + aligned_size, data + aligned_size,
			   size - aligned_size);
}

static int dfu_write_block(uint16_t block, const uint8_t *data, uint32_t size)
{
	uint32_t offset;
	int ret;

	if (size == 0U) {
		if (bytes_received == 0U) {
			return -EINVAL;
		}
		return 0;
	}

	if (block == 0U) {
		dfu_state_reset();
	} else if (block != expected_block) {
		LOG_WRN("Out-of-order DFU block: got %u expected %u", block, expected_block);
		return -EINVAL;
	}

	offset = (uint32_t)block * DFU_TRANSFER_SIZE;
	if ((offset > SCRATCH_SIZE) || (size > (SCRATCH_SIZE - offset))) {
		LOG_WRN("DFU write exceeds scratch partition: block %u len %u", block, size);
		return -EFBIG;
	}

	ret = dfu_erase_for_range(offset, size);
	if (ret < 0) {
		return ret;
	}

	ret = dfu_flash_write(offset, data, size);
	if (ret < 0) {
		LOG_ERR("Failed to write DFU scratch block %u (err %d)", block, ret);
		return ret;
	}

	bytes_received += size;
	expected_block = block + 1U;
	transfer_active = true;
	last_activity_ms = k_uptime_get();
	(void)pb_watchdog_feed();

	return 0;
}

static void dfu_set_error(int err)
{
	if (err == -EFBIG) {
		dfu_status = errADDRESS;
	} else if (err == -EIO) {
		dfu_status = errWRITE;
	} else {
		dfu_status = errUNKNOWN;
	}
	dfu_state = dfuERROR;
}

static void dfu_manifest(void)
{
	LOG_INF("USB DFU received %u bytes; scheduling staged update", bytes_received);
	pb_bootbit_set(PB_BOOTBIT_NEW_FW_AVAILABLE);
	manifest_complete = true;
	dfu_state = dfuMANIFEST_WAIT_RST;
	k_work_schedule(&reboot_work, K_MSEC(100));
}

static int dfu_class_handle_to_host(struct usb_setup_packet *setup, int32_t *len,
				    uint8_t **data)
{
	switch (setup->bRequest) {
	case DFU_GETSTATUS:
		if (dfu_state == dfuDNLOAD_SYNC) {
			dfu_state = dfuDNLOAD_IDLE;
		} else if (dfu_state == dfuMANIFEST_SYNC) {
			dfu_manifest();
		}

		status_response[0] = dfu_status;
		sys_put_le16(CONFIG_PB_USB_DFU_POLLTIMEOUT_MS, &status_response[1]);
		status_response[3] = 0U;
		status_response[4] = dfu_state;
		status_response[5] = 0U;
		*data = status_response;
		*len = sizeof(status_response);
		return 0;
	case DFU_GETSTATE:
		status_response[0] = dfu_state;
		*data = status_response;
		*len = 1;
		return 0;
	case DFU_UPLOAD:
		return -ENOTSUP;
	default:
		return -EINVAL;
	}
}

static int dfu_class_handle_to_device(struct usb_setup_packet *setup, int32_t *len,
				      uint8_t **data)
{
	int ret;

	ARG_UNUSED(len);

	last_activity_ms = k_uptime_get();

	switch (setup->bRequest) {
	case DFU_DNLOAD:
		if (!((dfu_state == dfuIDLE) || (dfu_state == dfuDNLOAD_IDLE))) {
			dfu_set_error(-EINVAL);
			return -EINVAL;
		}

		if (setup->wLength == 0U) {
			if (bytes_received == 0U) {
				dfu_set_error(-EINVAL);
				return -EINVAL;
			}
			dfu_state = dfuMANIFEST_SYNC;
			return 0;
		}

		ret = dfu_write_block(setup->wValue, *data, setup->wLength);
		if (ret < 0) {
			dfu_set_error(ret);
			return ret;
		}

		dfu_state = dfuDNLOAD_SYNC;
		return 0;
	case DFU_ABORT:
		dfu_state_reset();
		return 0;
	case DFU_CLRSTATUS:
		dfu_status = statusOK;
		dfu_state = dfuIDLE;
		return 0;
	case DFU_DETACH:
		return 0;
	default:
		return -EINVAL;
	}
}

static int dfu_class_handle_req(struct usb_setup_packet *setup, int32_t *len,
				uint8_t **data)
{
	if (usb_reqtype_is_to_host(setup)) {
		return dfu_class_handle_to_host(setup, len, data);
	}

	return dfu_class_handle_to_device(setup, len, data);
}

USBD_DEFINE_CFG_DATA(pb_dfu_config) = {
	.usb_device_description = NULL,
	.interface_config = dfu_interface_config,
	.interface_descriptor = &dfu_cfg.if0,
	.cb_usb_status = NULL,
	.interface = {
		.class_handler = dfu_class_handle_req,
	},
	.num_endpoints = 0,
};

int pb_usb_dfu_init(void)
{
	if (!device_is_ready(external_flash)) {
		LOG_ERR("External flash device not ready for USB DFU");
		return -ENODEV;
	}

	return 0;
}

bool pb_usb_dfu_requested(void)
{
	if (pb_bootbit_force_dfu_tst_and_clr()) {
		LOG_INF("Forced USB DFU requested");
		return true;
	}

	return pb_buttons_dfu_requested();
}

int pb_usb_dfu_run(void)
{
	int ret;

	ret = pb_usb_dfu_init();
	if (ret < 0) {
		return ret;
	}

	dfu_state_reset();
	LOG_INF("Entering USB DFU");

	ret = usb_enable(NULL);
	if (ret != 0) {
		return ret;
	}
	usb_enabled = true;

	while (!manifest_complete) {
		int64_t now = k_uptime_get();
		int64_t timeout_ms = transfer_active ? CONFIG_PB_USB_DFU_TRANSFER_TIMEOUT_MS :
						    CONFIG_PB_USB_DFU_IDLE_TIMEOUT_MS;

		if ((now - last_activity_ms) > timeout_ms) {
			LOG_INF("USB DFU timeout; continuing normal boot");
			break;
		}

		(void)pb_watchdog_feed();
		k_sleep(K_MSEC(10));
	}

	if (!manifest_complete && usb_enabled) {
		(void)usb_disable();
		usb_enabled = false;
		return 0;
	}

	while (true) {
		(void)pb_watchdog_feed();
		k_sleep(K_MSEC(100));
	}
}
