#include "fake_flash.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <class/dfu/dfu_device.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "firmware.h"
#include "usb_dfu.h"

static uint8_t image[4096];
static uint32_t image_size;
static uint32_t image_offset;
static bool manifest_sent;
static bool final_status_received;
static int64_t final_status_poll_ms;

static void prepare_image(uint32_t payload_size, bool recovery)
{
	host_fill_image(HOST_SCRATCH_ADDR, payload_size, recovery);
	image_size = payload_size + 12;
	assert(image_size <= sizeof(image));
	memcpy(image, host_external + HOST_SCRATCH_ADDR, image_size);
}

static void download_image(void)
{
	for (uint32_t offset = 0; offset < image_size; offset += HOST_DFU_TRANSFER_SIZE) {
		uint16_t size = MIN(HOST_DFU_TRANSFER_SIZE, image_size - offset);
		host_dispatch_download(offset / HOST_DFU_TRANSFER_SIZE, image + offset, size);
		assert(host_dfu_status == DFU_STATUS_OK);
	}
}

static void test_new_owner_and_block_order(void)
{
	host_reset();
	prepare_image(1037, false);
	pb_bootbit_set(PB_BOOTBIT_NEW_FW_AVAILABLE);
	pb_bootbit_set(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	assert(pb_usb_dfu_init() == 0);
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_PRF_AVAILABLE));

	host_dispatch_download(1, image, HOST_DFU_TRANSFER_SIZE);
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	assert(host_mutation_count == 0);
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_PRF_AVAILABLE));

	/* A new, accepted block zero supersedes both scratch ownership requests. */
	host_dispatch_download(0, image, HOST_DFU_TRANSFER_SIZE);
	assert(host_dfu_status == DFU_STATUS_OK);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_PRF_AVAILABLE));
	assert(host_mutations[0].external && host_mutations[0].op == HOST_FLASH_ERASE);
	assert(host_mutations[0].address == HOST_SCRATCH_ADDR);
	host_dispatch_download(2, image, 12);
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
}

static void test_short_final_and_failed_transfers(void)
{
	host_reset();
	prepare_image(1055, true);
	download_image();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_OK);
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_PRF_AVAILABLE));

	host_reset();
	prepare_image(1055, false);
	host_dispatch_download(0, image, 57);
	assert(host_dfu_status == DFU_STATUS_OK);
	host_dispatch_download(1, image + 57, 50);
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));

	host_reset();
	prepare_image(1055, false);
	host_dispatch_download(0, image, HOST_DFU_TRANSFER_SIZE);
	host_dispatch_abort();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));

	host_reset();
	prepare_image(1055, false);
	host_external_fault.write = -EIO;
	host_external_fault.partial = 28;
	host_dispatch_download(0, image, HOST_DFU_TRANSFER_SIZE);
	assert(host_dfu_status == DFU_STATUS_ERR_WRITE);
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(memcmp(host_external + HOST_SCRATCH_ADDR, image, 28) == 0);
	assert(host_external[HOST_SCRATCH_ADDR + 28] == 0xff);
}

static void test_invalid_manifests(void)
{
	host_reset();
	prepare_image(1055, false);
	image[8] ^= 1;
	download_image();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));

	host_reset();
	prepare_image(1055, false);
	image_size--;
	download_image();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);

	host_reset();
	prepare_image(1055, false);
	image[image_size] = 0xa5;
	image_size++;
	download_image();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);

	host_reset();
	prepare_image(1055, false);
	image[image_size - 2] = 3;
	uint32_t crc = crc32_ieee(image + 12, image_size - 12);
	for (unsigned i = 0; i < 4; i++) image[8 + i] = (uint8_t)(crc >> (i * 8));
	download_image();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);

	host_reset();
	prepare_image(1055, false);
	image[0] = 28; /* Obsolete XIP descriptor is not a staged nRF image. */
	download_image();
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);

	host_reset();
	prepare_image(1055, false);
	download_image();
	host_external_fault.read = -EIO;
	host_external_fault.match_address_enabled = true;
	host_external_fault.match_address = HOST_SCRATCH_ADDR;
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_WRITE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
}

static void test_first_erase_and_capacity_failure(void)
{
	host_reset();
	prepare_image(1055, false);
	pb_bootbit_set(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	host_external_fault.erase = -EIO;
	host_external_fault.partial = 256;
	host_dispatch_download(0, image, HOST_DFU_TRANSFER_SIZE);
	assert(host_dfu_status == DFU_STATUS_ERR_WRITE);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_PRF_AVAILABLE));
	assert(host_external[HOST_SCRATCH_ADDR] == 0xff);
	host_dispatch_manifest();
	assert(host_dfu_status == DFU_STATUS_ERR_FILE);

	host_reset();
	prepare_image(1055, false);
	pb_bootbit_set(PB_BOOTBIT_NEW_PRF_AVAILABLE);
	host_dispatch_download(0, image, HOST_DFU_TRANSFER_SIZE + 1);
	assert(host_dfu_status == DFU_STATUS_ERR_ADDRESS);
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_PRF_AVAILABLE));
	assert(host_mutation_count == 0);

	host_reset();
	uint8_t block[HOST_DFU_TRANSFER_SIZE] = {0};
	for (uint16_t i = 0; i < HOST_INTERNAL_SIZE / HOST_DFU_TRANSFER_SIZE; ++i) {
		host_dispatch_download(i, block, sizeof(block));
		assert(host_dfu_status == DFU_STATUS_OK);
	}
	size_t count = host_mutation_count;
	host_dispatch_download(HOST_INTERNAL_SIZE / HOST_DFU_TRANSFER_SIZE, block, 1);
	assert(host_dfu_status == DFU_STATUS_ERR_ADDRESS);
	assert(host_mutation_count == count);
	assert(host_external[HOST_SCRATCH_ADDR + HOST_INTERNAL_SIZE] == 0xff);
}

static void download_script(void)
{
	if (image_offset < image_size) {
		uint16_t size = MIN(HOST_DFU_TRANSFER_SIZE, image_size - image_offset);
		host_dispatch_download(image_offset / HOST_DFU_TRANSFER_SIZE,
		                       image + image_offset, size);
		assert(host_dfu_status == DFU_STATUS_OK);
		image_offset += size;
	} else if (!manifest_sent) {
		host_dispatch_manifest();
		assert(host_dfu_status == DFU_STATUS_OK);
		manifest_sent = true;
		final_status_poll_ms = host_now_ms + CONFIG_PB_USB_DFU_POLLTIMEOUT_MS + 1000;
	} else if (host_now_ms >= final_status_poll_ms) {
		final_status_received = true;
	}
}

static void test_timeout_policy_and_complete_install(void)
{
	host_reset();
	prepare_image(1055, true);
	assert(pb_usb_dfu_run(true) == 0);
	assert(host_disconnects == 1 && host_tinyusb_inits == 1);
	assert(host_now_ms > CONFIG_PB_USB_DFU_IDLE_TIMEOUT_MS);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));

	host_reset();
	host_task_limit = 2 * CONFIG_PB_USB_DFU_TRANSFER_TIMEOUT_MS;
	int event = setjmp(host_event_jmp);
	if (event == 0) {
		(void)pb_usb_dfu_run(false);
		assert(!"DFU exited without a valid image");
	}
	assert(event == HOST_SCRIPT_END);
	assert(host_disconnects == 0 && host_tinyusb_inits == 1);
	assert(host_now_ms > CONFIG_PB_USB_DFU_TRANSFER_TIMEOUT_MS);

	host_reset();
	host_fill_image(0, 127, true);
	uint8_t safe_image[139];
	memcpy(safe_image, host_external, sizeof(safe_image));
	prepare_image(1055, true);
	image_offset = 0;
	manifest_sent = false;
	final_status_received = false;
	host_dfu_task_hook = download_script;
	event = setjmp(host_event_jmp);
	if (event == 0) {
		(void)pb_usb_dfu_run(false);
		assert(!"manifest must warm reboot");
	}
	assert(event == HOST_REBOOT);
	assert(final_status_received);
	assert(pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(host_dfu_status == DFU_STATUS_OK);
	assert(host_disconnects == 0);
	assert(memcmp(host_external + HOST_SCRATCH_ADDR, image, image_size) == 0);
	assert(host_external[HOST_SAFE_SIZE] == 0xff);

	event = setjmp(host_event_jmp);
	if (event == 0) {
		(void)pb_firmware_load();
		assert(!"firmware installer must jump");
	}
	assert(event == HOST_JUMP && host_jump_address == 0x8000);
	assert(memcmp(host_internal + 0x8000, image + 12, image_size - 12) == 0);
	assert(memcmp(host_external, safe_image, sizeof(safe_image)) == 0);
	for (size_t i = 0; i < 4; ++i) assert(host_external[HOST_BOOT_STATE_ADDR + i] == 0xff);
	for (size_t i = 0; i < host_mutation_count; ++i) {
		const struct host_mutation *mutation = &host_mutations[i];
		if (!mutation->external) {
			assert(mutation->address >= 0x8000);
			assert(mutation->length <= HOST_INTERNAL_SIZE - mutation->address);
		} else {
			bool scratch = mutation->address >= HOST_SCRATCH_ADDR &&
			               mutation->address < HOST_SCRATCH_ADDR + HOST_INTERNAL_SIZE &&
			               mutation->length <= HOST_SCRATCH_ADDR + HOST_INTERNAL_SIZE - mutation->address;
			bool state = mutation->address == HOST_BOOT_STATE_ADDR &&
			             mutation->length <= 4096;
			assert(scratch || state);
		}
	}
	assert(host_external[HOST_SAFE_SIZE] == 0xff);
	assert(host_external[HOST_BOOT_STATE_ADDR + 4096] == 0xff);
	assert(!pb_bootbit_tst(PB_BOOTBIT_NEW_FW_AVAILABLE));
	assert(pb_bootbit_tst(PB_BOOTBIT_RECOVERY_START_IN_PROGRESS));
}

int main(void)
{
	test_new_owner_and_block_order();
	test_short_final_and_failed_transfers();
	test_invalid_manifests();
	test_first_erase_and_capacity_failure();
	test_timeout_policy_and_complete_install();
	puts("DFU production transport tests passed");
	return 0;
}
