#ifndef HOST_TUSB_H
#define HOST_TUSB_H
#include <stdbool.h>
#include <stdint.h>

#define CFG_TUD_DFU_XFER_BUFSIZE 256U
#define CFG_TUD_ENDPOINT0_SIZE 64U
#define TUSB_DESC_DEVICE 1U
#define TUSB_DESC_STRING 3U
#define TUD_CONFIG_DESC_LEN 9U
#define TUD_DFU_DESC_LEN(count) (9U + (count) * 9U)
#define TUD_CONFIG_DESCRIPTOR(...) 9, 2, 0, 0, 1, 1, 0, 0, 50
#define TUD_DFU_DESCRIPTOR(...) 9, 4, 0, 0, 0, 0, 0, 0, 0, 9, 33, 0, 0, 0, 0, 0, 0, 0
#define TUSB_ROLE_DEVICE 1
#define TUSB_SPEED_AUTO 0

typedef struct {
	uint8_t bLength, bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
	uint16_t idVendor, idProduct, bcdDevice;
	uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} tusb_desc_device_t;
typedef struct { int role, speed; } tusb_rhport_init_t;
bool tusb_init(uint8_t rhport, const tusb_rhport_init_t *init);
void tud_task(void);
void tud_int_handler(uint8_t rhport);
#endif
