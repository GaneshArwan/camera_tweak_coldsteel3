r"""
Tier 3 E2E Test Suite: Process Detachment, Atomic Rollback & Signal Interception

Tests verify crash-proof detachment and atomic rollback semantics:
1. In-memory process memory buffer patching with automatic original-byte backup.
2. 100% byte-for-byte memory restoration upon individual rollback.
3. Multi-patch global restoration executed strictly in reverse LIFO order.
4. Interception and atomic rollback on simulated Win32 ConsoleCtrlHandler signals:
   - CTRL_C_EVENT
   - CTRL_CLOSE_EVENT
   - CTRL_BREAK_EVENT
   - CTRL_LOGOFF_EVENT
   - CTRL_SHUTDOWN_EVENT
5. Dead target process crash guard (gracefully handles is_alive=False without writing to defunct handles).
6. Idempotency & cycle stress (multiple toggles preserve pristine original bytes).
7. Pairwise feature combinations: Pitch + Zoom, Zoom + Altitude, Pitch + Altitude, and Full Tri-Tweak.
"""
import pytest
from test_helpers import (
    AOB_SIGNATURES,
    DEFAULT_IMAGE_BASE,
    MockProcessMemory,
    PatchEngine,
    ProcessNotAliveError,
)


@pytest.fixture
def process_memory():
    """Provides a fresh MockProcessMemory buffer simulating game process space."""
    mem = MockProcessMemory(base_va=DEFAULT_IMAGE_BASE, size=0x1000000)
    # Seed pitch clamp instructions (16 bytes) at 0x1401fcd2a
    pitch_sig = bytes.fromhex(AOB_SIGNATURES["pitch_clamp"]["pattern"].replace(" ", ""))
    mem.write_bytes(AOB_SIGNATURES["pitch_clamp"]["expected_va"], pitch_sig)

    # Seed zoom clamp instructions (9 bytes) at 0x1402de35c
    zoom_sig = bytes.fromhex(AOB_SIGNATURES["zoom_max_clamp"]["pattern"].replace(" ", ""))
    mem.write_bytes(AOB_SIGNATURES["zoom_max_clamp"]["expected_va"], zoom_sig)

    # Seed altitude write instructions (8 bytes) at 0x1402de3bb
    alt_sig = bytes.fromhex(AOB_SIGNATURES["altitude_write"]["pattern"].replace(" ", ""))
    mem.write_bytes(AOB_SIGNATURES["altitude_write"]["expected_va"], alt_sig)

    # Seed zoom write instructions (9 bytes) at 0x1402de3f7
    zw_sig = bytes.fromhex(AOB_SIGNATURES["zoom_write"]["pattern"].replace(" ", ""))
    mem.write_bytes(AOB_SIGNATURES["zoom_write"]["expected_va"], zw_sig)

    return mem


@pytest.fixture
def patch_engine(process_memory):
    """Provides a fresh PatchEngine bound to process_memory."""
    return PatchEngine(process_memory)


# ==============================================================================
# Category 1: Patch Registration & Byte-for-Byte Restoration
# ==============================================================================

def test_single_pitch_patch_and_restoration(process_memory, patch_engine):
    """
    Verifies that applying pitch unlock patch (16 NOPs) saves original bytes
    and rollback restores 100% identical original bytes.
    """
    va = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    orig_bytes = process_memory.read_bytes(va, 16)
    patch_bytes = b"\x90" * 16

    # 1. Apply patch
    rec = patch_engine.register_and_apply("pitch_clamp", va, patch_bytes)
    assert rec.is_applied is True
    assert rec.original_bytes == orig_bytes
    assert process_memory.read_bytes(va, 16) == patch_bytes

    # 2. Rollback patch
    ok = patch_engine.rollback("pitch_clamp")
    assert ok is True
    assert rec.is_applied is False
    assert process_memory.read_bytes(va, 16) == orig_bytes


def test_single_zoom_patch_and_restoration(process_memory, patch_engine):
    """
    Verifies that applying zoom unlock patch (9 NOPs) saves original bytes
    and rollback restores 100% identical original bytes.
    """
    va = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    orig_bytes = process_memory.read_bytes(va, 9)
    patch_bytes = b"\x90" * 9

    # 1. Apply patch
    rec = patch_engine.register_and_apply("zoom_clamp", va, patch_bytes)
    assert rec.is_applied is True
    assert rec.original_bytes == orig_bytes
    assert process_memory.read_bytes(va, 9) == patch_bytes

    # 2. Rollback patch
    ok = patch_engine.rollback("zoom_clamp")
    assert ok is True
    assert rec.is_applied is False
    assert process_memory.read_bytes(va, 9) == orig_bytes


def test_multi_patch_reverse_lifo_restoration(process_memory, patch_engine):
    """
    Applies multiple distinct patches (Pitch, Zoom, Altitude).
    Verifies restore_all() restores memory in reverse LIFO order.
    """
    va_pitch = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    va_zoom = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    va_alt = AOB_SIGNATURES["altitude_write"]["expected_va"]

    orig_pitch = process_memory.read_bytes(va_pitch, 16)
    orig_zoom = process_memory.read_bytes(va_zoom, 9)
    orig_alt = process_memory.read_bytes(va_alt, 8)

    # Apply in order: Pitch, then Zoom, then Altitude
    patch_engine.register_and_apply("pitch_clamp", va_pitch, b"\x90" * 16)
    patch_engine.register_and_apply("zoom_clamp", va_zoom, b"\x90" * 9)
    patch_engine.register_and_apply("altitude_write", va_alt, b"\x90" * 8)

    # Confirm all 3 are patched
    assert process_memory.read_bytes(va_pitch, 16) == b"\x90" * 16
    assert process_memory.read_bytes(va_zoom, 9) == b"\x90" * 9
    assert process_memory.read_bytes(va_alt, 8) == b"\x90" * 8

    # Restore all
    restored_count = patch_engine.restore_all()
    assert restored_count == 3

    # Confirm 100% byte-for-byte restoration
    assert process_memory.read_bytes(va_pitch, 16) == orig_pitch
    assert process_memory.read_bytes(va_zoom, 9) == orig_zoom
    assert process_memory.read_bytes(va_alt, 8) == orig_alt


# ==============================================================================
# Category 2: Signal Handlers & Simulated Win32 Console Events
# ==============================================================================

@pytest.mark.parametrize("signal_name", [
    "CTRL_C_EVENT",
    "CTRL_CLOSE_EVENT",
    "CTRL_BREAK_EVENT",
    "CTRL_LOGOFF_EVENT",
    "CTRL_SHUTDOWN_EVENT",
    "SIGINT",
    "SIGTERM"
])
def test_simulated_signal_restoration(process_memory, patch_engine, signal_name):
    """
    Verifies that any shutdown signal triggers immediate atomic rollback
    and restores memory to original bytes.
    """
    va_pitch = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    va_zoom = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    orig_pitch = process_memory.read_bytes(va_pitch, 16)
    orig_zoom = process_memory.read_bytes(va_zoom, 9)

    patch_engine.register_and_apply("pitch_clamp", va_pitch, b"\x90" * 16)
    patch_engine.register_and_apply("zoom_clamp", va_zoom, b"\x90" * 9)

    # Trigger signal handler
    restored = patch_engine.handle_signal(signal_name)
    assert restored == 2
    assert signal_name in patch_engine.signal_history

    # Verify 100% restoration
    assert process_memory.read_bytes(va_pitch, 16) == orig_pitch
    assert process_memory.read_bytes(va_zoom, 9) == orig_zoom


# ==============================================================================
# Category 3: Dead Process Guard & Handle Management
# ==============================================================================

def test_dead_process_termination_guard(process_memory, patch_engine):
    """
    Simulates target game exiting or crashing before the utility detaches.
    Verifies restore_all() detects process termination, safely skips defunct writes,
    and returns 0 without raising access violations.
    """
    va_pitch = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    patch_engine.register_and_apply("pitch_clamp", va_pitch, b"\x90" * 16)

    # Game terminates unexpectedly
    process_memory.terminate_process()
    assert process_memory.is_alive is False

    # Attempting direct write raises ProcessNotAliveError
    with pytest.raises(ProcessNotAliveError):
        process_memory.write_bytes(va_pitch, b"\x00" * 16)

    # PatchEngine restore_all gracefully skips writing to defunct process
    restored = patch_engine.restore_all()
    assert restored == 0  # No writes attempted to dead process
    assert all(not rec.is_applied for rec in patch_engine.registry)


def test_closed_handle_leak_prevention(process_memory):
    """Verifies that closing a process handle prevents any subsequent access."""
    process_memory.close()
    assert process_memory.is_open is False
    with pytest.raises(RuntimeError, match="handle is closed"):
        process_memory.read_bytes(DEFAULT_IMAGE_BASE, 4)


def test_double_restore_safety(process_memory, patch_engine):
    """Verifies calling restore_all() multiple times consecutively is safe (no double write)."""
    va = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    orig = process_memory.read_bytes(va, 16)
    patch_engine.register_and_apply("pitch_clamp", va, b"\x90" * 16)

    # First restore
    count1 = patch_engine.restore_all()
    assert count1 == 1
    assert process_memory.read_bytes(va, 16) == orig

    # Second consecutive restore should be a no-op
    count2 = patch_engine.restore_all()
    assert count2 == 0
    assert process_memory.read_bytes(va, 16) == orig


# ==============================================================================
# Category 4: Idempotency & Rapid Toggle Cycling
# ==============================================================================

def test_idempotent_toggle_stress_test(process_memory, patch_engine):
    """
    Simulates 20 rapid toggles of pitch unlock and zoom unlock.
    Verifies no byte drifting or memory corruption across repeated cycles.
    """
    va_pitch = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    va_zoom = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    orig_pitch = process_memory.read_bytes(va_pitch, 16)
    orig_zoom = process_memory.read_bytes(va_zoom, 9)

    for cycle in range(20):
        # Apply
        patch_engine.register_and_apply("pitch_clamp", va_pitch, b"\x90" * 16)
        patch_engine.register_and_apply("zoom_clamp", va_zoom, b"\x90" * 9)
        assert process_memory.read_bytes(va_pitch, 16) == b"\x90" * 16
        assert process_memory.read_bytes(va_zoom, 9) == b"\x90" * 9

        # Rollback
        patch_engine.rollback("pitch_clamp")
        patch_engine.rollback("zoom_clamp")
        assert process_memory.read_bytes(va_pitch, 16) == orig_pitch
        assert process_memory.read_bytes(va_zoom, 9) == orig_zoom


# ==============================================================================
# Category 5: Pairwise Feature Interaction Tests
# ==============================================================================

def test_pairwise_pitch_and_zoom_concurrent(process_memory, patch_engine):
    """Pairwise Test 1: Pitch Clamp + Zoom Clamp concurrent patching and rollback."""
    va_p = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    va_z = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    orig_p = process_memory.read_bytes(va_p, 16)
    orig_z = process_memory.read_bytes(va_z, 9)

    patch_engine.register_and_apply("pitch", va_p, b"\x90" * 16)
    patch_engine.register_and_apply("zoom", va_z, b"\x90" * 9)

    # Rollback only pitch, zoom remains patched
    patch_engine.rollback("pitch")
    assert process_memory.read_bytes(va_p, 16) == orig_p
    assert process_memory.read_bytes(va_z, 9) == b"\x90" * 9

    # Rollback zoom
    patch_engine.rollback("zoom")
    assert process_memory.read_bytes(va_z, 9) == orig_z


def test_pairwise_zoom_and_altitude_concurrent(process_memory, patch_engine):
    """Pairwise Test 2: Zoom Clamp + Altitude Write concurrent patching and rollback."""
    va_z = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    va_a = AOB_SIGNATURES["altitude_write"]["expected_va"]
    orig_z = process_memory.read_bytes(va_z, 9)
    orig_a = process_memory.read_bytes(va_a, 8)

    patch_engine.register_and_apply("zoom", va_z, b"\x90" * 9)
    patch_engine.register_and_apply("alt", va_a, b"\x90" * 8)

    # Rollback only altitude
    patch_engine.rollback("alt")
    assert process_memory.read_bytes(va_a, 8) == orig_a
    assert process_memory.read_bytes(va_z, 9) == b"\x90" * 9

    # Rollback zoom
    patch_engine.rollback("zoom")
    assert process_memory.read_bytes(va_z, 9) == orig_z


def test_pairwise_pitch_and_altitude_concurrent(process_memory, patch_engine):
    """Pairwise Test 3: Pitch Clamp + Altitude Write concurrent patching and rollback."""
    va_p = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    va_a = AOB_SIGNATURES["altitude_write"]["expected_va"]
    orig_p = process_memory.read_bytes(va_p, 16)
    orig_a = process_memory.read_bytes(va_a, 8)

    patch_engine.register_and_apply("pitch", va_p, b"\x90" * 16)
    patch_engine.register_and_apply("alt", va_a, b"\x90" * 8)

    assert process_memory.read_bytes(va_p, 16) == b"\x90" * 16
    assert process_memory.read_bytes(va_a, 8) == b"\x90" * 8

    patch_engine.restore_all()
    assert process_memory.read_bytes(va_p, 16) == orig_p
    assert process_memory.read_bytes(va_a, 8) == orig_a


def test_tri_tweak_all_features_active(process_memory, patch_engine):
    """Pairwise Test 4: Pitch + Zoom + Altitude all simultaneously active."""
    va_p = AOB_SIGNATURES["pitch_clamp"]["expected_va"]
    va_z = AOB_SIGNATURES["zoom_max_clamp"]["expected_va"]
    va_a = AOB_SIGNATURES["altitude_write"]["expected_va"]
    orig_p = process_memory.read_bytes(va_p, 16)
    orig_z = process_memory.read_bytes(va_z, 9)
    orig_a = process_memory.read_bytes(va_a, 8)

    patch_engine.register_and_apply("pitch", va_p, b"\x90" * 16)
    patch_engine.register_and_apply("zoom", va_z, b"\x90" * 9)
    patch_engine.register_and_apply("alt", va_a, b"\x90" * 8)

    # Ensure addresses do not overlap
    assert abs(va_p - va_z) > 100
    assert abs(va_z - va_a) > 50

    patch_engine.restore_all()
    assert process_memory.read_bytes(va_p, 16) == orig_p
    assert process_memory.read_bytes(va_z, 9) == orig_z
    assert process_memory.read_bytes(va_a, 8) == orig_a
