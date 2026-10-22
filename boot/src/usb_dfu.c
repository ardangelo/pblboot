/*
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_dfu.h"

#include "buttons.h"
#include "firmware.h"
#include "watchdog.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <tusb.h>
#include <class/dfu/dfu_device.h>
#include <device/dcd.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <pb/bootbit.h>

LOG_MODULE_DECLARE(pblboot, CONFIG_PBLBOOT_LOG_LEVEL);

#define SCRATCH_ADDR DT_REG_ADDR(DT_CHOSEN(pb_firmware_scratch))
#define SCRATCH_SIZE DT_REG_SIZE(DT_CHOSEN(pb_firmware_scratch))
#define DFU_ERASE_SIZE 4096U
#define DFU_WRITE_SIZE 4U
#define DFU_TRANSFER_SIZE CFG_TUD_DFU_XFER_BUFSIZE
#define DFU_ERASE_SECTOR_COUNT (SCRATCH_SIZE / DFU_ERASE_SIZE)
#define DFU_ERASE_BITMAP_WORDS DIV_ROUND_UP(DFU_ERASE_SECTOR_COUNT, 32U)
#define DFU_ITF_NUM 0U
#define DFU_ALT_COUNT 1
#define DFU_STR_MANUFACTURER 1U
#define DFU_STR_PRODUCT 2U
#define DFU_STR_SERIAL 3U
#define DFU_STR_INTERFACE 4U
#define DFU_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(DFU_ALT_COUNT))

BUILD_ASSERT(IS_ENABLED(CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE));
BUILD_ASSERT(IS_ENABLED(CONFIG_PB_USB_DFU));
BUILD_ASSERT(IS_POWER_OF_TWO(DFU_ERASE_SIZE));
BUILD_ASSERT(IS_POWER_OF_TWO(DFU_WRITE_SIZE));
BUILD_ASSERT((SCRATCH_SIZE % DFU_ERASE_SIZE) == 0U);
BUILD_ASSERT(DFU_ERASE_SECTOR_COUNT <= 256U);
BUILD_ASSERT((DFU_TRANSFER_SIZE % DFU_WRITE_SIZE) == 0U);

extern void pb_tinyusb_nrf52_init(void);
extern void pb_tinyusb_nrf52_start(void);

static const struct device *const external_flash = DEVICE_DT_GET(DT_CHOSEN(pb_external_flash));
static uint32_t erased_sectors[DFU_ERASE_BITMAP_WORDS];
static uint32_t expected_block;
static uint32_t bytes_received;
static int64_t last_activity_ms;
static bool transfer_active;
static bool manifest_complete;
static bool short_block_received;
static bool transfer_failed;

static const tusb_desc_device_t desc_device = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0x00,
	.bDeviceSubClass = 0x00,
	.bDeviceProtocol = 0x00,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = 0xcafe,
	.idProduct = 0x4002,
	.bcdDevice = 0x0100,
	.iManufacturer = DFU_STR_MANUFACTURER,
	.iProduct = DFU_STR_PRODUCT,
	.iSerialNumber = DFU_STR_SERIAL,
	.bNumConfigurations = 0x01,
};

static const uint8_t desc_configuration[] = {
	TUD_CONFIG_DESCRIPTOR(1, 1, 0, DFU_CONFIG_TOTAL_LEN, 0x00, 100),
	TUD_DFU_DESCRIPTOR(DFU_ITF_NUM, DFU_ALT_COUNT, DFU_STR_INTERFACE,
			   DFU_ATTR_CAN_DOWNLOAD, 1000, DFU_TRANSFER_SIZE),
};

static const char *const string_desc_arr[] = {
	(const char[]){0x09, 0x04},
	"Core Devices",
	"Cyberdeck Bootloader DFU",
	"123456",
	"Cyberdeck DFU",
};

static uint16_t desc_str[32];

uint8_t const *tud_descriptor_device_cb(void)
{
	return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	ARG_UNUSED(index);

	return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	uint8_t chr_count;

	ARG_UNUSED(langid);

	if (index == 0U) {
		memcpy(&desc_str[1], string_desc_arr[0], 2U);
		chr_count = 1U;
	} else {
		const char *str;

		if (index >= ARRAY_SIZE(string_desc_arr)) {
			return NULL;
		}

		str = string_desc_arr[index];
		chr_count = (uint8_t)strlen(str);
		if (chr_count > 31U) {
			chr_count = 31U;
		}

		for (uint8_t i = 0U; i < chr_count; i++) {
			desc_str[1 + i] = str[i];
		}
	}

	desc_str[0] = (TUSB_DESC_STRING << 8) | ((2U * chr_count) + 2U);

	return desc_str;
}

ISR_DIRECT_DECLARE(pb_usb_dfu_isr)
{
	tud_int_handler(0);

	return 1;
}

static void dfu_irq_connect(void)
{
	IRQ_DIRECT_CONNECT(USBD_IRQn, 2, pb_usb_dfu_isr, 0);
}

static void dfu_state_reset(void)
{
	memset(erased_sectors, 0, sizeof(erased_sectors));
	expected_block = 0U;
	bytes_received = 0U;
	transfer_active = false;
	manifest_complete = false;
	short_block_received = false;
	transfer_failed = false;
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

	if (block == 0U && size != 0U) {
		dfu_state_reset();
	}
	if (transfer_failed || block != expected_block) {
		LOG_WRN("Out-of-order DFU block: got %u expected %u", block, expected_block);
		ret = -EINVAL;
		goto fail;
	}
	if (size == 0U) {
		if (bytes_received == 0U) {
			ret = -EINVAL;
			goto fail;
		}
		return 0;
	}
	if (short_block_received) {
		ret = -EINVAL;
		goto fail;
	}
	if (size > DFU_TRANSFER_SIZE) {
		ret = -EFBIG;
		goto fail;
	}

	offset = (uint32_t)block * DFU_TRANSFER_SIZE;
	if (offset != bytes_received) {
		ret = -EINVAL;
		goto fail;
	}
	if (offset > SCRATCH_SIZE || size > SCRATCH_SIZE - offset) {
		LOG_WRN("DFU write exceeds scratch partition: block %u len %u", block, size);
		ret = -EFBIG;
		goto fail;
	}

	if (block == 0U) {
		/* Scratch has a new owner only when its first erase is accepted. */
		pb_bootbit_clr(PB_BOOTBIT_NEW_FW_AVAILABLE);
		pb_bootbit_clr(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	}
	ret = dfu_erase_for_range(offset, size);
	if (ret < 0) {
		goto fail;
	}

	ret = dfu_flash_write(offset, data, size);
	if (ret < 0) {
		LOG_ERR("Failed to write DFU scratch block %u (err %d)", block, ret);
		goto fail;
	}

	bytes_received += size;
	expected_block = block + 1U;
	short_block_received = size < DFU_TRANSFER_SIZE;
	transfer_active = true;
	last_activity_ms = k_uptime_get();
	(void)pb_watchdog_feed();
	return 0;

fail:
	transfer_failed = true;
	return ret;
}

static uint8_t dfu_status_from_error(int err)
{
	if (err == -EFBIG) {
		return DFU_STATUS_ERR_ADDRESS;
	}
	if (err == -EINVAL || err == -ENOEXEC || err == -EBADMSG) {
		return DFU_STATUS_ERR_FILE;
	}
	if (err == -EIO) {
		return DFU_STATUS_ERR_WRITE;
	}

	return DFU_STATUS_ERR_UNKNOWN;
}

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

int pb_usb_dfu_run(bool allow_timeout)
{
	const tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO,
	};
	int ret;

	ret = pb_usb_dfu_init();
	if (ret < 0) {
		return ret;
	}

	dfu_state_reset();
	LOG_INF("Entering USB DFU");

	dfu_irq_connect();
	pb_tinyusb_nrf52_init();
	if (!tusb_init(0, &dev_init)) {
		return -EIO;
	}
	irq_enable(USBD_IRQn);
	pb_tinyusb_nrf52_start();

	while (!manifest_complete) {
		tud_task();
		int64_t now = k_uptime_get();
		int64_t timeout_ms = transfer_active ? CONFIG_PB_USB_DFU_TRANSFER_TIMEOUT_MS :
						    CONFIG_PB_USB_DFU_IDLE_TIMEOUT_MS;

		if (allow_timeout && (now - last_activity_ms) > timeout_ms) {
			LOG_INF("USB DFU timeout; continuing normal boot");
			break;
		}

		(void)pb_watchdog_feed();
		k_sleep(K_MSEC(1));
	}

	if (!manifest_complete) {
		dcd_disconnect(0);
		dcd_int_disable(0);
		return 0;
	}

	/* dfu-util waits another second before its final status poll and USB reset. */
	const int64_t reboot_at = k_uptime_get() + CONFIG_PB_USB_DFU_POLLTIMEOUT_MS + 2000;
	while (k_uptime_get() < reboot_at) {
		tud_task();
		(void)pb_watchdog_feed();
		k_sleep(K_MSEC(1));
	}
	sys_reboot(SYS_REBOOT_WARM);

	return 0;
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const *data, uint16_t length)
{
	int ret;

	ARG_UNUSED(alt);

	ret = dfu_write_block(block_num, data, length);
	if (ret < 0) {
		tud_dfu_finish_flashing(dfu_status_from_error(ret));
		return;
	}

	tud_dfu_finish_flashing(DFU_STATUS_OK);
}

void tud_dfu_manifest_cb(uint8_t alt)
{
	int ret;

	ARG_UNUSED(alt);

	if (transfer_failed || bytes_received == 0U) {
		tud_dfu_finish_flashing(DFU_STATUS_ERR_FILE);
		return;
	}

	ret = pb_firmware_validate_download(bytes_received);
	if (ret < 0) {
		transfer_failed = true;
		tud_dfu_finish_flashing(dfu_status_from_error(ret));
		return;
	}

	LOG_INF("USB DFU received %u bytes; scheduling staged update", bytes_received);
	pb_bootbit_set(PB_BOOTBIT_NEW_FW_AVAILABLE);
	manifest_complete = true;
	tud_dfu_finish_flashing(DFU_STATUS_OK);
}

uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num, uint8_t *data, uint16_t length)
{
	ARG_UNUSED(alt);
	ARG_UNUSED(block_num);
	ARG_UNUSED(data);
	ARG_UNUSED(length);

	return 0;
}

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
	ARG_UNUSED(alt);
	ARG_UNUSED(state);

	return CONFIG_PB_USB_DFU_POLLTIMEOUT_MS;
}

void tud_dfu_detach_cb(void)
{
}

void tud_dfu_abort_cb(uint8_t alt)
{
	ARG_UNUSED(alt);
	dfu_state_reset();
}
