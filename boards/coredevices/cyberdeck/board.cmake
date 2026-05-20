# Copyright (c) 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)

board_runner_args(nrfjprog "--nrf-family=NRF52")

include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
