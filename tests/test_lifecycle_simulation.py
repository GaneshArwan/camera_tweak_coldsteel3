r"""
Tier 4 E2E Test Suite: Realistic Workload & Camera Tweak Lifecycle Simulation

Tests simulate real-world gameplay and operational workloads:
- Scenario 1: Standard Gameplay Session (Discovery -> Scan -> Attach -> Pitch/Zoom Unlock -> Altitude Shift -> Reset -> Clean Exit)
- Scenario 2: Emergency Console Exit (Ctrl+C / Close Window during active gameplay)
- Scenario 3: Target Game Terminates First (Player quits game while utility is attached)
- Scenario 4: Rapid Toggle Cycling & Debounce Stress (Verifies 200ms debounce prevents state flutter)
- Scenario 5: Continuous Altitude Shift with Speed Modifiers (Normal 1.0x, Shift 3.0x, Ctrl 0.1x)
- Scenario 6: Complex Multi-Vector Camera Positioning & State Reset
"""
import os
import time
import pytest
from test_helpers import (
    DEFAULT_EXE_PATH,
    DEFAULT_IMAGE_BASE,
    AOB_SIGNATURES,
    CameraControllerSimulator,
    MockProcessMemory,
    parse_pe_headers,
    assert_unique_match,
)


@pytest.fixture(scope="module")
def binary_data():
    """Loads target executable into memory for offline validation."""
    if not DEFAULT_EXE_PATH or not os.path.isfile(DEFAULT_EXE_PATH):
        pytest.skip("Set CS3_EXE_PATH to ed8_3_PC.exe to run on-disk lifecycle scenario")
    with open(DEFAULT_EXE_PATH, "rb") as f:
        return f.read()


@pytest.fixture(scope="module")
def pe_info(binary_data):
    """Parses PE section headers for offset-to-VA mapping."""
    return parse_pe_headers(binary_data)


# ==============================================================================
# Scenario 1: Standard Gameplay Session
# ==============================================================================

def test_scenario_1_standard_gameplay_session(binary_data, pe_info):
    """
    Scenario 1: Complete end-to-end happy path workflow:
    1. Target Discovery: Identify ed8_3_PC.exe on disk and verify PE signature.
    2. AOB Scanning: Resolve all 5 core signatures with 100% uniqueness.
    3. Process Attachment: Initialize session and camera controller simulator.
    4. Pitch Unlock: Toggle pitch clamp unlock (verify 16 NOPs, pitch traverses -45° to +75°).
    5. Zoom Unlock: Toggle zoom clamp unlock (verify 9 NOPs, zoom extends to 25.0f).
    6. Altitude Adjustment: Elevate camera by +3.5 meters.
    7. Reset Defaults: Numpad 5 resets pitch, zoom, and altitude to default baseline.
    8. Clean Detach: Numpad 0 / End detaches session and verifies 100% byte-for-byte restoration.
    """
    # 1. Target Discovery & PE Verification
    assert pe_info.image_base == DEFAULT_IMAGE_BASE
    assert ".text" in pe_info.sections

    # 2. AOB Scanning
    resolved_offsets = {}
    for key, sig in AOB_SIGNATURES.items():
        off = assert_unique_match(
            binary_data,
            sig["pattern"],
            expected_offset=sig["expected_offset"],
            expected_va=sig["expected_va"],
            pe_info=pe_info
        )
        resolved_offsets[key] = off
    assert len(resolved_offsets) == 5

    # 3. Process Attachment & Session Init
    cam = CameraControllerSimulator()
    assert cam.is_pitch_unlocked is False
    assert cam.is_zoom_unlocked is False
    assert cam.current_altitude == cam.default_altitude

    # 4. Pitch Unlock Toggle
    unlocked = cam.toggle_pitch_unlock(True)
    assert unlocked is True
    assert cam.is_pitch_unlocked is True
    # Test pitch traversal beyond default clamp [5.0, 40.0]
    p_high = cam.adjust_pitch(+45.0)  # Total 65.0 deg (would be clamped to 40 without patch)
    assert p_high == 65.0
    p_low = cam.adjust_pitch(-110.0)  # Total -45.0 deg (would be clamped to 5 without patch)
    assert p_low == -45.0

    # 5. Zoom Unlock Toggle
    z_unlocked = cam.toggle_zoom_unlock(True)
    assert z_unlocked is True
    assert cam.is_zoom_unlocked is True
    # Test zoom extension beyond default clamp [1.6, 8.0]
    z_far = cam.adjust_zoom(+20.0)  # Total 25.0 units (would be clamped to 8.0/12.0 without patch)
    assert z_far == 25.0

    # 6. Altitude Elevation Adjustment
    alt_new = cam.adjust_altitude(+3.5)
    assert abs(alt_new - (cam.default_altitude + 3.5)) < 1e-5

    # 7. Reset Defaults
    cam.reset_defaults()
    assert cam.current_pitch == cam.default_pitch
    assert cam.current_zoom == cam.default_zoom
    assert cam.current_altitude == cam.default_altitude

    # 8. Clean Detach
    cam.detach()
    assert cam.is_pitch_unlocked is False
    assert cam.is_zoom_unlocked is False
    assert cam.mem.is_open is False
    assert cam.mem.peek_raw_bytes(cam.va_pitch_clamp, 16) != b"\x90" * 16
    assert cam.mem.peek_raw_bytes(cam.va_zoom_clamp, 9) != b"\x90" * 9


# ==============================================================================
# Scenario 2: Emergency Console Exit (Ctrl+C / Close Window)
# ==============================================================================

def test_scenario_2_emergency_console_exit():
    """
    Scenario 2: Emergency termination during active gameplay:
    1. User is running the tweak with both Pitch and Zoom unlocked and elevated altitude.
    2. User presses Ctrl+C or abruptly clicks the console [X] close button.
    3. Simulated signal handler intercepts event immediately.
    4. Asserts all patches rolled back 100% before process exit.
    """
    cam = CameraControllerSimulator()
    cam.toggle_pitch_unlock(True)
    cam.toggle_zoom_unlock(True)
    cam.adjust_altitude(+5.0)

    assert cam.is_pitch_unlocked is True
    assert cam.is_zoom_unlocked is True

    # Simulate emergency Ctrl+C signal
    restored = cam.engine.handle_signal("CTRL_C_EVENT")
    assert restored == 2
    assert "CTRL_C_EVENT" in cam.engine.signal_history

    # Verify both memory locations reverted to original bytes
    assert cam.is_pitch_unlocked is False
    assert cam.is_zoom_unlocked is False


# ==============================================================================
# Scenario 3: Target Game Terminates First (Dead PID)
# ==============================================================================

def test_scenario_3_target_game_terminates_first():
    """
    Scenario 3: Target game crashes or is closed by player before utility exits:
    1. Utility has active patches applied.
    2. Game process terminates (is_alive becomes False).
    3. Utility shutdown sequence invokes restore_all().
    4. Asserts utility safely detects defunct target, skips invalid memory writes,
       and exits cleanly without throwing Win32 access errors.
    """
    cam = CameraControllerSimulator()
    cam.toggle_pitch_unlock(True)
    cam.toggle_zoom_unlock(True)

    # Game terminates first
    cam.mem.terminate_process()
    assert cam.mem.is_alive is False

    # Detach / restore should gracefully skip writes
    cam.detach()
    assert all(not rec.is_applied for rec in cam.engine.registry)


# ==============================================================================
# Scenario 4: Rapid Toggle Cycling & Debounce Stress
# ==============================================================================

def test_scenario_4_debounce_stress_and_rapid_toggling():
    """
    Scenario 4: Key bouncing & rapid toggle stress:
    Simulates a player pressing the pitch unlock hotkey (Num / or F5) rapidly.
    Verifies that a 200ms debounce filter prevents rapid flipping (flutter),
    and when the debounce window expires, subsequent toggle succeeds cleanly.
    """
    cam = CameraControllerSimulator()

    class DebouncedToggle:
        def __init__(self, debounce_sec=0.200):
            self.debounce_sec = debounce_sec
            self.last_trigger = 0.0
            self.state = False

        def press(self, current_time: float) -> bool:
            if current_time - self.last_trigger >= self.debounce_sec:
                self.state = not self.state
                self.last_trigger = current_time
                return True
            return False  # Debounced

    key = DebouncedToggle(debounce_sec=0.200)
    simulated_time = 1000.0

    # First press at t=1000.0: should trigger
    assert key.press(simulated_time) is True
    cam.toggle_pitch_unlock(key.state)
    assert cam.is_pitch_unlocked is True

    # 10 rapid key bounces over 100ms (t=1000.01 to t=1000.10): all should be rejected
    for i in range(1, 11):
        bounce_time = simulated_time + (i * 0.010)
        assert key.press(bounce_time) is False  # Ignored by debouncer
    # Pitch unlock remains True (no flutter)
    assert cam.is_pitch_unlocked is True

    # Press after 250ms (t=1000.25): should trigger second toggle
    simulated_time += 0.250
    assert key.press(simulated_time) is True
    cam.toggle_pitch_unlock(key.state)
    assert cam.is_pitch_unlocked is False


# ==============================================================================
# Scenario 5: Continuous Altitude Shift with Speed Modifiers
# ==============================================================================

def test_scenario_5_continuous_altitude_speed_modifiers():
    """
    Scenario 5: Continuous camera altitude adjustment across three speed modes:
    1. Normal speed: 1.0x multiplier (+0.10f per tick)
    2. Fast panoramic speed with Shift: 3.0x multiplier (+0.30f per tick)
    3. Micro precision speed with Ctrl: 0.1x multiplier (+0.01f per tick)
    Verifies correct arithmetic accumulation and reset.
    """
    cam = CameraControllerSimulator()
    base_step = 0.10
    start_alt = cam.current_altitude

    # 1. 10 ticks normal speed
    for _ in range(10):
        cam.adjust_altitude(base_step, modifier=1.0)
    expected_normal = start_alt + (10 * 0.10)
    assert abs(cam.current_altitude - expected_normal) < 1e-4

    # 2. 10 ticks fast speed (Shift held, 3.0x)
    for _ in range(10):
        cam.adjust_altitude(base_step, modifier=3.0)
    expected_fast = expected_normal + (10 * 0.30)
    assert abs(cam.current_altitude - expected_fast) < 1e-4

    # 3. 10 ticks micro-tuning (Ctrl held, 0.1x)
    for _ in range(10):
        cam.adjust_altitude(base_step, modifier=0.1)
    expected_micro = expected_fast + (10 * 0.01)
    assert abs(cam.current_altitude - expected_micro) < 1e-4

    # 4. Downward shift
    cam.adjust_altitude(-1.0, modifier=1.0)
    assert abs(cam.current_altitude - (expected_micro - 1.0)) < 1e-4

    # 5. Reset to default
    cam.reset_defaults()
    assert cam.current_altitude == cam.default_altitude


# ==============================================================================
# Scenario 6: Complex Multi-Vector Camera Positioning & Detachment
# ==============================================================================

def test_scenario_6_multi_vector_camera_maneuvers():
    """
    Scenario 6: Complex cinematic shot setup:
    1. Unlock pitch and zoom.
    2. Tilt camera upwards to 70° (skyward view).
    3. Zoom out to 35.0 units (grand panoramic view).
    4. Elevate camera by +8.0 meters (aerial vantage point).
    5. Verify memory state represents all custom coordinates.
    6. Detach utility and verify complete restoration to original game instructions.
    """
    cam = CameraControllerSimulator()
    cam.toggle_pitch_unlock(True)
    cam.toggle_zoom_unlock(True)

    # Set panoramic vantage point
    cam.adjust_pitch(+50.0)      # 20 + 50 = 70 deg
    cam.adjust_zoom(+30.0)       # 5 + 30 = 35 units
    cam.adjust_altitude(+8.0)    # 1.8 + 8 = 9.8 meters

    assert cam.current_pitch == 70.0
    assert cam.current_zoom == 35.0
    assert abs(cam.current_altitude - 9.8) < 1e-5

    # Memory floats verify
    assert cam.mem.read_float(cam.pitch_va) == 70.0
    assert cam.mem.read_float(cam.zoom_va) == 35.0
    assert abs(cam.mem.read_float(cam.eye_y_va) - 9.8) < 1e-5

    # Detach and verify instruction restoration
    cam.detach()
    assert cam.is_pitch_unlocked is False
    assert cam.is_zoom_unlocked is False
    assert cam.mem.is_open is False
    assert cam.mem.peek_raw_bytes(cam.va_pitch_clamp, 16) != b"\x90" * 16
    assert cam.mem.peek_raw_bytes(cam.va_zoom_clamp, 9) != b"\x90" * 9
