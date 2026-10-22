#-------------------------------------------------------------------------------
# PebbleOS bootloader
#
# Copyright (c) 2025 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)
set(PB_BUILD_TIMESTAMP "" CACHE STRING "Override pblboot HEAD commit timestamp (decimal Unix seconds)")

if(CONFIG_PB_BOOTBIT_NRF52)
  set(_pb_source_root "${CMAKE_CURRENT_LIST_DIR}/..")
  get_filename_component(_pb_source_root "${_pb_source_root}" REALPATH)
  set(_pb_resolved_timestamp "${PB_BUILD_TIMESTAMP}")

  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${_pb_source_root}" rev-parse --show-toplevel
      OUTPUT_VARIABLE _pb_git_root
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _pb_git_root_status
    )
    if(_pb_git_root_status EQUAL 0)
      get_filename_component(_pb_git_root "${_pb_git_root}" REALPATH)
      if(_pb_git_root STREQUAL _pb_source_root)
        execute_process(
          COMMAND "${GIT_EXECUTABLE}" -C "${_pb_source_root}" rev-parse --path-format=absolute --git-path HEAD
          OUTPUT_VARIABLE _pb_head_path
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
          RESULT_VARIABLE _pb_head_path_status
        )
        if(NOT _pb_head_path_status EQUAL 0)
          message(FATAL_ERROR "Cannot resolve pblboot Git HEAD path")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pb_head_path}")

        execute_process(
          COMMAND "${GIT_EXECUTABLE}" -C "${_pb_source_root}" rev-parse --path-format=absolute --git-path packed-refs
          OUTPUT_VARIABLE _pb_packed_refs_path
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
          RESULT_VARIABLE _pb_packed_refs_status
        )
        if(NOT _pb_packed_refs_status EQUAL 0)
          message(FATAL_ERROR "Cannot resolve pblboot Git packed-refs path")
        endif()
        if(EXISTS "${_pb_packed_refs_path}")
          set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pb_packed_refs_path}")
        endif()

        execute_process(
          COMMAND "${GIT_EXECUTABLE}" -C "${_pb_source_root}" symbolic-ref -q HEAD
          OUTPUT_VARIABLE _pb_head_ref
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
          RESULT_VARIABLE _pb_head_ref_status
        )
        if(_pb_head_ref_status EQUAL 0)
          execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_pb_source_root}" rev-parse --path-format=absolute --git-path "${_pb_head_ref}"
            OUTPUT_VARIABLE _pb_ref_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _pb_ref_path_status
          )
          if(NOT _pb_ref_path_status EQUAL 0)
            message(FATAL_ERROR "Cannot resolve pblboot Git ref path")
          endif()
          if(EXISTS "${_pb_ref_path}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pb_ref_path}")
          else()
            get_filename_component(_pb_ref_parent "${_pb_ref_path}" DIRECTORY)
            while(NOT EXISTS "${_pb_ref_parent}")
              get_filename_component(_pb_ref_parent "${_pb_ref_parent}" DIRECTORY)
            endwhile()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pb_ref_parent}")
          endif()
        elseif(NOT _pb_head_ref_status EQUAL 1)
          message(FATAL_ERROR "Cannot resolve pblboot HEAD symbolic ref")
        endif()

        if(_pb_resolved_timestamp STREQUAL "")
          execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_pb_source_root}" log -1 --format=%ct
            OUTPUT_VARIABLE _pb_resolved_timestamp
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _pb_timestamp_status
          )
          if(NOT _pb_timestamp_status EQUAL 0)
            message(FATAL_ERROR "Cannot read pblboot HEAD timestamp; set -DPB_BUILD_TIMESTAMP=<decimal uint32>")
          endif()
        endif()
      elseif(_pb_resolved_timestamp STREQUAL "")
        message(FATAL_ERROR "Git repository is not pblboot (${_pb_git_root}); set -DPB_BUILD_TIMESTAMP=<decimal uint32>")
      endif()
    endif()
  endif()

  if(NOT _pb_resolved_timestamp MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PB_BUILD_TIMESTAMP must be a decimal timestamp in [1, 4294967295]")
  endif()
  string(REGEX REPLACE "^0+" "" _pb_resolved_timestamp "${_pb_resolved_timestamp}")
  string(LENGTH "${_pb_resolved_timestamp}" _pb_timestamp_digits)
  if(_pb_timestamp_digits EQUAL 0 OR _pb_timestamp_digits GREATER 10 OR
     (_pb_timestamp_digits EQUAL 10 AND _pb_resolved_timestamp STRGREATER "4294967295"))
    message(FATAL_ERROR "PB_BUILD_TIMESTAMP must be a decimal timestamp in [1, 4294967295]")
  endif()
  zephyr_compile_definitions(PB_BUILD_TIMESTAMP=${_pb_resolved_timestamp}UL)
endif()


target_sources(
  app
  PRIVATE
    src/buttons.c
    src/firmware.c
    src/main.c
    src/panic.c
    src/version.c
    src/watchdog.c
)

if(CONFIG_PB_CHARGER)
  target_sources(app PRIVATE src/charger.c)
endif()

if(CONFIG_PB_USB_DFU)
  target_sources(app PRIVATE src/usb_dfu.c)
endif()

if(CONFIG_PB_USB_DFU)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../third_party/tinyusb tinyusb)
endif()

zephyr_linker_sources(SECTIONS version.ld)
