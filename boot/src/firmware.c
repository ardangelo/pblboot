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

#ifndef CONFIG_PB_FIRMWARE_NRF52
#define PBLBOOT_MAGIC 0x96f3b83dUL

struct firmware_header {
	uint32_t magic;
	uint32_t header_length;
	uint64_t timestamp;
	uint32_t start_offset;
	uint32_t length;
	uint32_t crc;
} __packed;
#endif
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
#define BOOT_STATE_ADDR DT_REG_ADDR(DT_CHOSEN(pb_boot_state))
#define BOOT_STATE_SIZE DT_REG_SIZE(DT_CHOSEN(pb_boot_state))
#define SRAM_ADDR DT_REG_ADDR(DT_NODELABEL(sram0))
#define SRAM_SIZE DT_REG_SIZE(DT_NODELABEL(sram0))

#define FIRMWARE_FOOTER_SIZE 67U
#define FIRMWARE_MIN_SIZE (8U + FIRMWARE_FOOTER_SIZE)
#define FIRMWARE_METADATA_VERSION 1U
#define FIRMWARE_RECOVERY_FLAG 1U

struct firmware_description {
	uint32_t description_length;
	uint32_t firmware_length;
	uint32_t checksum;
} __packed;

struct staged_image {
	struct firmware_description description;
	bool is_recovery;
};

BUILD_ASSERT(sizeof(struct firmware_description) == 12);
BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
BUILD_ASSERT(CONFIG_PB_FIRMWARE_PLATFORM_ID != 0);
BUILD_ASSERT(IS_POWER_OF_TWO(FIRMWARE_ERASE_SIZE));
BUILD_ASSERT(IS_POWER_OF_TWO(FIRMWARE_WRITE_SIZE));
BUILD_ASSERT((CONFIG_PB_FLASH_READ_BUF_SIZE % FIRMWARE_WRITE_SIZE) == 0);
BUILD_ASSERT(FIRMWARE_ADDR == 0x8000 && FIRMWARE_SIZE == 0xf8000);

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

static bool firmware_vectors_valid(const uint32_t vectors[2], uint32_t payload_size)
{
	uint32_t sp = vectors[0];
	uint32_t reset = vectors[1];

	return (sp >= SRAM_ADDR + 0x100U) && (sp <= SRAM_ADDR + SRAM_SIZE) &&
	       ((sp & 7U) == 0U) && ((reset & 1U) != 0U) &&
	       ((reset & ~1U) >= FIRMWARE_ADDR) &&
	       ((reset & ~1U) - FIRMWARE_ADDR < payload_size);
}

static int firmware_validate_crc(uint32_t image_addr,
				 const struct firmware_description *description)
{
	uint32_t pending = description->firmware_length;
	uint32_t offset = sizeof(*description);
	uint32_t crc = crc32_ieee(NULL, 0U);
	int ret;

	while (pending > 0U) {
		size_t len = MIN(sizeof(buf), pending);

		ret = staged_read(image_addr, offset, buf, len);
		if (ret < 0) {
			return ret;
		}
		crc = crc32_ieee_update(crc, buf, len);
		pending -= len;
		offset += len;
	}

	return crc == description->checksum ? 0 : -EBADMSG;
}

static int firmware_validate_staged(uint32_t image_addr, uint32_t image_size,
				    bool require_recovery, struct staged_image *image)
{
	struct firmware_description *description = &image->description;
	uint32_t vectors[2];
	uint8_t footer[3];
	uint32_t length;
	int ret;

	if (image_size < sizeof(*description)) {
		return -EINVAL;
	}
	ret = staged_read(image_addr, 0U, description, sizeof(*description));
	if (ret < 0) {
		return ret;
	}
	if (description->description_length != sizeof(*description) ||
	    description->firmware_length < FIRMWARE_MIN_SIZE) {
		return -EINVAL;
	}
	length = description->firmware_length;
	if (length > FIRMWARE_SIZE || length > image_size - sizeof(*description) ||
	    ROUND_UP(length, FIRMWARE_WRITE_SIZE) > FIRMWARE_SIZE ||
	    ROUND_UP(length, FIRMWARE_ERASE_SIZE) > FIRMWARE_SIZE) {
		return -EFBIG;
	}

	ret = staged_read(image_addr, sizeof(*description), vectors, sizeof(vectors));
	if (ret < 0) {
		return ret;
	}
	if (!firmware_vectors_valid(vectors, length)) {
		return -ENOEXEC;
	}

	ret = staged_read(image_addr, sizeof(*description) + length - sizeof(footer),
			  footer, sizeof(footer));
	if (ret < 0) {
		return ret;
	}
	if ((footer[0] & ~FIRMWARE_RECOVERY_FLAG) != 0U ||
	    footer[1] != CONFIG_PB_FIRMWARE_PLATFORM_ID ||
	    footer[2] != FIRMWARE_METADATA_VERSION ||
	    (require_recovery && footer[0] != FIRMWARE_RECOVERY_FLAG)) {
		return -ENOEXEC;
	}
	image->is_recovery = footer[0] == FIRMWARE_RECOVERY_FLAG;
	return firmware_validate_crc(image_addr, description);
}

int pb_firmware_validate_download(uint32_t received_length)
{
	struct staged_image image;
	struct firmware_description description;
	int ret;

	if (received_length < sizeof(description)) {
		return -EINVAL;
	}
	if (received_length > SCRATCH_SIZE) {
		return -EFBIG;
	}
	ret = staged_read(SCRATCH_ADDR, 0U, &description, sizeof(description));
	if (ret < 0) {
		return ret;
	}
	if (description.description_length == sizeof(description) &&
	    description.firmware_length <= SCRATCH_SIZE - sizeof(description) &&
	    received_length != sizeof(description) + description.firmware_length) {
		return -EINVAL;
	}
	return firmware_validate_staged(SCRATCH_ADDR, SCRATCH_SIZE, false, &image);
}

static int firmware_state_read(uint32_t *word)
{
	return flash_read(external_flash, BOOT_STATE_ADDR, word, sizeof(*word));
}

int pb_firmware_check_internal(void)
{
	uint32_t word;
	uint32_t vectors[2];
	int ret = firmware_state_read(&word);

	if (ret < 0) {
		return ret;
	}
	if (word != UINT32_MAX || pb_bootbit_tst(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS)) {
		return -EINPROGRESS;
	}
	ret = internal_read(FIRMWARE_ADDR, vectors, sizeof(vectors));
	if (ret < 0) {
		return ret;
	}
	return firmware_vectors_valid(vectors, FIRMWARE_SIZE) ? 0 : -ENOEXEC;
}

static int firmware_mark_dirty(void)
{
	uint32_t word = 0U;
	int ret = flash_write(external_flash, BOOT_STATE_ADDR, &word, sizeof(word));

	if (ret < 0) {
		return ret;
	}
	ret = firmware_state_read(&word);
	if (ret < 0) {
		return ret;
	}
	if (word != 0U) {
		return -EIO;
	}
	pb_bootbit_set(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);
	return 0;
}

static int firmware_erase_internal(uint32_t length)
{
	return flash_erase(internal_flash, FIRMWARE_ADDR, ROUND_UP(length, FIRMWARE_ERASE_SIZE));
}

static int firmware_copy_to_internal(uint32_t image_addr,
				     const struct firmware_description *description)
{
	uint32_t pending = description->firmware_length;
	uint32_t src_offset = sizeof(*description);
	uint32_t dst_offset = FIRMWARE_ADDR;
	int ret;

	while (pending > 0U) {
		size_t len = MIN(sizeof(buf), pending);
		size_t write_len = ROUND_UP(len, FIRMWARE_WRITE_SIZE);

		ret = staged_read(image_addr, src_offset, buf, len);
		if (ret < 0) {
			return ret;
		}
		if (write_len != len) {
			memset(&buf[len], 0xff, write_len - len);
		}
		ret = flash_write(internal_flash, dst_offset, buf, write_len);
		if (ret < 0) {
			return ret;
		}
		pending -= len;
		src_offset += len;
		dst_offset += write_len;
	}
	return 0;
}

static int firmware_validate_internal_crc(const struct firmware_description *description)
{
	uint32_t pending = description->firmware_length;
	uint32_t offset = FIRMWARE_ADDR;
	uint32_t crc = crc32_ieee(NULL, 0U);
	int ret;

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
	return crc == description->checksum ? 0 : -EIO;
}

static int firmware_install_staged(uint32_t image_addr, const struct staged_image *image,
				   bool *marked, bool *verified)
{
	uint32_t word;
	int ret = firmware_mark_dirty();

	if (ret < 0) {
		return ret;
	}
	if (marked != NULL) {
		*marked = true;
	}
	ret = firmware_erase_internal(image->description.firmware_length);
	if (ret < 0) {
		return ret;
	}
	ret = firmware_copy_to_internal(image_addr, &image->description);
	if (ret < 0) {
		return ret;
	}
	ret = firmware_validate_internal_crc(&image->description);
	if (ret < 0) {
		return ret;
	}
	if (verified != NULL) {
		*verified = true;
	}
	ret = flash_erase(external_flash, BOOT_STATE_ADDR, BOOT_STATE_SIZE);
	if (ret < 0) {
		return ret;
	}
	ret = firmware_state_read(&word);
	if (ret < 0) {
		return ret;
	}
	if (word != UINT32_MAX) {
		return -EIO;
	}
	pb_bootbit_clr(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);
	pb_bootbit_fw_fail_cnt_set(0U);
	pb_bootbit_prf_fail_cnt_set(0U);
	pb_bootbit_reset_loop_cnt_set(0U);
	pb_bootbit_set(PB_BOOTBIT_NEW_FW_INSTALLED);
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
	struct staged_image image;
	int ret = firmware_validate_staged(SAFE_FIRMWARE_ADDR, SAFE_FIRMWARE_SIZE, true, &image);

	if (ret < 0) {
		return ret;
	}
	ret = firmware_install_staged(SAFE_FIRMWARE_ADDR, &image, NULL, NULL);
	if (ret < 0) {
		return ret;
	}
	pb_bootbit_clr(PB_BOOTBIT_NEW_FW_AVAILABLE);
	pb_bootbit_prf_starting_set();
	pb_fwjump(CONFIG_FLASH_BASE_ADDRESS + FIRMWARE_ADDR);
	return 0;
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
	struct staged_image image;
	bool marked = false;
	bool verified = false;
	int ret;

	if (pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE)) {
		ret = firmware_validate_staged(SCRATCH_ADDR, SCRATCH_SIZE, false, &image);
		if (ret < 0) {
			if (ret != -EINVAL && ret != -EFBIG && ret != -ENOEXEC && ret != -EBADMSG) {
				return ret;
			}
			pb_bootbit_clr(PB_BOOTBIT_NEW_FW_AVAILABLE);
			ret = pb_firmware_check_internal();
			if (ret == -EINPROGRESS || ret == -ENOEXEC) {
				return pb_firmware_load_prf();
			}
			if (ret < 0) {
				return ret;
			}
		} else {
			ret = firmware_install_staged(SCRATCH_ADDR, &image, &marked, &verified);
			if (ret < 0) {
				if (verified) {
					return ret;
				}
				return marked ? pb_firmware_load_prf() : ret;
			}
			pb_bootbit_clr(PB_BOOTBIT_NEW_FW_AVAILABLE);
			if (image.is_recovery) {
				pb_bootbit_prf_starting_set();
			} else {
				pb_bootbit_clr(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS);
			}
		}
	} else {
		ret = pb_firmware_check_internal();
		if (ret == -EINPROGRESS || ret == -ENOEXEC) {
			return pb_firmware_load_prf();
		}
		if (ret < 0) {
			return ret;
		}
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
