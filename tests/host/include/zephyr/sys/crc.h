#ifndef HOST_ZEPHYR_CRC_H
#define HOST_ZEPHYR_CRC_H

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

uint32_t crc32_ieee(const uint8_t *data, size_t len);
uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t len);

#endif
