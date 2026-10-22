#ifndef HOST_DFU_DEVICE_H
#define HOST_DFU_DEVICE_H
#include <stdint.h>
#define DFU_ATTR_CAN_DOWNLOAD 1U
#define DFU_STATUS_OK 0U
#define DFU_STATUS_ERR_FILE 2U
#define DFU_STATUS_ERR_WRITE 3U
#define DFU_STATUS_ERR_ADDRESS 8U
#define DFU_STATUS_ERR_UNKNOWN 14U
void tud_dfu_finish_flashing(uint8_t status);
void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const *data, uint16_t length);
void tud_dfu_manifest_cb(uint8_t alt);
void tud_dfu_abort_cb(uint8_t alt);
#endif
