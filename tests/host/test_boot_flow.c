#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fake_flash.h"
#include "firmware.h"

extern int pb_test_boot_main(void);

static void put_u32(uint8_t *where, uint32_t value)
{
	memcpy(where, &value, sizeof(value));
}

static void healthy_internal(void)
{
	put_u32(host_internal + 0x8000, 0x20040000U);
	put_u32(host_internal + 0x8004, 0x00008011U);
}

static int boot_once(void)
{
	int event = setjmp(host_event_jmp);
	if (!event) (void)pb_test_boot_main();
	return event;
}

static void assert_safe_installed(uint32_t length)
{
	assert(host_jump_address == 0x8000U);
	assert(memcmp(host_internal + 0x8000, host_external + 12, length) == 0);
	assert(host_bootbits & BIT(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS));
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS)));
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE)));
	assert(*(uint32_t *)(void *)(host_external + HOST_BOOT_STATE_ADDR) == UINT32_MAX);
}

static void test_force_prf_outranks_scratch(void)
{
	host_reset();
	healthy_internal();
	host_fill_image(0, 201, true);
	host_fill_image(HOST_SCRATCH_ADDR, 301, false);
	host_bootbits |= BIT(PB_BOOTBIT_FORCE_PRF) | BIT(PB_BOOTBIT_NEW_FW_AVAILABLE) |
			 BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	assert(boot_once() == HOST_JUMP);
	assert_safe_installed(201);
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE));
	assert(!(host_bootbits & BIT(PB_BOOTBIT_FORCE_PRF)));
}

static void test_dirty_pending_retries_scratch(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, 1031, false);
	host_fill_image(0, 201, true);
	put_u32(host_external + HOST_BOOT_STATE_ADDR, 0);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	assert(boot_once() == HOST_JUMP);
	assert(memcmp(host_internal + 0x8000, host_external + HOST_SCRATCH_ADDR + 12, 1031) == 0);
	assert(!(host_bootbits & BIT(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS)));
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE)));
}

static void test_retained_loss_uses_safe_before_strikes(void)
{
	host_reset();
	host_fill_image(0, 201, true);
	healthy_internal();
	put_u32(host_external + HOST_BOOT_STATE_ADDR, 0);
	/* Losing retained RAM also loses request and strike state. */
	host_bootbits = 0;
	assert(boot_once() == HOST_JUMP);
	assert_safe_installed(201);

	host_reset();
	host_fill_image(0, 201, true);
	host_bootbits |= BIT(PB_BOOTBIT_RESET_LOOP_DETECT_ONE) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_TWO) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_THREE);
	assert(boot_once() == HOST_JUMP);
	assert_safe_installed(201);
}

static void test_normal_strike_policy(void)
{
	host_reset();
	healthy_internal();
	assert(boot_once() == HOST_JUMP);
	assert(pb_bootbit_reset_loop_cnt_get() == 1);
	host_reset();
	healthy_internal();
	host_bootbits |= BIT(PB_BOOTBIT_RESET_LOOP_DETECT_ONE) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_TWO) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_THREE);
	assert(boot_once() == HOST_PANIC);
	assert(host_jump_address == 0);
}

static void test_invalid_scratch_falls_back_safely(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, 201, false);
	host_fill_image(0, 207, true);
	host_external[HOST_SCRATCH_ADDR + 12 + 70] ^= 1;
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE) | BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	assert(boot_once() == HOST_JUMP);
	assert_safe_installed(207);
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE));
}

static void test_invalid_safe_enters_nonexpiring_dfu(void)
{
	host_reset();
	host_task_limit = 300;
	assert(boot_once() == HOST_SCRIPT_END);
	assert(host_jump_address == 0);
	assert(host_tinyusb_inits == 1);
	assert(host_now_ms >= 300);

	host_reset();
	healthy_internal();
	host_external_fault.read = -EIO;
	host_task_limit = 300;
	assert(boot_once() == HOST_SCRIPT_END);
	assert(host_jump_address == 0);
	assert(host_tinyusb_inits == 1);
}

static void test_explicit_dfu_timeout_only_when_healthy(void)
{
	host_reset();
	healthy_internal();
	host_bootbits |= BIT(PB_BOOTBIT_FORCE_DFU);
	assert(boot_once() == HOST_JUMP);
	assert(host_now_ms >= CONFIG_PB_USB_DFU_IDLE_TIMEOUT_MS);
	assert(host_jump_address == 0x8000U);
	assert(!(host_bootbits & BIT(PB_BOOTBIT_FORCE_DFU)));
	assert(host_tinyusb_inits == 1);

	host_reset();
	host_bootbits |= BIT(PB_BOOTBIT_FORCE_DFU);
	host_task_limit = 300;
	assert(boot_once() == HOST_SCRIPT_END);
	assert(host_now_ms >= 300);
	assert(host_jump_address == 0);
}

int main(void)
{
	test_force_prf_outranks_scratch();
	test_dirty_pending_retries_scratch();
	test_retained_loss_uses_safe_before_strikes();
	test_normal_strike_policy();
	test_invalid_scratch_falls_back_safely();
	test_invalid_safe_enters_nonexpiring_dfu();
	test_explicit_dfu_timeout_only_when_healthy();
	puts("boot-flow production cases passed");
	return 0;
}
