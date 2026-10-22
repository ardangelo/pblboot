#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/crc.h>

#include "fake_flash.h"
#include "firmware.h"

#define APP_ADDR 0x8000U
#define APP_SIZE 0xf8000U
#define IMAGE_SIZE 4099U

static void put_u32(uint8_t *where, uint32_t value)
{
	memcpy(where, &value, sizeof(value));
}

static void refresh_crc(uint32_t address, uint32_t length)
{
	put_u32(host_external + address + 8,
		crc32_ieee(host_external + address + 12, length));
}

static void assert_no_internal_mutation(void)
{
	for (size_t i = 0; i < host_mutation_count; ++i) {
		assert(host_mutations[i].external);
	}
}

static void assert_only_installer_mutations(void)
{
	for (size_t i = 0; i < host_mutation_count; ++i) {
		const struct host_mutation *m = &host_mutations[i];
		if (m->external) {
			assert(m->address == HOST_BOOT_STATE_ADDR);
			assert(m->length <= 0x1000);
		} else {
			assert(m->address >= APP_ADDR);
			assert(m->length <= HOST_INTERNAL_SIZE - m->address);
		}
	}
}

static void test_description_and_identity(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == 0);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 11) == -EINVAL);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 13) == -EINVAL);

	put_u32(host_external + HOST_SCRATCH_ADDR, 28);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -EINVAL);
	put_u32(host_external + HOST_SCRATCH_ADDR, 12);
	put_u32(host_external + HOST_SCRATCH_ADDR + 4, 74);
	assert(pb_firmware_validate_download(86) == -EINVAL);
	put_u32(host_external + HOST_SCRATCH_ADDR + 4, APP_SIZE + 1);
	assert(pb_firmware_validate_download(APP_SIZE + 13) == -EFBIG);
	put_u32(host_external + HOST_SCRATCH_ADDR + 4, IMAGE_SIZE);
	uint8_t *payload = host_external + HOST_SCRATCH_ADDR + 12;
	put_u32(payload, 0x200000f8U);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -ENOEXEC);
	put_u32(payload, 0x20040000U);
	put_u32(payload + 4, APP_ADDR + IMAGE_SIZE + 1);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -ENOEXEC);
	put_u32(payload + 4, APP_ADDR + 0x11);
	payload[IMAGE_SIZE - 3] = 2;
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -ENOEXEC);
	payload[IMAGE_SIZE - 3] = 1;
	payload[IMAGE_SIZE - 2] = 0;
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -ENOEXEC);
	payload[IMAGE_SIZE - 2] = 22;
	payload[IMAGE_SIZE - 1] = 2;
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -ENOEXEC);
	payload[IMAGE_SIZE - 1] = 1;
	refresh_crc(HOST_SCRATCH_ADDR, IMAGE_SIZE);
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == 0);
	payload[IMAGE_SIZE / 2] ^= 1;
	assert(pb_firmware_validate_download(IMAGE_SIZE + 12) == -EBADMSG);
	assert_no_internal_mutation();
}

static void test_internal_health(void)
{
	host_reset();
	assert(pb_firmware_check_internal() == -ENOEXEC);
	put_u32(host_internal + APP_ADDR, 0x20040000U);
	put_u32(host_internal + APP_ADDR + 4, APP_ADDR + APP_SIZE - 1);
	assert(pb_firmware_check_internal() == 0);
	put_u32(host_internal + APP_ADDR, 0x20040004U);
	assert(pb_firmware_check_internal() == -ENOEXEC);
	put_u32(host_internal + APP_ADDR, 0x20040000U);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS);
	assert(pb_firmware_check_internal() == -EINPROGRESS);
	host_bootbits = 0;
	put_u32(host_external + HOST_BOOT_STATE_ADDR, 0xfffffffeU);
	assert(pb_firmware_check_internal() == -EINPROGRESS);
	host_external_fault.read = -EIO;
	assert(pb_firmware_check_internal() == -EIO);
}

static void test_exact_install(uint32_t length, bool recovery)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, length, recovery);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE) | BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE) |
			 BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS) |
			 BIT(PB_BOOTBIT_FW_START_FAIL_STRIKE_ONE) |
			 BIT(PB_BOOTBIT_FW_START_FAIL_STRIKE_TWO) |
			 BIT(PB_BOOTBIT_RECOVERY_LOAD_FAIL_STRIKE_ONE) |
			 BIT(PB_BOOTBIT_RECOVERY_LOAD_FAIL_STRIKE_TWO) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_ONE) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_TWO) |
			 BIT(PB_BOOTBIT_RESET_LOOP_DETECT_THREE);
	int event = setjmp(host_event_jmp);
	if (!event) (void)pb_firmware_load();
	assert(event == HOST_JUMP);
	assert(host_jump_address == APP_ADDR);
	assert(memcmp(host_internal + APP_ADDR, host_external + HOST_SCRATCH_ADDR + 12,
		      length) == 0);
	if (length < APP_SIZE) assert(host_internal[APP_ADDR + length] == 0xff);
	assert(*(uint32_t *)(void *)(host_external + HOST_BOOT_STATE_ADDR) == UINT32_MAX);
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE)));
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS)));
	assert(!!(host_bootbits & BIT(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS)) == recovery);
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE));
	assert(pb_bootbit_fw_fail_cnt_get() == 0);
	assert(pb_bootbit_prf_fail_cnt_get() == 0);
	assert(pb_bootbit_reset_loop_cnt_get() == 0);
	assert(pb_firmware_check_internal() == 0);
	assert_only_installer_mutations();
}

static void test_preerase_rejection(void)
{
	host_reset();
	put_u32(host_internal + APP_ADDR, 0x20040000U);
	put_u32(host_internal + APP_ADDR + 4, APP_ADDR + 0x11);
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_external[HOST_SCRATCH_ADDR + 12 + IMAGE_SIZE - 1] ^= 1;
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	int event = setjmp(host_event_jmp);
	if (!event) (void)pb_firmware_load();
	assert(event == HOST_JUMP && host_jump_address == APP_ADDR);
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE)));
	assert_no_internal_mutation();
}

static void test_marker_failure(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_external_fault.write = -EIO;
	assert(pb_firmware_load() == -EIO);
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert_no_internal_mutation();

	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_external_fault.write = -EIO;
	host_external_fault.partial = 2;
	assert(pb_firmware_load() == -EIO);
	assert(pb_firmware_check_internal() == -EINPROGRESS);
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS)));
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert_no_internal_mutation();
}

static void test_marker_readback_and_cleanup_failure(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_external_fault.read = -EIO;
	host_external_fault.match_address_enabled = true;
	host_external_fault.match_address = HOST_BOOT_STATE_ADDR;
	assert(pb_firmware_load() == -EIO);
	assert_no_internal_mutation();
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE));

	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_external_fault.erase = -EIO;
	host_external_fault.match_address_enabled = true;
	host_external_fault.match_address = HOST_BOOT_STATE_ADDR;
	/* After full payload verification, a failed marker cleanup cannot jump. */
	assert(pb_firmware_load() == -EIO);
	assert(host_jump_address == 0);
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS));
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert_only_installer_mutations();

	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_external_fault.erase = -EIO;
	host_external_fault.partial = 4;
	host_external_fault.match_address_enabled = true;
	host_external_fault.match_address = HOST_BOOT_STATE_ADDR;
	assert(pb_firmware_load() == -EIO);
	assert(*(uint32_t *)(void *)(host_external + HOST_BOOT_STATE_ADDR) == UINT32_MAX);
	assert(pb_firmware_check_internal() == -EINPROGRESS);
	assert(host_jump_address == 0);
}

static void test_partial_copy_stays_dirty_without_safe(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_internal_fault.write = -EIO;
	host_internal_fault.partial = 16;
	assert(pb_firmware_load() == -EINVAL);
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_UPDATE_IN_PROGRESS));
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(pb_firmware_check_internal() == -EINPROGRESS);
	assert(host_jump_address == 0);
	assert_only_installer_mutations();
}

static void test_corrupt_internal_readback_uses_safe(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_fill_image(0, IMAGE_SIZE + 120, true);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE);
	host_corrupt_internal_write_once = true;
	int event = setjmp(host_event_jmp);
	if (!event) (void)pb_firmware_load();
	assert(event == HOST_JUMP && host_jump_address == APP_ADDR);
	assert(memcmp(host_internal + APP_ADDR, host_external + 12, IMAGE_SIZE + 120) == 0);
	assert(host_bootbits & BIT(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS));
	assert(pb_firmware_check_internal() == 0);
	assert_only_installer_mutations();
}

static void test_recovery_after_interrupted_copy(void)
{
	host_reset();
	host_fill_image(HOST_SCRATCH_ADDR, IMAGE_SIZE, false);
	host_fill_image(0, IMAGE_SIZE + 120, true);
	host_bootbits |= BIT(PB_BOOTBIT_NEW_FW_AVAILABLE) | BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	host_internal_fault.erase = -EIO;
	int event = setjmp(host_event_jmp);
	if (!event) (void)pb_firmware_load();
	assert(event == HOST_JUMP && host_jump_address == APP_ADDR);
	assert(memcmp(host_internal + APP_ADDR, host_external + 12, IMAGE_SIZE + 120) == 0);
	assert(!(host_bootbits & BIT(PB_BOOTBIT_NEW_FW_AVAILABLE)));
	assert(host_bootbits & BIT(PB_BOOTBIT_NEW_PRF_AVAILABLE));
	assert(host_bootbits & BIT(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS));
	assert_only_installer_mutations();
}

static void test_cold_dirty_and_unsafe_source(void)
{
	host_reset();
	host_fill_image(0, IMAGE_SIZE, true);
	put_u32(host_external + HOST_BOOT_STATE_ADDR, 0);
	put_u32(host_internal + APP_ADDR, 0x20040000U);
	put_u32(host_internal + APP_ADDR + 4, APP_ADDR + 0x11);
	int event = setjmp(host_event_jmp);
	if (!event) (void)pb_firmware_load();
	assert(event == HOST_JUMP && host_jump_address == APP_ADDR);
	assert(memcmp(host_internal + APP_ADDR, host_external + 12, IMAGE_SIZE) == 0);

	host_reset();
	host_fill_image(0, IMAGE_SIZE, false);
	assert(pb_firmware_load_prf() == -ENOEXEC);
	assert_no_internal_mutation();
}

int main(void)
{
	test_description_and_identity();
	test_internal_health();
	test_exact_install(75, false);
	test_exact_install(IMAGE_SIZE, true);
	test_exact_install(APP_SIZE, false);
	test_preerase_rejection();
	test_marker_failure();
	test_marker_readback_and_cleanup_failure();
	test_partial_copy_stays_dirty_without_safe();
	test_corrupt_internal_readback_uses_safe();
	test_recovery_after_interrupted_copy();
	test_cold_dirty_and_unsafe_source();
	puts("firmware production cases passed");
	return 0;
}
