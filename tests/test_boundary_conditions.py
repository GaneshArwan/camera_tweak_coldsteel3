r"""
Tier 2 E2E Test Suite: Boundary Conditions, Corrupt PE Handling & Pattern Collisions

Tests verify defensive error handling and boundary resilience:
1. Missing executable paths and directories instead of files.
2. 0-byte, truncated, and corrupt DOS / PE headers.
3. Missing or truncated PE section headers.
4. Malformed AOB pattern syntax (odd hex chars, non-hex, invalid wildcards, empty).
5. Collision detection on synthetic buffers and real game binary.
6. Extreme buffer boundaries (0-length buffer, pattern longer than buffer, exact edge alignment).
"""
import os
import struct
import pytest
from test_helpers import (
    DEFAULT_EXE_PATH,
    KNOWN_COLLIDING_PATTERN,
    parse_aob,
    scan_binary_pattern,
    assert_unique_match,
    parse_pe_headers,
    AOBPatternError,
    PatternNotFoundError,
    PatternCollisionError,
    InvalidPEError,
    SectionNotFoundError,
)


# ==============================================================================
# Category 1: Missing File, Path Boundaries & Type Errors
# ==============================================================================

def test_missing_file_raises_error():
    """Verifies that non-existent executable path raises FileNotFoundError."""
    non_existent = r"C:\NonExistentDirectory\FakePath\ed8_3_PC.exe"
    with pytest.raises(FileNotFoundError):
        with open(non_existent, "rb") as f:
            f.read()


def test_directory_path_instead_of_file():
    """Verifies opening a directory as a binary file is rejected."""
    dir_path = os.path.dirname(DEFAULT_EXE_PATH) if DEFAULT_EXE_PATH else os.path.dirname(os.path.abspath(__file__))
    if os.path.exists(dir_path):
        with pytest.raises((PermissionError, IsADirectoryError, OSError)):
            with open(dir_path, "rb") as f:
                f.read()


def test_scan_invalid_data_types():
    """Verifies scan_binary_pattern rejects non-bytes input."""
    with pytest.raises(TypeError):
        scan_binary_pattern("not bytes", "F3 0F 5D")
    with pytest.raises(TypeError):
        scan_binary_pattern(12345, "F3 0F 5D")
    with pytest.raises(TypeError):
        scan_binary_pattern(None, "F3 0F 5D")


# ==============================================================================
# Category 2: Truncated, 0-Byte & Corrupt PE Headers
# ==============================================================================

def test_empty_zero_byte_file():
    """Verifies 0-byte file raises InvalidPEError."""
    with pytest.raises(InvalidPEError, match="too small"):
        parse_pe_headers(b"")


def test_truncated_dos_header_under_64_bytes():
    """Verifies binary under 64 bytes is rejected immediately."""
    with pytest.raises(InvalidPEError, match="too small"):
        parse_pe_headers(b"MZ" + b"\x00" * 30)


def test_invalid_dos_magic_signature():
    """Verifies file with non-MZ magic is rejected."""
    garbage = b"PK\x03\x04" + b"\x00" * 100
    with pytest.raises(InvalidPEError, match="Invalid DOS signature"):
        parse_pe_headers(garbage)


def test_e_lfanew_offset_past_eof():
    """Verifies DOS header pointing to PE offset past end-of-file is rejected."""
    header = bytearray(b"MZ" + b"\x00" * 62)
    struct.pack_into("<I", header, 0x3C, 0x1000)  # e_lfanew = 4096, but buffer len is 64
    with pytest.raises(InvalidPEError, match="Invalid e_lfanew offset"):
        parse_pe_headers(bytes(header))


def test_e_lfanew_offset_pointing_to_non_pe():
    """Verifies e_lfanew pointing to non-PE magic raises InvalidPEError."""
    header = bytearray(b"MZ" + b"\x00" * 128)
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"NE\x00\x00"  # 16-bit New Executable magic
    with pytest.raises(InvalidPEError, match="Invalid PE signature"):
        parse_pe_headers(bytes(header))


def test_pe_truncated_in_coff_header():
    """Verifies binary truncated before 20-byte COFF header completes is rejected."""
    header = bytearray(b"MZ" + b"\x00" * 128)
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\x00\x00"
    # Only 10 bytes after PE signature instead of 20
    header = header[:0x44 + 10]
    with pytest.raises(InvalidPEError, match="truncated in COFF header"):
        parse_pe_headers(bytes(header))


def test_pe_missing_text_section():
    """Verifies PE without a .text section raises SectionNotFoundError."""
    # Construct minimal PE with only .data section
    header = bytearray(b"MZ" + b"\x00" * 512)
    e_lfanew = 0x40
    struct.pack_into("<I", header, 0x3C, e_lfanew)
    header[e_lfanew:e_lfanew+4] = b"PE\x00\x00"
    # COFF: machine=0x8664, num_sections=1, size_opt=240
    struct.pack_into("<HHIIIHH", header, e_lfanew + 4, 0x8664, 1, 0, 0, 0, 240, 0x02)
    # Optional header: magic=0x20B (PE32+)
    struct.pack_into("<H", header, e_lfanew + 24, 0x20B)
    # Section header at e_lfanew + 24 + 240 = e_lfanew + 264
    sec_off = e_lfanew + 264
    header[sec_off:sec_off+8] = b".data\x00\x00\x00"
    struct.pack_into("<IIII", header, sec_off + 8, 0x1000, 0x2000, 0x1000, 0x400)
    
    with pytest.raises(SectionNotFoundError, match=".text code section not found"):
        parse_pe_headers(bytes(header))


# ==============================================================================
# Category 3: Malformed AOB Pattern Parsing
# ==============================================================================

def test_aob_empty_pattern_rejected():
    """Verifies empty pattern strings raise AOBPatternError."""
    with pytest.raises(AOBPatternError, match="cannot be empty"):
        parse_aob("")
    with pytest.raises(AOBPatternError, match="cannot be empty"):
        parse_aob("     ")


def test_aob_odd_length_hex_rejected():
    """Verifies odd-length hex characters raise AOBPatternError."""
    with pytest.raises(AOBPatternError, match="Malformed token"):
        parse_aob("F3 0F 5")
    with pytest.raises(AOBPatternError, match="Malformed token"):
        parse_aob("F3 0F 123")


def test_aob_non_hex_characters_rejected():
    """Verifies non-hex characters raise AOBPatternError."""
    with pytest.raises(AOBPatternError, match="Invalid non-hex token"):
        parse_aob("F3 0F ZZ BB")
    with pytest.raises(AOBPatternError, match="Invalid non-hex token"):
        parse_aob("48 8B GH 12")


def test_aob_invalid_wildcard_syntax_rejected():
    """Verifies invalid wildcard syntax raises AOBPatternError."""
    with pytest.raises(AOBPatternError, match="Malformed token"):
        parse_aob("48 8B ??? 12")
    with pytest.raises(AOBPatternError, match="(Malformed token|Invalid non-hex token)"):
        parse_aob("48 8B ?F 12")


def test_aob_only_wildcards_rejected():
    """Verifies pattern with only wildcards and zero concrete bytes is rejected."""
    with pytest.raises(AOBPatternError, match="cannot consist entirely of wildcards"):
        parse_aob("?? ?? ?? ??")
    with pytest.raises(AOBPatternError, match="cannot consist entirely of wildcards"):
        parse_aob("? ? ?")


def test_aob_whitespace_and_case_insensitivity():
    """Verifies extra whitespace and lowercase hex are parsed cleanly and match identically."""
    regex_upper = parse_aob("F3 0F 5D BB 24")
    regex_lower = parse_aob("  f3   0f   5d   bb   24  ")
    test_sample = bytes.fromhex("F3 0F 5D BB 24")
    assert regex_upper.search(test_sample) is not None
    assert regex_lower.search(test_sample) is not None


# ==============================================================================
# Category 4: Synthetic Collision & Boundary Tests
# ==============================================================================

def test_pattern_collision_on_synthetic_buffer():
    """
    Constructs a synthetic buffer with a duplicate pattern (2 occurrences).
    Verifies assert_unique_match raises PatternCollisionError with exact count.
    """
    pattern_bytes = bytes.fromhex("AA BB CC DD EE FF")
    pattern_str = "AA BB CC DD EE FF"
    synthetic_mem = bytearray(b"\x00" * 100)
    synthetic_mem[10:16] = pattern_bytes
    synthetic_mem[60:66] = pattern_bytes

    with pytest.raises(PatternCollisionError) as exc_info:
        assert_unique_match(bytes(synthetic_mem), pattern_str)
    
    assert len(exc_info.value.matches) == 2
    assert exc_info.value.matches == [10, 60]


def test_multiple_collisions_reporting():
    """
    Constructs a synthetic buffer with 5 repeated occurrences.
    Verifies all 5 offsets are captured in the collision report.
    """
    pattern_bytes = bytes.fromhex("DE AD BE EF")
    pattern_str = "DE AD BE EF"
    offsets = [10, 30, 50, 70, 90]
    buf = bytearray(b"\x90" * 120)
    for off in offsets:
        buf[off:off+4] = pattern_bytes

    with pytest.raises(PatternCollisionError) as exc_info:
        assert_unique_match(bytes(buf), pattern_str)
    
    assert exc_info.value.matches == offsets


def test_real_game_colliding_pattern_detected():
    """
    Tests the known non-unique config pattern (C7 83 C0 A7 03 00 CD CC CC 3F)
    against the actual game binary. Verifies it correctly triggers PatternCollisionError.
    """
    if not DEFAULT_EXE_PATH or not os.path.isfile(DEFAULT_EXE_PATH):
        pytest.skip("Set CS3_EXE_PATH to ed8_3_PC.exe to run disk-binary collision test")

    with open(DEFAULT_EXE_PATH, "rb") as f:
        binary_data = f.read()

    with pytest.raises(PatternCollisionError) as exc_info:
        assert_unique_match(binary_data, KNOWN_COLLIDING_PATTERN)
    
    assert len(exc_info.value.matches) == 2, f"Expected 2 collisions, got {len(exc_info.value.matches)}"
    assert exc_info.value.matches == [0x2c1bcc, 0x2ced1e]


def test_pattern_not_found_raises_runtime_error():
    """Verifies that a pattern not in the binary raises PatternNotFoundError."""
    synthetic_mem = b"\x00\x11\x22\x33\x44\x55"
    missing_pattern = "FF EE DD CC"
    with pytest.raises(PatternNotFoundError, match="not found"):
        assert_unique_match(synthetic_mem, missing_pattern)


def test_pattern_at_start_and_end_boundaries():
    """Verifies patterns located at index 0 and index (len - pattern_len) match accurately."""
    pat_bytes = bytes.fromhex("12 34 56 78")
    pat_str = "12 34 56 78"
    
    # Test at index 0
    buf_start = pat_bytes + b"\x00" * 20
    assert assert_unique_match(buf_start, pat_str) == 0

    # Test at final byte boundary
    buf_end = b"\x00" * 20 + pat_bytes
    assert assert_unique_match(buf_end, pat_str) == 20


def test_pattern_longer_than_buffer():
    """Verifies searching a pattern longer than the target buffer returns 0 matches safely."""
    small_buf = b"\x12\x34"
    long_pat = "12 34 56 78 90 AB"
    matches = scan_binary_pattern(small_buf, long_pat)
    assert matches == []
