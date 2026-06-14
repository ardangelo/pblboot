/*
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <hal/nrf_power.h>
#include <nrfx_power.h>

extern void tusb_hal_nrf_power_event(uint32_t event);

static void power_event_handler(nrfx_power_usb_evt_t event)
{
	tusb_hal_nrf_power_event((uint32_t)event);
}

void pb_tinyusb_nrf52_init(void)
{
	if (!nrfx_power_init_check()) {
		const nrfx_power_config_t power_config = {0};

		(void)nrfx_power_init(&power_config);
	}

	const nrfx_power_usbevt_config_t usb_event_config = {
		.handler = power_event_handler,
	};

	nrfx_power_usbevt_init(&usb_event_config);
	nrfx_power_usbevt_enable();
}

void pb_tinyusb_nrf52_start(void)
{
	const uint32_t usb_reg = NRF_POWER->USBREGSTATUS;

	if ((usb_reg & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0U) {
		tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_DETECTED);
		tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_READY);
	}
}
