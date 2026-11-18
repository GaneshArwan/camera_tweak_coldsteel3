r"""
Tier 1 E2E Test Suite: Exact AOB Pattern Uniqueness & Verification on Disk
Target Binary: <GAME_DIR>\bin\x64\ed8_3_PC.exe (configured via CS3_EXE_PATH)

Tests verify against the real disk binary:
1. 100% uniqueness (exactly 1 match, 0 false-positive collisions).
2. Exact expected file offset.
3. Exact expected Virtual Address (VA = ImageBase + RVA).
4. Machine code instruction validity via Capstone disassembly.
5. Strict PE section residency within .text code boundaries.
6. Target RIP-relative address resolution for GameManager pointer.
7. Total scan duration SLA (< 50ms).
"""
import os
import time
import pytest
import capstone
from test_helpers import (
    DEFAULT_EXE_PATH,
    DEFAULT_IMAGE_BASE,
    TEXT_RAW_START,
    TEXT_RAW_END,
    AOB_SIGNATURES,
    SECONDARY_SIGNATURES,
    parse_pe_headers,
    scan_binary_pattern,
    assert_unique_match,
    PEInfo,
)


@pytest.fixture(scope="module")
def binary_data():
    """Loads target executable into memory once for all Tier 1 tests."""
    if not DEFAULT_EXE_PATH or not os.path.isfile(DEFAULT_EXE_PATH):
        pytest.skip("Set CS3_EXE_PATH to ed8_3_PC.exe to run on-disk AOB signature tests")
    with open(DEFAULT_EXE_PATH, "rb") as f:
        data = f.read()
    assert len(data) > 0, "Target game binary is empty"
    return data


@pytest.fixture(scope="module")
def pe_info(binary_data):
    """Parses PE headers and section mapping."""
    return parse_pe_headers(binary_data)


@pytest.fixture(scope="module")
def disassembler():
    """Capstone x64 disassembler engine."""
    return capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


# ==============================================================================
# PE Header & Section Boundary Verification
# ==============================================================================

def test_pe_header_structure(binary_data, pe_info):
    """Verifies standard PE32+ 64-bit DOS/PE headers and ImageBase."""
    assert binary_data[:2] == b"MZ", "DOS header signature mismatch"
    assert pe_info.image_base == DEFAULT_IMAGE_BASE, f"Expected ImageBase {hex(DEFAULT_IMAGE_BASE)}, got {hex(pe_info.image_base)}"
    assert ".text" in pe_info.sections, "Required .text section missing in PE"
    assert ".rdata" in pe_info.sections, "Required .rdata section missing in PE"
    assert ".data" in pe_info.sections, "Required .data section missing in PE"


def test_text_section_boundaries(pe_info):
    """Verifies that the .text section raw boundaries match expected layout."""
    text_sec = pe_info.sections[".text"]
    assert text_sec.raw_offset == TEXT_RAW_START, f"Expected .text raw offset {hex(TEXT_RAW_START)}, got {hex(text_sec.raw_offset)}"
    assert text_sec.raw_offset + text_sec.raw_size == TEXT_RAW_END, f"Expected .text raw end {hex(TEXT_RAW_END)}, got {hex(text_sec.raw_offset + text_sec.raw_size)}"
    assert text_sec.virtual_address == 0x1000, f"Expected .text RVA 0x1000, got {hex(text_sec.virtual_address)}"


# ==============================================================================
# Mandatory AOB Signature 1: Pitch Clamp
# ==============================================================================

def test_pitch_clamp_aob_uniqueness(binary_data, pe_info):
    """
    Pitch Clamp Signature:
    Pattern: F3 0F 5D BB 24 01 00 00 F3 0F 5F BB 20 01 00 00
    File Offset: 0x1fc12a | VA: 0x1401fcd2a
    Asserts: Exactly 1 match, 0 collisions.
    """
    sig = AOB_SIGNATURES["pitch_clamp"]
    offset = assert_unique_match(
        binary_data,
        sig["pattern"],
        expected_offset=sig["expected_offset"],
        expected_va=sig["expected_va"],
        pe_info=pe_info
    )
    assert pe_info.is_in_text(offset), f"Match offset {hex(offset)} not inside .text section"


def test_pitch_clamp_disassembly(binary_data, pe_info, disassembler):
    """Verifies machine instructions at pitch clamp offset: minss + maxss."""
    sig = AOB_SIGNATURES["pitch_clamp"]
    offset = sig["expected_offset"]
    va = sig["expected_va"]
    code_slice = binary_data[offset:offset + sig["length"]]

    insns = list(disassembler.disasm(code_slice, va))
    assert len(insns) == 2, f"Expected 2 instructions, got {len(insns)}"
    assert insns[0].mnemonic == "minss", f"Expected minss, got {insns[0].mnemonic}"
    assert "xmm7" in insns[0].op_str and ("0x124" in insns[0].op_str or "124h" in insns[0].op_str)
    assert insns[1].mnemonic == "maxss", f"Expected maxss, got {insns[1].mnemonic}"
    assert "xmm7" in insns[1].op_str and ("0x120" in insns[1].op_str or "120h" in insns[1].op_str)


# ==============================================================================
# Mandatory AOB Signature 2: Zoom Max Clamp
# ==============================================================================

def test_zoom_max_clamp_aob_uniqueness(binary_data, pe_info):
    """
    Zoom Max Clamp Signature:
    Pattern: F3 45 0F 5D 96 C4 A7 03 00
    File Offset: 0x2dd75c | VA: 0x1402de35c
    Asserts: Exactly 1 match, 0 collisions.
    """
    sig = AOB_SIGNATURES["zoom_max_clamp"]
    offset = assert_unique_match(
        binary_data,
        sig["pattern"],
        expected_offset=sig["expected_offset"],
        expected_va=sig["expected_va"],
        pe_info=pe_info
    )
    assert pe_info.is_in_text(offset), f"Match offset {hex(offset)} not inside .text section"


def test_zoom_max_clamp_disassembly(binary_data, disassembler):
    """Verifies machine instruction at zoom max clamp offset: minss xmm10, [r14+3a7c4h]."""
    sig = AOB_SIGNATURES["zoom_max_clamp"]
    offset = sig["expected_offset"]
    va = sig["expected_va"]
    code_slice = binary_data[offset:offset + sig["length"]]

    insns = list(disassembler.disasm(code_slice, va))
    assert len(insns) == 1, f"Expected 1 instruction, got {len(insns)}"
    assert insns[0].mnemonic == "minss"
    assert "xmm10" in insns[0].op_str
    assert "3a7c4h" in insns[0].op_str or "0x3a7c4" in insns[0].op_str


# ==============================================================================
# Mandatory AOB Signature 3: Altitude Write
# ==============================================================================

def test_altitude_write_aob_uniqueness(binary_data, pe_info):
    """
    Altitude Write Signature:
    Pattern: F3 0F 11 86 68 03 00 00
    File Offset: 0x2dd7bb | VA: 0x1402de3bb
    Asserts: Exactly 1 match, 0 collisions.
    """
    sig = AOB_SIGNATURES["altitude_write"]
    offset = assert_unique_match(
        binary_data,
        sig["pattern"],
        expected_offset=sig["expected_offset"],
        expected_va=sig["expected_va"],
        pe_info=pe_info
    )
    assert pe_info.is_in_text(offset), f"Match offset {hex(offset)} not inside .text section"


def test_altitude_write_disassembly(binary_data, disassembler):
    """Verifies machine instruction at altitude write offset: movss [rsi+368h], xmm0."""
    sig = AOB_SIGNATURES["altitude_write"]
    offset = sig["expected_offset"]
    va = sig["expected_va"]
    code_slice = binary_data[offset:offset + sig["length"]]

    insns = list(disassembler.disasm(code_slice, va))
    assert len(insns) == 1, f"Expected 1 instruction, got {len(insns)}"
    assert insns[0].mnemonic == "movss"
    assert "rsi" in insns[0].op_str
    assert "368h" in insns[0].op_str or "0x368" in insns[0].op_str
    assert "xmm0" in insns[0].op_str


# ==============================================================================
# Mandatory AOB Signature 4: Zoom Write
# ==============================================================================

def test_zoom_write_aob_uniqueness(binary_data, pe_info):
    """
    Zoom Write Signature:
    Pattern: F3 44 0F 11 96 5C 06 00 00
    File Offset: 0x2dd7f7 | VA: 0x1402de3f7
    Asserts: Exactly 1 match, 0 collisions.
    """
    sig = AOB_SIGNATURES["zoom_write"]
    offset = assert_unique_match(
        binary_data,
        sig["pattern"],
        expected_offset=sig["expected_offset"],
        expected_va=sig["expected_va"],
        pe_info=pe_info
    )
    assert pe_info.is_in_text(offset), f"Match offset {hex(offset)} not inside .text section"


def test_zoom_write_disassembly(binary_data, disassembler):
    """Verifies machine instruction at zoom write offset: movss [rsi+65ch], xmm10."""
    sig = AOB_SIGNATURES["zoom_write"]
    offset = sig["expected_offset"]
    va = sig["expected_va"]
    code_slice = binary_data[offset:offset + sig["length"]]

    insns = list(disassembler.disasm(code_slice, va))
    assert len(insns) == 1, f"Expected 1 instruction, got {len(insns)}"
    assert insns[0].mnemonic == "movss"
    assert "rsi" in insns[0].op_str
    assert "65ch" in insns[0].op_str or "0x65c" in insns[0].op_str
    assert "xmm10" in insns[0].op_str


# ==============================================================================
# Mandatory AOB Signature 5: GameManager Resolver
# ==============================================================================

def test_gm_resolver_aob_uniqueness(binary_data, pe_info):
    """
    GameManager Resolver Signature:
    Pattern: 48 8B 05 ?? ?? ?? ?? 48 8B 80 E8 1C 00 00 C3
    File Offset: 0x2a7100 | VA: 0x1402a7d00
    Asserts: Exactly 1 match, 0 collisions.
    """
    sig = AOB_SIGNATURES["gm_resolver"]
    offset = assert_unique_match(
        binary_data,
        sig["pattern"],
        expected_offset=sig["expected_offset"],
        expected_va=sig["expected_va"],
        pe_info=pe_info
    )
    assert pe_info.is_in_text(offset), f"Match offset {hex(offset)} not inside .text section"


def test_gm_resolver_disassembly_and_displacement(binary_data, disassembler):
    """
    Verifies machine instructions at GameManager resolver:
    1. mov rax, [rip + disp32] -> target points to GameManager pointer in data section
    2. mov rax, [rax + 1ce8h]
    3. ret
    """
    sig = AOB_SIGNATURES["gm_resolver"]
    offset = sig["expected_offset"]
    va = sig["expected_va"]
    code_slice = binary_data[offset:offset + sig["length"]]

    insns = list(disassembler.disasm(code_slice, va))
    assert len(insns) == 3, f"Expected 3 instructions, got {len(insns)}"
    assert insns[0].mnemonic == "mov" and "rax" in insns[0].op_str and "rip" in insns[0].op_str
    assert insns[1].mnemonic == "mov" and "rax" in insns[1].op_str and ("0x1ce8" in insns[1].op_str or "1ce8h" in insns[1].op_str)
    assert insns[2].mnemonic == "ret"

    # Verify RIP-relative target displacement calculation
    # Instruction is 7 bytes: 48 8B 05 <disp32>
    disp32 = int.from_bytes(code_slice[3:7], byteorder="little", signed=True)
    target_va = va + 7 + disp32
    # Should point to global GameManager root address 0x140C53290 in .data / .rdata
    assert target_va == 0x140C53290, f"Expected RIP displacement target 0x140c53290, got {hex(target_va)}"


# ==============================================================================
# Secondary Signatures Verification
# ==============================================================================

def test_secondary_signatures_uniqueness(binary_data, pe_info):
    """Verifies that secondary game camera structures also resolve uniquely."""
    for key, sig in SECONDARY_SIGNATURES.items():
        offset = assert_unique_match(
            binary_data,
            sig["pattern"],
            expected_offset=sig["expected_offset"],
            expected_va=sig["expected_va"],
            pe_info=pe_info
        )
        assert pe_info.is_in_text(offset), f"Match offset {hex(offset)} not inside .text"


# ==============================================================================
# Performance Benchmark SLA Verification
# ==============================================================================

def test_full_binary_scan_performance_sla(binary_data):
    """
    Scans all 5 core patterns across the full 11.5 MB binary.
    Asserts: Total scanning duration is strictly < 50ms.
    """
    t0 = time.perf_counter()
    for sig in AOB_SIGNATURES.values():
        matches = scan_binary_pattern(binary_data, sig["pattern"])
        assert len(matches) == 1
    total_ms = (time.perf_counter() - t0) * 1000
    print(f"\n[BENCHMARK] Scanned 5 full-binary signatures in {total_ms:.2f} ms")
    assert total_ms < 50.0, f"Scan SLA exceeded: {total_ms:.2f} ms > 50.0 ms"
