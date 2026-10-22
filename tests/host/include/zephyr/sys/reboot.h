#ifndef HOST_ZEPHYR_REBOOT_H
#define HOST_ZEPHYR_REBOOT_H
#include <zephyr/toolchain.h>
#define SYS_REBOOT_WARM 1
void FUNC_NORETURN sys_reboot(int type);
#endif
