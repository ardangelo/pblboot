#include "fake_flash.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>

#include <class/dfu/dfu_device.h>
#include <device/dcd.h>
#include <tusb.h>

#include "buttons.h"
#include "panic.h"
#include "watchdog.h"
#include <pb/fwjump.h>

uint8_t host_internal[HOST_INTERNAL_SIZE];
uint8_t host_external[HOST_EXTERNAL_SIZE];
uint32_t host_bootbits;
struct device host_internal_device = { .external = false, .ready = true };
struct device host_external_device = { .external = true, .ready = true };
struct host_flash_fault host_internal_fault;
struct host_flash_fault host_external_fault;
struct host_mutation host_mutations[HOST_MAX_MUTATIONS];
size_t host_mutation_count;
int64_t host_now_ms;
int host_watchdog_feeds;
int host_tinyusb_inits;
int host_disconnects;
int host_dfu_status;
bool host_dfu_button;
bool host_prf_button;
bool host_tusb_init_ok;
bool host_corrupt_internal_write_once;
void (*host_dfu_task_hook)(void);
jmp_buf host_event_jmp;
enum host_event host_last_event;
uintptr_t host_jump_address;
uint32_t host_panic_reason;
unsigned host_task_count;
unsigned host_task_limit;

static void record_mutation(const struct device *device, enum host_flash_op op,
                            uint32_t offset, size_t size)
{
	assert(host_mutation_count < HOST_MAX_MUTATIONS);
	host_mutations[host_mutation_count++] = (struct host_mutation){
		.external = device->external, .op = op, .address = offset, .length = size,
	};
}

static uint8_t *flash_space(const struct device *device, uint32_t offset, size_t size)
{
	size_t capacity = device->external ? HOST_EXTERNAL_SIZE : HOST_INTERNAL_SIZE;
	if (offset > capacity || size > capacity - offset) {
		return NULL;
	}
	return (device->external ? host_external : host_internal) + offset;
}

static struct host_flash_fault *flash_fault(const struct device *device)
{
	return device->external ? &host_external_fault : &host_internal_fault;
}

int flash_read(const struct device *device, uint32_t offset, void *data, size_t size)
{
	uint8_t *source = flash_space(device, offset, size);
	struct host_flash_fault *fault = flash_fault(device);
	if (!source) return -EINVAL;
	if (fault->read && (!fault->match_address_enabled || fault->match_address == offset)) {
		int err = fault->read;
		fault->read = 0;
		return err;
	}
	memcpy(data, source, size);
	return 0;
}

int flash_write(const struct device *device, uint32_t offset, const void *data, size_t size)
{
	uint8_t *destination = flash_space(device, offset, size);
	const uint8_t *source = data;
	struct host_flash_fault *fault = flash_fault(device);
	if (!destination || !size || (offset & 3U) ||
	    (device->external ? (size > 3U && (size & 3U)) : (size & 3U))) return -EINVAL;
	int err = fault->write && (!fault->match_address_enabled ||
	                           fault->match_address == offset) ? fault->write : 0;
	size_t n = err ? MIN(size, fault->partial) : size;
	if (err) {
		fault->write = 0;
		fault->partial = 0;
	}
	for (size_t i = 0; i < n; i++) {
		if ((destination[i] & source[i]) != source[i]) return -EIO;
	}
	for (size_t i = 0; i < n; i++) destination[i] &= source[i];
	if (n) record_mutation(device, HOST_FLASH_WRITE, offset, n);
	if (!err && !device->external && host_corrupt_internal_write_once) {
		for (size_t i = MIN(n, 8U); i < n; ++i) {
			if (destination[i] != 0U) {
				destination[i] &= destination[i] - 1U;
				host_corrupt_internal_write_once = false;
				break;
			}
		}
	}
	return err;
}

int flash_erase(const struct device *device, uint32_t offset, size_t size)
{
	uint8_t *destination = flash_space(device, offset, size);
	struct host_flash_fault *fault = flash_fault(device);
	if (!destination || !size || (offset & 4095U) || (size & 4095U)) return -EINVAL;
	int err = fault->erase && (!fault->match_address_enabled ||
	                           fault->match_address == offset) ? fault->erase : 0;
	size_t n = err ? MIN(size, fault->partial) : size;
	if (err) {
		fault->erase = 0;
		fault->partial = 0;
	}
	memset(destination, 0xff, n);
	if (n) record_mutation(device, HOST_FLASH_ERASE, offset, n);
	return err;
}

void host_reset(void)
{
	memset(host_internal, 0xff, sizeof(host_internal));
	memset(host_external, 0xff, sizeof(host_external));
	memset(&host_internal_fault, 0, sizeof(host_internal_fault));
	memset(&host_external_fault, 0, sizeof(host_external_fault));
	host_internal_device.ready = true;
	host_external_device.ready = true;
	host_bootbits = 0;
	host_mutation_count = 0;
	host_now_ms = 0;
	host_watchdog_feeds = 0;
	host_tinyusb_inits = 0;
	host_disconnects = 0;
	host_dfu_status = -1;
	host_dfu_button = false;
	host_prf_button = false;
	host_tusb_init_ok = true;
	host_corrupt_internal_write_once = false;
	host_dfu_task_hook = NULL;
	host_task_count = 0;
	host_task_limit = 100000;
	host_last_event = 0;
	host_jump_address = 0;
	host_panic_reason = 0;
}

static void put_le32(uint8_t *data, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i) data[i] = (uint8_t)(value >> (i * 8));
}

void host_fill_image(uint32_t address, uint32_t length, bool recovery)
{
	assert(address <= HOST_EXTERNAL_SIZE && length >= 75 &&
	       length <= HOST_EXTERNAL_SIZE - address - 12);
	uint8_t *header = host_external + address;
	uint8_t *payload = header + 12;
	for (uint32_t i = 0; i < length; ++i) payload[i] = (uint8_t)(i * 37U + 19U);
	put_le32(payload, 0x20040000U);
	put_le32(payload + 4, 0x00008011U);
	payload[length - 3] = recovery ? 1 : 0;
	payload[length - 2] = 22;
	payload[length - 1] = 1;
	put_le32(header, 12);
	put_le32(header + 4, length);
	put_le32(header + 8, crc32_ieee(payload, length));
}

void pb_bootbit_init(void) { host_bootbits |= BIT(PB_BOOTBIT_INITIALIZED); }
void pb_bootbit_set(enum pb_bootbit bit) { host_bootbits |= BIT(bit); }
void pb_bootbit_clr(enum pb_bootbit bit) { host_bootbits &= ~BIT(bit); }
bool pb_bootbit_tst(enum pb_bootbit bit) { return (host_bootbits & BIT(bit)) != 0; }
int pb_buttons_init(void) { return 0; }
bool pb_buttons_prf_requested(void) { return host_prf_button; }
bool pb_buttons_dfu_requested(void) { return host_dfu_button; }
bool pb_buttons_any_pressed(void) { return host_dfu_button || host_prf_button; }
int pb_watchdog_init(void) { return 0; }
int pb_watchdog_feed(void) { host_watchdog_feeds++; return 0; }
void pb_panic_init(void) {}
void pb_panic(pb_panic_reason_t reason)
{
	host_panic_reason = reason;
	host_last_event = HOST_PANIC;
	longjmp(host_event_jmp, HOST_PANIC);
}
void pb_fwjump(uintptr_t address)
{
	host_jump_address = address;
	host_last_event = HOST_JUMP;
	longjmp(host_event_jmp, HOST_JUMP);
}
void sys_reboot(int type)
{
	assert(type == SYS_REBOOT_WARM);
	host_last_event = HOST_REBOOT;
	longjmp(host_event_jmp, HOST_REBOOT);
}
int64_t k_uptime_get(void) { return host_now_ms; }
void k_sleep(int32_t milliseconds)
{
	host_now_ms += milliseconds;
}
void pb_tinyusb_nrf52_init(void) { host_tinyusb_inits++; }
void pb_tinyusb_nrf52_start(void) {}
bool tusb_init(uint8_t rhport, const tusb_rhport_init_t *init)
{
	assert(rhport == 0 && init->role == TUSB_ROLE_DEVICE);
	return host_tusb_init_ok;
}
void tud_task(void)
{
	host_task_count++;
	if (host_dfu_task_hook) host_dfu_task_hook();
	if (host_task_count > host_task_limit) {
		host_last_event = HOST_SCRIPT_END;
		longjmp(host_event_jmp, HOST_SCRIPT_END);
	}
}
void tud_int_handler(uint8_t rhport) { assert(rhport == 0); }
void tud_dfu_finish_flashing(uint8_t status) { host_dfu_status = status; }
void dcd_disconnect(uint8_t rhport) { assert(rhport == 0); host_disconnects++; }
void dcd_int_disable(uint8_t rhport) { assert(rhport == 0); }
void host_dispatch_download(uint16_t block, const uint8_t *data, uint16_t length)
{
	tud_dfu_download_cb(0, block, data, length);
}
void host_dispatch_manifest(void) { tud_dfu_manifest_cb(0); }
void host_dispatch_abort(void) { tud_dfu_abort_cb(0); }
