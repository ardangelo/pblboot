/*
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <pb/bootbit.h>

#include <nrfx.h>

LOG_MODULE_REGISTER(pb_bootbit, CONFIG_PB_BOOTBIT_LOG_LEVEL);

#define NRF_RETAINED_BASE_ADDRESS 0x20000000UL
#define NRF_RETAINED_WORDS        (256U / sizeof(uint32_t))

#define RTC_BKP_BOOTBIT_DR          0U
#define NRF_RETAINED_REGISTER_CRC   31U
#define PB_BOOTBIT_INITIALIZED_MASK  BIT(PB_BOOTBIT_INITIALIZED)

static uint32_t *const retained = (uint32_t *)NRF_RETAINED_BASE_ADDRESS;

static const char *bootbit_str(enum pb_bootbit bit)
{
	switch (bit) {
	case PB_BOOTBIT_INITIALIZED:
		return "INITIALIZED";
	case PB_BOOTBIT_NEW_FW_AVAILABLE:
		return "NEW_FW_AVAILABLE";
	case PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS:
		return "NEW_FW_UPDATE_IN_PROGRESS";
	case PB_BOOTBIT_FW_START_FAIL_STRIKE_ONE:
		return "FW_START_FAIL_STRIKE_ONE";
	case PB_BOOTBIT_FW_START_FAIL_STRIKE_TWO:
		return "FW_START_FAIL_STRIKE_TWO";
	case PB_BOOTBIT_RECOVERY_LOAD_FAIL_STRIKE_ONE:
		return "RECOVERY_LOAD_FAIL_STRIKE_ONE";
	case PB_BOOTBIT_RECOVERY_LOAD_FAIL_STRIKE_TWO:
		return "RECOVERY_LOAD_FAIL_STRIKE_TWO";
	case PB_BOOTBIT_RECOVERY_START_IN_PROGRESS:
		return "RECOVERY_START_IN_PROGRESS";
	case PB_BOOTBIT_SOFTWARE_FAILURE_OCCURRED:
		return "SOFTWARE_FAILURE_OCCURRED";
	case PB_BOOTBIT_RESET_LOOP_DETECT_ONE:
		return "RESET_LOOP_DETECT_ONE";
	case PB_BOOTBIT_RESET_LOOP_DETECT_TWO:
		return "RESET_LOOP_DETECT_TWO";
	case PB_BOOTBIT_RESET_LOOP_DETECT_THREE:
		return "RESET_LOOP_DETECT_THREE";
	case PB_BOOTBIT_FW_STABLE:
		return "FW_STABLE";
	case PB_BOOTBIT_NEW_FW_INSTALLED:
		return "NEW_FW_INSTALLED";
	case PB_BOOTBIT_FORCE_PRF:
		return "FORCE_PRF";
	case PB_BOOTBIT_NEW_PRF_AVAILABLE:
		return "NEW_PRF_AVAILABLE";
	case PB_BOOTBIT_FORCE_DFU:
		return "FORCE_DFU";
	default:
		return "UNKNOWN";
	}
}

static uint32_t retained_crc(void)
{
	return crc32_ieee((const uint8_t *)retained,
			    NRF_RETAINED_REGISTER_CRC * sizeof(retained[0]));
}

static void retained_crc_update(void)
{
	retained[NRF_RETAINED_REGISTER_CRC] = retained_crc();
}

static void retained_write(uint8_t id, uint32_t value)
{
	retained[id] = value;
	retained_crc_update();
}

static uint32_t retained_read(uint8_t id)
{
	return retained[id];
}

static void retained_clear(void)
{
	memset(retained, 0, NRF_RETAINED_WORDS * sizeof(retained[0]));
}

static void retained_enable_power(void)
{
#if defined(POWER_RAM_POWER_S0RETENTION_Msk)
	NRF_POWER->RAM[0].POWERSET = POWER_RAM_POWER_S0RETENTION_Msk;
#elif defined(POWER_RAM_POWER_S2RETENTION_Msk)
	NRF_POWER->RAM[0].POWERSET = POWER_RAM_POWER_S2RETENTION_Msk;
#endif
}

void pb_bootbit_init(void)
{
	uint32_t crc;

	retained_enable_power();

	crc = retained_crc();
	if (crc != retained[NRF_RETAINED_REGISTER_CRC]) {
		LOG_WRN("Retained register CRC failed: expected 0x%08" PRIx32
			", got 0x%08" PRIx32 "; clearing boot bits",
			crc, retained[NRF_RETAINED_REGISTER_CRC]);
		retained_clear();
	}

	if ((retained_read(RTC_BKP_BOOTBIT_DR) & PB_BOOTBIT_INITIALIZED_MASK) == 0U) {
		LOG_DBG("Boot bits not initialized, initializing now");
		retained_write(RTC_BKP_BOOTBIT_DR, PB_BOOTBIT_INITIALIZED_MASK);
	}

	LOG_DBG("Boot bits: 0x%08" PRIx32, retained_read(RTC_BKP_BOOTBIT_DR));
}

void pb_bootbit_set(enum pb_bootbit bit)
{
	uint32_t current_value = retained_read(RTC_BKP_BOOTBIT_DR);

	current_value |= BIT(bit);
	retained_write(RTC_BKP_BOOTBIT_DR, current_value);
	LOG_DBG("Set boot bit %d (%s)", bit, bootbit_str(bit));
}

void pb_bootbit_clr(enum pb_bootbit bit)
{
	uint32_t current_value = retained_read(RTC_BKP_BOOTBIT_DR);

	current_value &= ~BIT(bit);
	retained_write(RTC_BKP_BOOTBIT_DR, current_value);
	LOG_DBG("Cleared boot bit %d (%s)", bit, bootbit_str(bit));
}

bool pb_bootbit_tst(enum pb_bootbit bit)
{
	bool ret = (retained_read(RTC_BKP_BOOTBIT_DR) & BIT(bit)) != 0U;

	LOG_DBG("Tested boot bit %d (%s): %u", bit, bootbit_str(bit), ret);

	return ret;
}
