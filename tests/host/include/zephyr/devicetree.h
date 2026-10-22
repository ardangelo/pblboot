#ifndef HOST_ZEPHYR_DEVICETREE_H
#define HOST_ZEPHYR_DEVICETREE_H

#define DT_CHOSEN(name) name
#define DT_NODELABEL(name) name
#define DT_REG_ADDR(node) DT_REG_ADDR_(node)
#define DT_REG_ADDR_(node) DT_REG_ADDR_##node
#define DT_REG_SIZE(node) DT_REG_SIZE_(node)
#define DT_REG_SIZE_(node) DT_REG_SIZE_##node
#define DT_PROP(node, property) DT_PROP_(node, property)
#define DT_PROP_(node, property) DT_PROP_##node##_##property
#define DT_PARENT(node) DT_PARENT_(node)
#define DT_PARENT_(node) DT_PARENT_##node

#define DT_REG_ADDR_pb_firmware 0x8000U
#define DT_REG_SIZE_pb_firmware 0xf8000U
#define DT_REG_ADDR_pb_firmware_scratch 0x100000U
#define DT_REG_SIZE_pb_firmware_scratch 0x100000U
#define DT_REG_ADDR_pb_safe_firmware 0U
#define DT_REG_SIZE_pb_safe_firmware 0xff000U
#define DT_REG_ADDR_pb_boot_state 0x1dd0000U
#define DT_REG_SIZE_pb_boot_state 0x1000U
#define DT_REG_ADDR_sram0 0x20000000U
#define DT_REG_SIZE_sram0 0x40000U
#define DT_PARENT_pb_firmware internal_partition
#define DT_PARENT_internal_partition internal_flash
#define DT_PROP_internal_flash_erase_block_size 4096U
#define DT_PROP_internal_flash_write_block_size 4U

#endif
