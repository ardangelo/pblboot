# Copyright (c) 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

if(NOT CONFIG_PB_USB_DFU)
  return()
endif()

set(TINYUSB_DIR ${CMAKE_CURRENT_LIST_DIR}/tinyusb)

set(tinyusb_sources
  ${TINYUSB_DIR}/src/tusb.c
  ${TINYUSB_DIR}/src/common/tusb_fifo.c
  ${TINYUSB_DIR}/src/device/usbd.c
  ${TINYUSB_DIR}/src/device/usbd_control.c
  ${TINYUSB_DIR}/src/class/dfu/dfu_device.c
  ${TINYUSB_DIR}/src/portable/nordic/nrf5x/dcd_nrf5x.c
  ${CMAKE_CURRENT_LIST_DIR}/port/nrf52.c
)

target_sources(app PRIVATE ${tinyusb_sources})

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${TINYUSB_DIR}/hw
  ${TINYUSB_DIR}/src
  ${TINYUSB_DIR}/src/common
  ${TINYUSB_DIR}/src/device
  ${TINYUSB_DIR}/src/class/dfu
  ${TINYUSB_DIR}/src/portable/nordic/nrf5x
)

target_compile_options(app PRIVATE
  -Wno-error=cast-align
  -Wno-error=cast-qual
  -Wno-error=redundant-decls
  -Wno-error=undef
  -Wno-error=unused-function
  -Wno-error=unused-parameter
  -Wno-error=unused-variable
)
