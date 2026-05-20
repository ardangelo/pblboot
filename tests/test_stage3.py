# Copyright (c) 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PARENT = ROOT.parent
SCRATCH_SIZE = 1024 * 1024
DFU_XFER_SIZE = 256
BOOTBIT_NEW_FW_AVAILABLE = 1
BOOTBIT_FORCE_DFU = 20


class DfuScratch:
    def __init__(self):
        self.data = bytearray(b"\xff" * SCRATCH_SIZE)
        self.erased_sectors = set()
        self.expected_block = 0
        self.bytes_received = 0
        self.bootbits = 0

    def write_block(self, block, payload):
        if len(payload) == 0:
            if self.bytes_received == 0:
                raise ValueError("empty manifest")
            return
        if block == 0:
            self.erased_sectors.clear()
            self.expected_block = 0
            self.bytes_received = 0
        elif block != self.expected_block:
            raise ValueError("out of order")

        offset = block * DFU_XFER_SIZE
        end = offset + len(payload)
        if end > SCRATCH_SIZE:
            raise ValueError("out of range")

        start_sector = offset // 4096
        end_sector = (end - 1) // 4096
        for sector in range(start_sector, end_sector + 1):
            if sector not in self.erased_sectors:
                sector_offset = sector * 4096
                self.data[sector_offset:sector_offset + 4096] = b"\xff" * 4096
                self.erased_sectors.add(sector)

        self.data[offset:end] = payload
        self.bytes_received += len(payload)
        self.expected_block = block + 1

    def manifest(self):
        if self.bytes_received == 0:
            raise ValueError("empty manifest")
        self.bootbits |= 1 << BOOTBIT_NEW_FW_AVAILABLE


def test_bootloader_and_firmware_force_dfu_bootbit_values_match():
    bootloader = (ROOT / "include/pb/bootbit.h").read_text()
    firmware = (PARENT / "src/fw/system/bootbits.h").read_text()

    assert "PB_BOOTBIT_FORCE_DFU = 20" in bootloader
    assert "BOOT_BIT_FORCE_DFU = 0x1 << 20" in firmware


def test_dfu_entry_sources_are_wired_before_update_flow():
    main = (ROOT / "boot/src/main.c").read_text()
    buttons = (ROOT / "boot/src/buttons.c").read_text()
    usb_dfu = (ROOT / "boot/src/usb_dfu.c").read_text()

    assert "pb_bootbit_force_dfu_tst_and_clr()" in usb_dfu
    assert "pb_buttons_dfu_requested()" in usb_dfu
    assert "button_dfu_pressed" in buttons
    assert main.index("pb_usb_dfu_requested()") < main.index("reset loop counter handling")
    assert main.index("pb_usb_dfu_requested()") < main.index("pb_bootbit_force_prf_tst_and_clr()")


def test_dfu_writes_payload_to_scratch_without_description_header():
    dfu = DfuScratch()
    block0 = bytes(range(DFU_XFER_SIZE))
    block1 = b"payload-tail"

    dfu.write_block(0, block0)
    dfu.write_block(1, block1)

    assert dfu.data[:DFU_XFER_SIZE] == block0
    assert dfu.data[DFU_XFER_SIZE:DFU_XFER_SIZE + len(block1)] == block1
    assert dfu.data[:4] != len(block0 + block1).to_bytes(4, "little")


def test_dfu_handles_partial_final_flash_write():
    usb_dfu = (ROOT / "boot/src/usb_dfu.c").read_text()

    assert "static int dfu_flash_write" in usb_dfu
    assert "aligned_size = size & ~(DFU_WRITE_SIZE - 1U)" in usb_dfu
    assert "size - aligned_size" in usb_dfu


def test_out_of_range_dfu_write_is_rejected_without_manifest_bit():
    dfu = DfuScratch()
    for block in range(SCRATCH_SIZE // DFU_XFER_SIZE):
        dfu.write_block(block, b"x")

    try:
        dfu.write_block(SCRATCH_SIZE // DFU_XFER_SIZE, b"x")
    except ValueError as exc:
        assert str(exc) == "out of range"
    else:
        raise AssertionError("out-of-range write accepted")

    assert dfu.bootbits & (1 << BOOTBIT_NEW_FW_AVAILABLE) == 0


def test_dfu_manifest_sets_new_firmware_available_bit():
    dfu = DfuScratch()

    dfu.write_block(0, b"pblboot-headered image")
    dfu.manifest()

    assert dfu.bootbits & (1 << BOOTBIT_NEW_FW_AVAILABLE)


def test_firmware_boot_dfu_command_sets_shared_bootbit_and_resets():
    commands_c = (PARENT / "src/fw/console/prompt_commands.c").read_text()
    commands_h = (PARENT / "src/fw/console/prompt_commands.h").read_text()

    assert "void command_boot_dfu(void)" in commands_c
    assert "boot_bit_set(BOOT_BIT_FORCE_DFU);" in commands_c
    assert "system_reset();" in commands_c
    assert '"boot dfu", command_boot_dfu, 0' in commands_h
