#ifndef HOST_ZEPHYR_DEVICE_H
#define HOST_ZEPHYR_DEVICE_H

#include <stdbool.h>
#include <zephyr/devicetree.h>

struct device { bool external; bool ready; };
extern struct device host_internal_device;
extern struct device host_external_device;
#define DEVICE_DT_GET(node) DEVICE_DT_GET_(node)
#define DEVICE_DT_GET_(node) DEVICE_DT_GET_##node
#define DEVICE_DT_GET_zephyr_flash_controller (&host_internal_device)
#define DEVICE_DT_GET_pb_external_flash (&host_external_device)

static inline bool device_is_ready(const struct device *device)
{
	return device->ready;
}

#endif
