#ifndef HOST_ZEPHYR_KERNEL_H
#define HOST_ZEPHYR_KERNEL_H
#include <stdint.h>
#define K_MSEC(value) (value)
int64_t k_uptime_get(void);
void k_sleep(int32_t milliseconds);
#endif
