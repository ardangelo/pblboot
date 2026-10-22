#ifndef HOST_ZEPHYR_FLASH_H
#define HOST_ZEPHYR_FLASH_H

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

int flash_read(const struct device *device, uint32_t offset, void *data, size_t size);
int flash_write(const struct device *device, uint32_t offset, const void *data, size_t size);
int flash_erase(const struct device *device, uint32_t offset, size_t size);

#endif
