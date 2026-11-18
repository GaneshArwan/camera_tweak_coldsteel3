"""
Test Helpers & Mock Environment for Cold Steel III Camera Tweak E2E Test Suite
Provides:
1. Robust AOB string parsing & regex generation with strict boundary validation.
2. Lightweight PE32+ (x64) parser for section resolution and VA mapping.
3. In-memory MockProcessMemory simulating Windows virtual memory operations.
4. Transactional PatchEngine with signal handling and atomic rollback registry.
5. High-fidelity CameraControllerSimulator modeling game camera state and clamps.
"""
import os
import re
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# Optional game binary path on disk (set CS3_EXE_PATH to run disk-binary verification tests)
DEFAULT_EXE_PATH = os.environ.get("CS3_EXE_PATH", "")

# Section and memory constants from Cold Steel III PE header
DEFAULT_IMAGE_BASE = 0x140000000
TEXT_RAW_START = 0x400
TEXT_RAW_END = 0x7E9200
TEXT_RAW_SIZE = 0x7E8E00
TEXT_VIRTUAL_START = 0x1000
TEXT_VIRTUAL_SIZE = 0x7E8C0B
TEXT_VA_OFFSET = 0xC00  # VA = 0x140000000 + file_offset + 0xC00 for .text

# Canonical AOB signatures mandated by specification
AOB_SIGNATURES = {
    "pitch_clamp": {
        "name": "Pitch clamp",
        "pattern": "F3 0F 5D BB 24 01 00 00 F3 0F 5F BB 20 01 00 00",
        "expected_offset": 0x1FC12A,
        "expected_va": 0x1401FCD2A,
        "length": 16,
        "patch_length": 16,
        "description": "minss [rbx+124h], maxss [rbx+120h] clamping vertical tilt"
    },
    "zoom_max_clamp": {
        "name": "Zoom max clamp",
        "pattern": "F3 45 0F 5D 96 C4 A7 03 00",
        "expected_offset": 0x2DD75C,
        "expected_va": 0x1402DE35C,
        "length": 9,
        "patch_length": 9,
        "description": "minss xmm10, [r14+3a7c4h] clamping max zoom distance"
    },
    "altitude_write": {
        "name": "Altitude write",
        "pattern": "F3 0F 11 86 68 03 00 00",
        "expected_offset": 0x2DD7BB,
        "expected_va": 0x1402DE3BB,
        "length": 8,
        "patch_length": 8,
        "description": "movss [rsi+368h], xmm0 updating vertical camera height"
    },
    "zoom_write": {
        "name": "Zoom write",
        "pattern": "F3 44 0F 11 96 5C 06 00 00",
        "expected_offset": 0x2DD7F7,
        "expected_va": 0x1402DE3F7,
        "length": 9,
        "patch_length": 9,
        "description": "movss [rsi+65ch], xmm10 storing active camera zoom distance"
    },
    "gm_resolver": {
        "name": "GameManager resolver",
        "pattern": "48 8B 05 ?? ?? ?? ?? 48 8B 80 E8 1C 00 00 C3",
        "expected_offset": 0x2A7100,
        "expected_va": 0x1402A7D00,
        "length": 15,
        "patch_length": 0,
        "description": "mov rax, [rip+disp32]; mov rax, [rax+1ce8h]; ret GameManager pointer root"
    }
}

# Secondary reference signatures
SECONDARY_SIGNATURES = {
    "pitch_indoor_clamp": {
        "name": "Indoor pitch clamp",
        "pattern": "F3 0F 10 05 ?? ?? ?? ?? F3 0F 5D 3D ?? ?? ?? ?? F3 0F 5F C7",
        "expected_offset": 0x1FC046,
        "expected_va": 0x1401FCC46,
        "length": 20
    }
}

# Known colliding pattern for Tier 2 collision tests
KNOWN_COLLIDING_PATTERN = "C7 83 C0 A7 03 00 CD CC CC 3F"


# ==============================================================================
# Custom Exceptions for Exact Boundary & Failure Verification
# ==============================================================================

class AOBPatternError(ValueError):
    """Raised when an AOB pattern string is malformed or invalid."""
    pass


class PatternNotFoundError(RuntimeError):
    """Raised when an AOB pattern produces 0 matches."""
    pass


class PatternCollisionError(RuntimeError):
    """Raised when an AOB pattern is not unique (matches > 1)."""
    def __init__(self, message: str, matches: List[int]):
        super().__init__(message)
        self.matches = matches


class InvalidPEError(ValueError):
    """Raised when file data cannot be parsed as a valid PE binary."""
    pass


class SectionNotFoundError(KeyError):
    """Raised when a required PE section (.text) is missing."""
    pass


class ProcessNotAliveError(RuntimeError):
    """Raised when attempting memory operations on a dead target process."""
    pass


# ==============================================================================
# AOB Pattern Parser & Scanner
# ==============================================================================

def parse_aob(pattern_str: str) -> re.Pattern:
    """
    Parses a space-separated hex AOB string with optional wildcards ('?' or '??').
    Strictly validates token formats:
    - Rejects empty strings.
    - Rejects odd-length hex tokens (e.g. 'F', '123').
    - Rejects non-hex characters (e.g. 'ZZ', 'GG').
    - Rejects invalid wildcard syntax (e.g. '???', '?A').
    - Rejects patterns containing only wildcards with no concrete bytes.
    """
    if not isinstance(pattern_str, str):
        raise AOBPatternError(f"Pattern must be str, got {type(pattern_str).__name__}")
    
    cleaned = pattern_str.strip()
    if not cleaned:
        raise AOBPatternError("AOB pattern string cannot be empty")
    
    tokens = cleaned.split()
    regex_parts = []
    concrete_byte_count = 0

    for tok in tokens:
        if tok in ("??", "?"):
            regex_parts.append(b".")
        elif len(tok) == 2:
            try:
                byte_val = bytes.fromhex(tok)
                regex_parts.append(re.escape(byte_val))
                concrete_byte_count += 1
            except ValueError:
                raise AOBPatternError(f"Invalid non-hex token '{tok}' in pattern '{pattern_str}'")
        else:
            raise AOBPatternError(f"Malformed token '{tok}' in pattern '{pattern_str}': expected 2 hex chars or '?'/'??'")

    if concrete_byte_count == 0:
        raise AOBPatternError(f"AOB pattern '{pattern_str}' cannot consist entirely of wildcards")

    return re.compile(b"".join(regex_parts), re.DOTALL)


def scan_binary_pattern(binary_data: bytes, pattern_str: str) -> List[int]:
    """
    Scans binary_data for all occurrences of pattern_str.
    Returns list of file offset indices.
    """
    if not isinstance(binary_data, (bytes, bytearray)):
        raise TypeError(f"binary_data must be bytes or bytearray, got {type(binary_data).__name__}")
    regex = parse_aob(pattern_str)
    return [m.start() for m in regex.finditer(binary_data)]


def assert_unique_match(
    binary_data: bytes,
    pattern_str: str,
    expected_offset: Optional[int] = None,
    expected_va: Optional[int] = None,
    pe_info: Optional["PEInfo"] = None
) -> int:
    """
    Scans pattern_str across binary_data and strictly asserts 100% uniqueness:
    - Asserts exactly 1 match (raises PatternNotFoundError if 0, PatternCollisionError if > 1).
    - If expected_offset is provided, asserts match offset equals expected_offset.
    - If expected_va and pe_info are provided, asserts resolved VA equals expected_va.
    Returns the unique match file offset.
    """
    matches = scan_binary_pattern(binary_data, pattern_str)
    if len(matches) == 0:
        raise PatternNotFoundError(f"Signature '{pattern_str}' not found in binary (0 matches, expected 1)")
    if len(matches) > 1:
        raise PatternCollisionError(
            f"Signature '{pattern_str}' collided {len(matches)} times: {[hex(m) for m in matches]}",
            matches=matches
        )
    
    offset = matches[0]
    if expected_offset is not None and offset != expected_offset:
        raise AssertionError(f"Pattern matched at offset {hex(offset)}, expected {hex(expected_offset)}")
    
    if expected_va is not None:
        if pe_info is None:
            raise ValueError("pe_info required to assert expected_va")
        resolved_va = pe_info.file_offset_to_va(offset)
        if resolved_va != expected_va:
            raise AssertionError(f"Offset {hex(offset)} resolved to VA {hex(resolved_va)}, expected {hex(expected_va)}")

    return offset


# ==============================================================================
# PE Header & Section Parser (PE32+ x64)
# ==============================================================================

@dataclass
class PESection:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int


@dataclass
class PEInfo:
    file_size: int
    image_base: int
    entry_point_rva: int
    section_alignment: int
    file_alignment: int
    sections: Dict[str, PESection] = field(default_factory=dict)

    def file_offset_to_va(self, file_offset: int) -> int:
        """Translates a raw disk file offset into runtime Virtual Address (VA)."""
        for sec in self.sections.values():
            if sec.raw_offset <= file_offset < sec.raw_offset + sec.raw_size:
                rva = sec.virtual_address + (file_offset - sec.raw_offset)
                return self.image_base + rva
        # Fallback if outside mapped raw section (e.g. headers)
        return self.image_base + file_offset

    def va_to_file_offset(self, va: int) -> int:
        """Translates a runtime Virtual Address (VA) into raw disk file offset."""
        rva = va - self.image_base
        for sec in self.sections.values():
            if sec.virtual_address <= rva < sec.virtual_address + sec.virtual_size:
                return sec.raw_offset + (rva - sec.virtual_address)
        raise ValueError(f"VA {hex(va)} does not map to any file section")

    def is_in_text(self, file_offset: int) -> bool:
        """Checks if a file offset resides within the executable .text section."""
        sec = self.sections.get(".text")
        if not sec:
            return False
        return sec.raw_offset <= file_offset < sec.raw_offset + sec.raw_size


def parse_pe_headers(binary_data: bytes) -> PEInfo:
    """
    Parses PE32+ (x64) DOS, COFF, Optional, and Section headers with strict bounds checking.
    """
    if len(binary_data) < 64:
        raise InvalidPEError(f"Data size ({len(binary_data)} bytes) too small for DOS header (minimum 64 bytes)")
    
    # Check DOS magic
    if binary_data[:2] != b"MZ":
        raise InvalidPEError(f"Invalid DOS signature: expected b'MZ', got {binary_data[:2]!r}")
    
    # Read e_lfanew
    e_lfanew = struct.unpack_from("<I", binary_data, 0x3C)[0]
    if e_lfanew < 64 or e_lfanew + 4 > len(binary_data):
        raise InvalidPEError(f"Invalid e_lfanew offset {hex(e_lfanew)} for binary of size {len(binary_data)}")
    
    # Check PE signature
    pe_sig = binary_data[e_lfanew:e_lfanew+4]
    if pe_sig != b"PE\x00\x00":
        raise InvalidPEError(f"Invalid PE signature at offset {hex(e_lfanew)}: expected b'PE\\0\\0', got {pe_sig!r}")
    
    coff_offset = e_lfanew + 4
    if coff_offset + 20 > len(binary_data):
        raise InvalidPEError("Binary truncated in COFF header")
    
    machine, num_sections, _, _, _, size_of_opt_hdr, characteristics = struct.unpack_from("<HHIIIHH", binary_data, coff_offset)
    if machine != 0x8664:  # IMAGE_FILE_MACHINE_AMD64
        raise InvalidPEError(f"Unsupported machine architecture {hex(machine)}, expected x64 AMD64 (0x8664)")
    
    opt_offset = coff_offset + 20
    if opt_offset + size_of_opt_hdr > len(binary_data):
        raise InvalidPEError("Binary truncated in Optional header")
    
    opt_magic = struct.unpack_from("<H", binary_data, opt_offset)[0]
    if opt_magic != 0x20B:  # PE32+ (64-bit)
        raise InvalidPEError(f"Unsupported optional header magic {hex(opt_magic)}, expected PE32+ (0x20B)")
    
    entry_point = struct.unpack_from("<I", binary_data, opt_offset + 16)[0]
    image_base = struct.unpack_from("<Q", binary_data, opt_offset + 24)[0]
    sec_alignment = struct.unpack_from("<I", binary_data, opt_offset + 32)[0]
    file_alignment = struct.unpack_from("<I", binary_data, opt_offset + 36)[0]

    sec_tbl_offset = opt_offset + size_of_opt_hdr
    sections: Dict[str, PESection] = {}

    for i in range(num_sections):
        entry_offset = sec_tbl_offset + (i * 40)
        if entry_offset + 40 > len(binary_data):
            raise InvalidPEError(f"Binary truncated while reading section header #{i}")
        
        name_raw = binary_data[entry_offset:entry_offset+8]
        name = name_raw.rstrip(b"\x00").decode("latin1", errors="replace")
        v_size, v_addr, r_size, r_offset = struct.unpack_from("<IIII", binary_data, entry_offset + 8)
        chars = struct.unpack_from("<I", binary_data, entry_offset + 36)[0]

        sections[name] = PESection(
            name=name,
            virtual_address=v_addr,
            virtual_size=v_size,
            raw_offset=r_offset,
            raw_size=r_size,
            characteristics=chars
        )

    if ".text" not in sections:
        raise SectionNotFoundError("Mandatory .text code section not found in PE headers")

    return PEInfo(
        file_size=len(binary_data),
        image_base=image_base,
        entry_point_rva=entry_point,
        section_alignment=sec_alignment,
        file_alignment=file_alignment,
        sections=sections
    )


# ==============================================================================
# In-Memory Process Memory & Transactional Patch Engine
# ==============================================================================

@dataclass
class PatchRecord:
    name: str
    target_va: int
    original_bytes: bytes
    patched_bytes: bytes
    is_applied: bool = False


class MockProcessMemory:
    """
    Simulates process memory space for ed8_3_PC.exe.
    Maintains a bytearray buffer indexed by Virtual Address (VA).
    Provides ReadProcessMemory, WriteProcessMemory, and process liveness controls.
    """
    def __init__(self, base_va: int = DEFAULT_IMAGE_BASE, size: int = 0x1000000):
        self.base_va = base_va
        self.size = size
        self.buffer = bytearray(size)
        self.is_alive = True
        self.is_open = True
        self.read_count = 0
        self.write_count = 0

    def _va_to_idx(self, va: int, length: int) -> Tuple[int, int]:
        if not self.is_alive:
            raise ProcessNotAliveError(f"Cannot access memory: process has terminated")
        if not self.is_open:
            raise RuntimeError("Cannot access memory: process handle is closed")
        
        idx = va - self.base_va
        if idx < 0 or idx + length > self.size:
            raise IndexError(f"Virtual Address {hex(va)} (len {length}) out of range [{hex(self.base_va)}, {hex(self.base_va + self.size)})")
        return idx, idx + length

    def read_bytes(self, va: int, length: int) -> bytes:
        idx_start, idx_end = self._va_to_idx(va, length)
        self.read_count += 1
        return bytes(self.buffer[idx_start:idx_end])

    def write_bytes(self, va: int, data: bytes) -> int:
        idx_start, idx_end = self._va_to_idx(va, len(data))
        self.buffer[idx_start:idx_end] = data
        self.write_count += 1
        return len(data)

    def write_float(self, va: int, val: float) -> None:
        self.write_bytes(va, struct.pack("<f", val))

    def read_float(self, va: int) -> float:
        return struct.unpack("<f", self.read_bytes(va, 4))[0]

    def peek_raw_bytes(self, va: int, length: int) -> bytes:
        """Inspects underlying physical buffer regardless of handle open state."""
        idx = va - self.base_va
        return bytes(self.buffer[idx:idx + length])

    def close(self) -> None:
        self.is_open = False

    def terminate_process(self) -> None:
        """Simulates target game exiting unexpectedly."""
        self.is_alive = False


class PatchEngine:
    """
    Atomic transactional memory patch engine mirroring the C++ PatchEngine design.
    - Preserves original bytes prior to patching.
    - Supports atomic apply, individual rollback, and global restore_all.
    - Global restore operates in reverse LIFO order.
    - Intercepts simulated Win32 console signals (CTRL_C_EVENT, CTRL_CLOSE_EVENT).
    - Safely skips defunct writes if target process has crashed.
    """
    def __init__(self, process_mem: MockProcessMemory):
        self.mem = process_mem
        self.registry: List[PatchRecord] = []
        self.signal_history: List[str] = []

    def register_and_apply(self, name: str, target_va: int, patch_bytes: bytes) -> PatchRecord:
        """Reads original bytes, applies patch bytes, and logs record."""
        # Check if already registered
        for rec in self.registry:
            if rec.name == name and rec.target_va == target_va:
                if not rec.is_applied:
                    self.mem.write_bytes(target_va, patch_bytes)
                    rec.is_applied = True
                return rec

        orig = self.mem.read_bytes(target_va, len(patch_bytes))
        self.mem.write_bytes(target_va, patch_bytes)
        rec = PatchRecord(
            name=name,
            target_va=target_va,
            original_bytes=orig,
            patched_bytes=patch_bytes,
            is_applied=True
        )
        self.registry.append(rec)
        return rec

    def rollback(self, name: str) -> bool:
        """Rolls back a single patch by name."""
        for rec in self.registry:
            if rec.name == name and rec.is_applied:
                if self.mem.is_alive:
                    self.mem.write_bytes(rec.target_va, rec.original_bytes)
                rec.is_applied = False
                return True
        return False

    def restore_all(self) -> int:
        """
        Restores all applied patches in reverse LIFO order.
        Returns number of patches restored.
        """
        if not self.mem.is_alive:
            # Game already dead; skip writes safely
            for rec in self.registry:
                rec.is_applied = False
            return 0

        restored_count = 0
        for rec in reversed(self.registry):
            if rec.is_applied:
                self.mem.write_bytes(rec.target_va, rec.original_bytes)
                rec.is_applied = False
                restored_count += 1
        return restored_count

    def handle_signal(self, signal_name: str) -> int:
        """Simulates Win32 ConsoleCtrlHandler signal invocation."""
        self.signal_history.append(signal_name)
        return self.restore_all()


# ==============================================================================
# Camera Controller Simulator
# ==============================================================================

class CameraControllerSimulator:
    """
    High-fidelity simulation of Cold Steel III camera controller:
    - Pitch range: default clamped to [5.0f, 40.0f]. When unlocked, [-89.0f, +89.0f].
    - Zoom range: default clamped to [1.6f, 8.0f] (or 12.0f). When unlocked, [0.5f, 50.0f].
    - Altitude: defaults to Eye Y base; user can shift up/down with precision modifiers.
    - Binds NOP patches to Pitch Clamp VA (0x1401FCD2A, 16 NOPs) and Zoom Clamp VA (0x1402DE35C, 9 NOPs).
    """
    def __init__(self, memory: Optional[MockProcessMemory] = None):
        self.mem = memory or MockProcessMemory()
        self.engine = PatchEngine(self.mem)

        # Addresses
        self.va_pitch_clamp = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
        self.va_zoom_clamp = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
        self.va_alt_write = AOB_SIGNATURES["altitude_write"]["expected_va"]
        self.va_zoom_write = AOB_SIGNATURES["zoom_write"]["expected_va"]

        # Camera struct offsets relative to mock controller base
        self.controller_base = 0x140500000
        self.eye_x_va = self.controller_base + 0x364
        self.eye_y_va = self.controller_base + 0x368  # Altitude
        self.eye_z_va = self.controller_base + 0x36C
        self.zoom_va = self.controller_base + 0x65C
        self.pitch_va = self.controller_base + 0x120  # Pitch stored value

        # Defaults
        self.default_pitch = 20.0  # degrees
        self.default_zoom = 5.0    # units
        self.default_altitude = 1.8 # meters
        self.current_pitch = self.default_pitch
        self.current_zoom = self.default_zoom
        self.current_altitude = self.default_altitude

        # Seed memory with initial instructions and camera parameters
        self._seed_initial_state()

    def _seed_initial_state(self) -> None:
        # Seed pitch clamp instructions (16 bytes)
        pitch_sig = bytes.fromhex(AOB_SIGNATURES["pitch_clamp"]["pattern"].replace(" ", ""))
        self.mem.write_bytes(self.va_pitch_clamp, pitch_sig)

        # Seed zoom clamp instructions (9 bytes)
        zoom_sig = bytes.fromhex(AOB_SIGNATURES["zoom_max_clamp"]["pattern"].replace(" ", ""))
        self.mem.write_bytes(self.va_zoom_clamp, zoom_sig)

        # Seed altitude write instruction (8 bytes)
        alt_sig = bytes.fromhex(AOB_SIGNATURES["altitude_write"]["pattern"].replace(" ", ""))
        self.mem.write_bytes(self.va_alt_write, alt_sig)

        # Seed initial camera coordinate floats
        self.mem.write_float(self.eye_x_va, 0.0)
        self.mem.write_float(self.eye_y_va, self.default_altitude)
        self.mem.write_float(self.eye_z_va, 0.0)
        self.mem.write_float(self.zoom_va, self.default_zoom)
        self.mem.write_float(self.pitch_va, self.default_pitch)

    @property
    def is_pitch_unlocked(self) -> bool:
        if not self.mem.is_open:
            return any(r.name == "pitch_clamp" and r.is_applied for r in self.engine.registry)
        bytes_at = self.mem.read_bytes(self.va_pitch_clamp, 16)
        return bytes_at == b"\x90" * 16

    @property
    def is_zoom_unlocked(self) -> bool:
        if not self.mem.is_open:
            return any(r.name == "zoom_clamp" and r.is_applied for r in self.engine.registry)
        bytes_at = self.mem.read_bytes(self.va_zoom_clamp, 9)
        return bytes_at == b"\x90" * 9

    def toggle_pitch_unlock(self, enable: Optional[bool] = None) -> bool:
        new_state = (not self.is_pitch_unlocked) if enable is None else enable
        if new_state:
            self.engine.register_and_apply("pitch_clamp", self.va_pitch_clamp, b"\x90" * 16)
        else:
            self.engine.rollback("pitch_clamp")
        return self.is_pitch_unlocked

    def toggle_zoom_unlock(self, enable: Optional[bool] = None) -> bool:
        new_state = (not self.is_zoom_unlocked) if enable is None else enable
        if new_state:
            self.engine.register_and_apply("zoom_clamp", self.va_zoom_clamp, b"\x90" * 9)
        else:
            self.engine.rollback("zoom_clamp")
        return self.is_zoom_unlocked

    def adjust_altitude(self, delta: float, modifier: float = 1.0) -> float:
        self.current_altitude += (delta * modifier)
        self.mem.write_float(self.eye_y_va, self.current_altitude)
        return self.current_altitude

    def adjust_zoom(self, delta: float, modifier: float = 1.0) -> float:
        raw_zoom = self.current_zoom + (delta * modifier)
        if not self.is_zoom_unlocked:
            # Apply game's standard [1.6f, 8.0f] clamp
            self.current_zoom = max(1.6, min(8.0, raw_zoom))
        else:
            # Unlocked allows broad zoom [0.5f, 50.0f]
            self.current_zoom = max(0.5, min(50.0, raw_zoom))
        self.mem.write_float(self.zoom_va, self.current_zoom)
        return self.current_zoom

    def adjust_pitch(self, delta: float, modifier: float = 1.0) -> float:
        raw_pitch = self.current_pitch + (delta * modifier)
        if not self.is_pitch_unlocked:
            # Apply game's standard [5.0f, 40.0f] clamp
            self.current_pitch = max(5.0, min(40.0, raw_pitch))
        else:
            # Unlocked allows vertical tilt [-89.0f, +89.0f]
            self.current_pitch = max(-89.0, min(89.0, raw_pitch))
        self.mem.write_float(self.pitch_va, self.current_pitch)
        return self.current_pitch

    def reset_defaults(self) -> None:
        self.current_pitch = self.default_pitch
        self.current_zoom = self.default_zoom
        self.current_altitude = self.default_altitude
        self.mem.write_float(self.eye_y_va, self.default_altitude)
        self.mem.write_float(self.zoom_va, self.default_zoom)
        self.mem.write_float(self.pitch_va, self.default_pitch)

    def detach(self) -> None:
        """Restores memory patches and cleans up session."""
        self.engine.restore_all()
        self.mem.close()
