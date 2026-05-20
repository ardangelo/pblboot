# Copyright (c) 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

import importlib.util
import struct
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PARENT = ROOT.parent
PBLBOOT_MAGIC = 0x96F3B83D
HEADER = struct.Struct("<LLQLLL")
HEADER_LEN = HEADER.size
FIRMWARE_LOAD_ADDR = 0x8000
FIRMWARE_SIZE = 0xF8000
SRAM_BASE = 0x20000100
SRAM_SIZE = 255 * 1024


def _load_pblboot_tool():
    spec = importlib.util.spec_from_file_location("pblboot_tool", PARENT / "waftools" / "pblboot.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _payload(size=128):
    data = bytearray((i & 0xFF for i in range(size)))
    struct.pack_into("<LL", data, 0, SRAM_BASE + 0x1000, FIRMWARE_LOAD_ADDR + 0x41)
    return bytes(data)


def _headered(payload, *, magic=PBLBOOT_MAGIC, header_len=HEADER_LEN, start=HEADER_LEN,
              length=None, crc=None):
    if length is None:
        length = len(payload)
    if crc is None:
        crc = zlib.crc32(payload) & 0xFFFFFFFF
    return HEADER.pack(magic, header_len, 1, start, length, crc) + (b"\xff" * (start - HEADER_LEN)) + payload


def _validate_staged(image):
    if len(image) < HEADER_LEN:
        raise ValueError("truncated header")
    magic, header_len, _timestamp, start, length, expected_crc = HEADER.unpack_from(image)
    if magic != PBLBOOT_MAGIC:
        raise ValueError("bad magic")
    if header_len != HEADER_LEN:
        raise ValueError("bad header length")
    if start < header_len or length == 0 or length > FIRMWARE_SIZE:
        raise ValueError("bad bounds")
    if start + length > len(image):
        raise ValueError("truncated payload")
    initial_sp, reset_vector = struct.unpack_from("<LL", image, start)
    if not (SRAM_BASE <= initial_sp < SRAM_BASE + SRAM_SIZE):
        raise ValueError("bad stack")
    if reset_vector & 1 == 0:
        raise ValueError("reset vector not thumb")
    reset_addr = reset_vector & ~1
    if not (FIRMWARE_LOAD_ADDR <= reset_addr < FIRMWARE_LOAD_ADDR + length):
        raise ValueError("bad load address")
    payload = image[start:start + length]
    if (zlib.crc32(payload) & 0xFFFFFFFF) != expected_crc:
        raise ValueError("bad crc")
    return payload


def _install(image, internal):
    payload = _validate_staged(image)
    internal[:] = b"\xff" * len(internal)
    internal[:len(payload)] = payload
    if (zlib.crc32(internal[:len(payload)]) & 0xFFFFFFFF) != (zlib.crc32(payload) & 0xFFFFFFFF):
        raise ValueError("internal crc")


def test_pblboot_header_bin_uses_payload_offset(tmp_path):
    tool = _load_pblboot_tool()
    payload = _payload()
    source = tmp_path / "firmware.nohdr.bin"
    output = tmp_path / "firmware.bin"
    source.write_bytes(payload)

    tool._insert_header_bin(str(source), str(output), HEADER_LEN)

    image = output.read_bytes()
    magic, header_len, _timestamp, start, length, crc = HEADER.unpack_from(image)
    assert (magic, header_len, start, length) == (PBLBOOT_MAGIC, HEADER_LEN, HEADER_LEN, len(payload))
    assert crc == zlib.crc32(payload) & 0xFFFFFFFF
    assert image[HEADER_LEN:] == payload


def test_cyberdeck_w25q256jw_devicetree_contract():
    dts = (ROOT / "boards/coredevices/cyberdeck/cyberdeck.dts").read_text()
    pinctrl = (ROOT / "boards/coredevices/cyberdeck/cyberdeck-pinctrl.dtsi").read_text()

    assert 'compatible = "nordic,qspi-nor"' in dts
    assert 'status = "okay"' in dts
    assert "jedec-id = [ef 60 19]" in dts
    assert 'quad-enable-requirements = "S2B1v6"' in dts
    assert "address-size-32" in dts
    assert "enter-4byte-addr = <0xb7>" in dts
    assert "safe_firmware_partition: partition@0" in dts
    assert "firmware_scratch_partition: partition@80000" in dts
    assert "prf_partition: partition@180000" in dts
    assert "NRF_PSEL(QSPI_CSN, 0, 20)" in pinctrl
    assert "NRF_PSEL(QSPI_IO0, 0, 17)" in pinctrl
    assert "NRF_PSEL(QSPI_IO1, 0, 22)" in pinctrl
    assert "NRF_PSEL(QSPI_IO2, 0, 23)" in pinctrl
    assert "NRF_PSEL(QSPI_IO3, 0, 21)" in pinctrl


@pytest.mark.parametrize(
    "image",
    [
        _headered(_payload(), magic=0),
        _headered(_payload(), header_len=HEADER_LEN - 4),
        _headered(_payload(), start=HEADER_LEN - 1),
        _headered(_payload(), length=FIRMWARE_SIZE + 1),
        _headered(_payload(), length=len(_payload()) + 1),
        _headered(_payload(), crc=0),
        _headered(struct.pack("<LL", SRAM_BASE + 4, FIRMWARE_LOAD_ADDR - 1) + b"x" * 64),
    ],
)
def test_malformed_staged_images_are_rejected(image):
    with pytest.raises(ValueError):
        _validate_staged(image)


def test_staged_payload_crc_mismatch_does_not_erase_internal_flash():
    payload = _payload()
    image = bytearray(_headered(payload))
    image[-1] ^= 0x55
    internal = bytearray(b"\x5a" * 4096)

    with pytest.raises(ValueError):
        _install(bytes(image), internal)

    assert internal == bytearray(b"\x5a" * 4096)


def test_successful_staged_image_copy_path():
    payload = _payload(512)
    image = _headered(payload)
    internal = bytearray(b"\x5a" * 4096)

    _install(image, internal)

    assert internal[:len(payload)] == payload
    assert internal[len(payload):] == b"\xff" * (len(internal) - len(payload))
