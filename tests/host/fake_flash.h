#ifndef PB_HOST_FAKE_FLASH_H
#define PB_HOST_FAKE_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <zephyr/sys/util.h>

#include <pb/bootbit.h>

#define HOST_INTERNAL_SIZE 0x100000U
#define HOST_EXTERNAL_SIZE 0x2000000U
#define HOST_SCRATCH_ADDR 0x100000U
#define HOST_SAFE_SIZE 0xff000U
#define HOST_BOOT_STATE_ADDR 0x1dd0000U
#define HOST_DFU_TRANSFER_SIZE 256U

extern uint8_t host_internal[HOST_INTERNAL_SIZE];
extern uint8_t host_external[HOST_EXTERNAL_SIZE];
extern uint32_t host_bootbits;
extern int64_t host_now_ms;
extern int host_watchdog_feeds;
extern int host_tinyusb_inits;
extern int host_disconnects;
extern int host_dfu_status;
extern bool host_dfu_button;
extern bool host_prf_button;
extern bool host_tusb_init_ok;
extern bool host_corrupt_internal_write_once;
extern void (*host_dfu_task_hook)(void);

enum host_event { HOST_JUMP = 1, HOST_REBOOT, HOST_PANIC, HOST_SCRIPT_END };
extern jmp_buf host_event_jmp;
extern enum host_event host_last_event;
extern uintptr_t host_jump_address;
extern uint32_t host_panic_reason;
extern unsigned host_task_count;
extern unsigned host_task_limit;

/* A fault occurs once on the next matching operation of its kind. A nonzero
 * partial count programs or erases that many bytes before returning err. */
struct host_flash_fault {
	int read;
	int write;
	int erase;
	size_t partial;
	bool match_address_enabled;
	uint32_t match_address;
};
extern struct host_flash_fault host_internal_fault;
extern struct host_flash_fault host_external_fault;

enum host_flash_op { HOST_FLASH_READ, HOST_FLASH_WRITE, HOST_FLASH_ERASE };
struct host_mutation {
	bool external;
	enum host_flash_op op;
	uint32_t address;
	size_t length;
};
#define HOST_MAX_MUTATIONS 8192U
extern struct host_mutation host_mutations[HOST_MAX_MUTATIONS];
extern size_t host_mutation_count;

void host_reset(void);
void host_fill_image(uint32_t scratch_address, uint32_t payload_length, bool recovery);
void host_dispatch_download(uint16_t block, const uint8_t *data, uint16_t length);
void host_dispatch_manifest(void);
void host_dispatch_abort(void);

#endif
