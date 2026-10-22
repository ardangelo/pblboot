cmake_minimum_required(VERSION 3.20)
project(pblboot_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT PB_ZEPHYR_BASE OR NOT EXISTS "${PB_ZEPHYR_BASE}/subsys/crc/crc32_sw.c")
  message(FATAL_ERROR "PB_ZEPHYR_BASE must point to the Zephyr source tree")
endif()

set(boot_source "${CMAKE_CURRENT_SOURCE_DIR}/../../boot/src")
set(common_sources
  fake_flash.c
  "${PB_ZEPHYR_BASE}/subsys/crc/crc32_sw.c"
  "${boot_source}/firmware.c"
  "${boot_source}/usb_dfu.c"
)

foreach(suite firmware usb_dfu boot_flow)
  add_executable(test_${suite} test_${suite}.c ${common_sources})
  target_include_directories(test_${suite} PRIVATE include .
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include" "${boot_source}")
  target_compile_definitions(test_${suite} PRIVATE
    CONFIG_PB_FIRMWARE_NRF52=1 CONFIG_PB_FIRMWARE_NRF52_STAGED_UPDATE=1
    CONFIG_PB_FIRMWARE_PLATFORM_ID=22 CONFIG_PB_USB_DFU=1
    CONFIG_PB_FLASH_READ_BUF_SIZE=4096 CONFIG_PBLBOOT_LOG_LEVEL=3
    CONFIG_FLASH_BASE_ADDRESS=0 CONFIG_SRAM_BASE_ADDRESS=0x20000100
    CONFIG_SRAM_SIZE=255 CONFIG_PB_USB_DFU_IDLE_TIMEOUT_MS=100
    CONFIG_PB_USB_DFU_TRANSFER_TIMEOUT_MS=250
    CONFIG_PB_USB_DFU_POLLTIMEOUT_MS=10)
  if(suite STREQUAL "boot_flow")
    target_sources(test_${suite} PRIVATE "${boot_source}/main.c")
    set_source_files_properties("${boot_source}/main.c" PROPERTIES COMPILE_DEFINITIONS "main=pb_test_boot_main")
  endif()
  add_test(NAME ${suite} COMMAND test_${suite})
endforeach()
