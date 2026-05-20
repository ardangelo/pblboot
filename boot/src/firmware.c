/*
 * Copyright (c) 2025 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "firmware.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <pb/bootbit.h>
#include <pb/fwjump.h>

LOG_MODULE_DECLARE(pblboot, CONFIG_PBLBOOT_LOG_LEVEL);

#define PBLBOOT_MAGIC 0x96f3b83dUL

struct firmware_header {
	uint32_t magic;
	uint32_t header_length;
	uint64_t timestamp;
	uint32_t start_offset;
	uint32_t length;
	uint32_t crc;
} __packed;

#ifdef CONFIG_PB_FIRMWARE_NRF52
#define FIRMWARE_ADDR DT_REG_ADDR(DT_CHOSEN(pb_firmware))
#define FIRMWARE_SIZE DT_REG_SIZE(DT_CHOSEN(pb_firmware))

#ifdef CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE
#define FIRMWARE_ERASE_SIZE DT_PROP(DT_PARENT(DT_PARENT(DT_CHOSEN(pb_firmware))), erase_block_size)
#define FIRMWARE_WRITE_SIZE DT_PROP(DT_PARENT(DT_PARENT(DT_CHOSEN(pb_firmware))), write_block_size)

#define SCRATCH_ADDR DT_REG_ADDR(DT_CHOSEN(pb_firmware_scratch))
#define SCRATCH_SIZE DT_REG_SIZE(DT_CHOSEN(pb_firmware_scratch))
#define SAFE_FIRMWARE_ADDR DT_REG_ADDR(DT_CHOSEN(pb_safe_firmware))
#define SAFE_FIRMWARE_SIZE DT_REG_SIZE(DT_CHOSEN(pb_safe_firmware))
#define PRF_ADDR DT_REG_ADDR(DT_CHOSEN(pb_prf))
#define PRF_SIZE DT_REG_SIZE(DT_CHOSEN(pb_prf))

BUILD_ASSERT(IS_POWER_OF_TWO(FIRMWARE_ERASE_SIZE));
BUILD_ASSERT(IS_POWER_OF_TWO(FIRMWARE_WRITE_SIZE));
BUILD_ASSERT((CONFIG_PB_FLASH_READ_BUF_SIZE % FIRMWARE_WRITE_SIZE) == 0);

static uint8_t buf[CONFIG_PB_FLASH_READ_BUF_SIZE];
static const struct device *internal_flash = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
static const struct device *external_flash = DEVICE_DT_GET(DT_CHOSEN(pb_external_flash));

static int staged_read(uint32_t image_addr, uint32_t offset, void *dst, size_t len)
{
	return flash_read(external_flash, image_addr + offset, dst, len);
}

static int internal_read(uint32_t offset, void *dst, size_t len)
{
	return flash_read(internal_flash, offset, dst, len);
}

static int firmware_header_get(uint32_t image_addr, struct firmware_header *hdr)
{
	int ret;

	ret = staged_read(image_addr, 0U, hdr, sizeof(*hdr));
	if (ret < 0) {
		LOG_ERR("Failed to read staged firmware header (err %d)", ret);
		return ret;
	}

	if (hdr->magic != PBLBOOT_MAGIC) {
		LOG_WRN("Invalid staged firmware magic: 0x%08" PRIx32, hdr->magic);
		return -EINVAL;
	}

	if (hdr->header_length != sizeof(*hdr)) {
		LOG_WRN("Invalid staged firmware header length: %" PRIu32, hdr->header_length);
		return -EINVAL;
	}

	return 0;
}

static bool add_overflows_u32(uint32_t a, uint32_t b)
{
	return b > (UINT32_MAX - a);
}

static int firmware_header_validate_bounds(uint32_t image_size, const struct firmware_header *hdr)
{
	if ((hdr->start_offset < hdr->header_length) || (hdr->length == 0U)) {
		return -EINVAL;
	}

	if (hdr->length > FIRMWARE_SIZE) {
		LOG_WRN("Staged firmware too large: %" PRIu32 " > %u", hdr->length, FIRMWARE_SIZE);
		return -EFBIG;
	}

	if (add_overflows_u32(hdr->start_offset, hdr->length) ||
	    ((hdr->start_offset + hdr->length) > image_size)) {
		LOG_WRN("Staged firmware is truncated");
		return -EINVAL;
	}

	return 0;
}

static int firmware_validate_vector_table(uint32_t image_addr, const struct firmware_header *hdr)
{
	uint32_t vectors[2];
	uint32_t initial_sp;
	uint32_t reset_vector;
	int ret;

	ret = staged_read(image_addr, hdr->start_offset, vectors, sizeof(vectors));
	if (ret < 0) {
		return ret;
	}

	initial_sp = vectors[0];
	reset_vector = vectors[1];
	if ((initial_sp < CONFIG_SRAM_BASE_ADDRESS) ||
	    (initial_sp >= (CONFIG_SRAM_BASE_ADDRESS + (CONFIG_SRAM_SIZE * 1024U)))) {
		LOG_WRN("Staged firmware initial SP is outside SRAM: 0x%08" PRIx32, initial_sp);
		return -ENOEXEC;
	}

	if (((reset_vector & BIT(0)) == 0U) || ((reset_vector & ~BIT(0)) < FIRMWARE_ADDR) ||
	    ((reset_vector & ~BIT(0)) >= (FIRMWARE_ADDR + hdr->length))) {
		LOG_WRN("Staged firmware reset vector is incompatible: 0x%08" PRIx32, reset_vector);
		return -ENOEXEC;
	}

	return 0;
}

static int firmware_validate_crc(uint32_t image_addr, const struct firmware_header *hdr)
{
	uint32_t pending;
	uint32_t offset;
	uint32_t crc;
	int ret;

	crc = crc32_ieee(NULL, 0U);
	pending = hdr->length;
	offset = hdr->start_offset;
	while (pending > 0U) {
		size_t len = MIN(sizeof(buf), pending);

		ret = staged_read(image_addr, offset, buf, len);
		if (ret < 0) {
			LOG_ERR("Failed to read staged firmware payload (err %d)", ret);
			return ret;
		}

		crc = crc32_ieee_update(crc, buf, len);
		pending -= len;
		offset += len;
	}

	if (crc != hdr->crc) {
		LOG_WRN("Staged firmware CRC mismatch: got 0x%08" PRIx32 ", expected 0x%08" PRIx32,
			crc, hdr->crc);
		return -EIO;
	}

	return 0;
}

static int firmware_validate_staged(uint32_t image_addr, uint32_t image_size,
					   struct firmware_header *hdr)
{
	int ret;

	ret = firmware_header_get(image_addr, hdr);
	if (ret < 0) {
		return ret;
	}

	ret = firmware_header_validate_bounds(image_size, hdr);
	if (ret < 0) {
		return ret;
	}

	ret = firmware_validate_vector_table(image_addr, hdr);
	if (ret < 0) {
		return ret;
	}

	return firmware_validate_crc(image_addr, hdr);
}

static int firmware_erase_internal(uint32_t length)
{
	uint32_t erase_len = ROUND_UP(length, FIRMWARE_ERASE_SIZE);

	if (erase_len > FIRMWARE_SIZE) {
		return -EFBIG;
	}

	LOG_INF("Erasing internal firmware flash: %" PRIu32 " bytes", erase_len);
	return flash_erase(internal_flash, FIRMWARE_ADDR, erase_len);
}

static int firmware_copy_to_internal(uint32_t image_addr, const struct firmware_header *hdr)
{
	uint32_t pending = hdr->length;
	uint32_t src_offset = hdr->start_offset;
	uint32_t dst_offset = FIRMWARE_ADDR;
	int ret;

	while (pending > 0U) {
		size_t len = MIN(sizeof(buf), pending);
		size_t write_len = ROUND_UP(len, FIRMWARE_WRITE_SIZE);

		ret = staged_read(image_addr, src_offset, buf, len);
		if (ret < 0) {
			LOG_ERR("Failed to read staged firmware payload (err %d)", ret);
			return ret;
		}

		if (write_len != len) {
			memset(&buf[len], 0xff, write_len - len);
		}

		ret = flash_write(internal_flash, dst_offset, buf, write_len);
		if (ret < 0) {
			LOG_ERR("Failed to write internal firmware flash (err %d)", ret);
			return ret;
		}

		pending -= len;
		src_offset += len;
		dst_offset += len;
	}

	return 0;
}

static int firmware_validate_internal_crc(const struct firmware_header *hdr)
{
	uint32_t pending;
	uint32_t offset;
	uint32_t crc;
	int ret;

	crc = crc32_ieee(NULL, 0U);
	pending = hdr->length;
	offset = FIRMWARE_ADDR;
	while (pending > 0U) {
		size_t len = MIN(sizeof(buf), pending);

		ret = internal_read(offset, buf, len);
		if (ret < 0) {
			return ret;
		}

		crc = crc32_ieee_update(crc, buf, len);
		pending -= len;
		offset += len;
	}

	if (crc != hdr->crc) {
		LOG_ERR("Internal firmware CRC mismatch after copy: got 0x%08" PRIx32
			", expected 0x%08" PRIx32,
			crc, hdr->crc);
		return -EIO;
	}

	return 0;
}

static int firmware_install_staged(uint32_t image_addr, uint32_t image_size)
{
	struct firmware_header hdr;
	int ret;

	ret = firmware_validate_staged(image_addr, image_size, &hdr);
	if (ret < 0) {
		return ret;
	}

	pb_bootbit_set(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);

	ret = firmware_erase_internal(hdr.length);
	if (ret < 0) {
		return ret;
	}

	ret = firmware_copy_to_internal(image_addr, &hdr);
	if (ret < 0) {
		return ret;
	}

	ret = firmware_validate_internal_crc(&hdr);
	if (ret < 0) {
		return ret;
	}

	pb_bootbit_clr(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);
	pb_bootbit_clr(PB_BOOTBIT_FW_START_FAIL_STRIKE_ONE);
	pb_bootbit_clr(PB_BOOTBIT_FW_START_FAIL_STRIKE_TWO);
	pb_bootbit_clr(PB_BOOTBIT_RECOVERY_LOAD_FAIL_STRIKE_ONE);
	pb_bootbit_clr(PB_BOOTBIT_RECOVERY_LOAD_FAIL_STRIKE_TWO);
	pb_bootbit_set(PB_BOOTBIT_NEW_FW_INSTALLED);

	return 0;
}

static int firmware_install_if_requested(enum pb_bootbit request_bit, uint32_t image_addr,
						 uint32_t image_size)
{
	int ret;

	if (!pb_bootbit_tst(request_bit)) {
		return 0;
	}

	if (pb_bootbit_tst(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS)) {
		LOG_ERR("Previous firmware update failed before completion; leaving internal flash untouched");
		pb_bootbit_clr(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);
		pb_bootbit_clr(request_bit);
		return 0;
	}

	LOG_INF("Installing staged firmware from external flash @ 0x%08" PRIx32, image_addr);
	ret = firmware_install_staged(image_addr, image_size);
	if (ret < 0) {
		LOG_ERR("Staged firmware install failed before completion (err %d)", ret);
		pb_bootbit_clr(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);
		pb_bootbit_clr(request_bit);
		return ret;
	}

	pb_bootbit_clr(request_bit);
	return 0;
}
#endif /* CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE */
#else
#define SLOT0_ADDR DT_REG_ADDR(DT_CHOSEN(pb_slot0))
#define SLOT1_ADDR DT_REG_ADDR(DT_CHOSEN(pb_slot1))
#define PRF_ADDR   DT_REG_ADDR(DT_CHOSEN(pb_prf))

static uint8_t buf[CONFIG_PB_FLASH_READ_BUF_SIZE];
static const struct device *flash = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

static int firmware_header_get(uint32_t address, struct firmware_header *hdr)
{
	int ret;

	ret = flash_read(flash, address, hdr, sizeof(*hdr));
	if (ret < 0) {
		LOG_ERR("Failed to read from flash (err %d)", ret);
		return ret;
	}

	if ((hdr->magic != PBLBOOT_MAGIC) || (hdr->header_length != sizeof(*hdr))) {
		return -EINVAL;
	}

	return 0;
}

static int firmware_validate(uint32_t address, const struct firmware_header *hdr)
{
	int ret;
	uint32_t pending;
	uint32_t addr;
	uint32_t crc;

	crc = crc32_ieee(NULL, 0U);
	pending = hdr->length;
	addr = address + hdr->start_offset;
	while (pending > 0U) {
		size_t len = MIN(sizeof(buf), pending);
		ret = flash_read(flash, addr, buf, len);
		if (ret < 0) {
			LOG_ERR("Failed to read from flash (err %d)", ret);
			return ret;
		}

		crc = crc32_ieee_update(crc, buf, len);

		pending -= len;
		addr += len;
	}

	if (crc != hdr->crc) {
		return -EIO;
	}

	return 0;
}
#endif /* CONFIG_PB_FIRMWARE_NRF52 */

int pb_firmware_init(void)
{
#ifdef CONFIG_PB_FIRMWARE_NRF52
#ifdef CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE
	if (!device_is_ready(internal_flash)) {
		LOG_ERR("Internal flash device not ready");
		return -ENODEV;
	}

	if (!device_is_ready(external_flash)) {
		LOG_ERR("External flash device not ready");
		return -ENODEV;
	}
#endif /* CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE */
	return 0;
#else
	if (!device_is_ready(flash)) {
		LOG_ERR("Flash device not ready");
		return -ENODEV;
	}

	return 0;
#endif /* CONFIG_PB_FIRMWARE_NRF52 */
}

int pb_firmware_load_prf(void)
{
#ifdef CONFIG_PB_FIRMWARE_NRF52
#ifdef CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE
	int ret;

	ret = firmware_install_staged(SAFE_FIRMWARE_ADDR, SAFE_FIRMWARE_SIZE);
	if (ret < 0) {
		return ret;
	}

	pb_bootbit_prf_starting_set();
	return pb_firmware_load();
#else
	return -ENOTSUP;
#endif /* CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE */
#else
	uint32_t prf_load_address;
	struct firmware_header hdr;
	int ret;

	ret = firmware_header_get(PRF_ADDR, &hdr);
	if (ret < 0) {
		LOG_ERR("PRF not found or invalid (err %d)", ret);
		return ret;
	}

	ret = firmware_validate(PRF_ADDR, &hdr);
	if (ret < 0) {
		LOG_ERR("PRF image is corrupted");
		return ret;
	}

	pb_bootbit_prf_starting_set();

	prf_load_address = CONFIG_FLASH_BASE_ADDRESS + PRF_ADDR + hdr.start_offset;
	LOG_INF("Loading PRF at address 0x%" PRIx32, prf_load_address);
	pb_fwjump(prf_load_address);

	return 0;
#endif /* CONFIG_PB_FIRMWARE_NRF52 */
}

int pb_firmware_load(void)
{
#ifdef CONFIG_PB_FIRMWARE_NRF52
	uint32_t firmware_load_address = CONFIG_FLASH_BASE_ADDRESS + FIRMWARE_ADDR;
#ifdef CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE
	int ret;

	ret = firmware_install_if_requested(PB_BOOTBIT_NEW_PRF_AVAILABLE, PRF_ADDR, PRF_SIZE);
	if (ret < 0) {
		return ret;
	}

	ret = firmware_install_if_requested(PB_BOOTBIT_NEW_FW_AVAILABLE, SCRATCH_ADDR, SCRATCH_SIZE);
	if (ret < 0) {
		return ret;
	}
#endif /* CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE */

	LOG_INF("Loading firmware @ 0x%" PRIx32, firmware_load_address);
	pb_fwjump(firmware_load_address);

	return 0;
#else
	struct firmware_header slot0_hdr;
	struct firmware_header slot1_hdr;
	uint32_t slot0_load_address = 0U;
	uint32_t slot1_load_address = 0U;
	bool slot0_valid;
	bool slot1_valid;
	int ret;

	slot0_valid = false;
	ret = firmware_header_get(SLOT0_ADDR, &slot0_hdr);
	if (ret == 0) {
		ret = firmware_validate(SLOT0_ADDR, &slot0_hdr);
		if (ret == 0) {
			slot0_valid = true;
			slot0_load_address =
				CONFIG_FLASH_BASE_ADDRESS + SLOT0_ADDR + slot0_hdr.start_offset;
			LOG_INF("slot0 firmware valid (0x%" PRIx32 ", %" PRIu64 ")",
				slot0_load_address, slot0_hdr.timestamp);
		}
	}

	slot1_valid = false;
	ret = firmware_header_get(SLOT1_ADDR, &slot1_hdr);
	if (ret == 0) {
		ret = firmware_validate(SLOT1_ADDR, &slot1_hdr);
		if (ret == 0) {
			slot1_valid = true;
			slot1_load_address =
				CONFIG_FLASH_BASE_ADDRESS + SLOT1_ADDR + slot1_hdr.start_offset;
			LOG_INF("slot1 firmware valid (0x%" PRIx32 ", %" PRIu64 ")",
				slot1_load_address, slot1_hdr.timestamp);
		}
	}

	if (slot0_valid) {
		if (slot1_valid && (slot1_hdr.timestamp > slot0_hdr.timestamp)) {
			LOG_INF("Loading slot1 firmware @ 0x%" PRIx32, slot1_load_address);
			pb_fwjump(slot1_load_address);
		} else {
			LOG_INF("Loading slot0 firmware @ 0x%" PRIx32, slot0_load_address);
			pb_fwjump(slot0_load_address);
		}
	} else if (slot1_valid) {
		if (slot0_valid && (slot0_hdr.timestamp > slot1_hdr.timestamp)) {
			LOG_INF("Loading slot0 firmware @ 0x%" PRIx32, slot0_load_address);
			pb_fwjump(slot0_load_address);
		} else {
			LOG_INF("Loading slot1 firmware @ 0x%" PRIx32, slot1_load_address);
			pb_fwjump(slot1_load_address);
		}
	} else {
		LOG_ERR("No valid firmware image");
		return pb_firmware_load_prf();
	}
#endif /* CONFIG_PB_FIRMWARE_NRF52 */
}
