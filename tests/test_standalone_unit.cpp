#include "../src/hotkey_manager.hpp"
#include "../src/camera_patcher.hpp"
#include "../src/common.hpp"

#include <windows.h>
#include <iostream>
#include <vector>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>

using namespace ColdSteel3;

static int g_passedTests = 0;
static int g_failedTests = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] Line " << __LINE__ << ": " << msg << std::endl; \
            g_failedTests++; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        std::cout << "[RUN ] " << #fn << "..." << std::endl; \
        if (fn()) { \
            std::cout << "[PASS] " << #fn << std::endl; \
            g_passedTests++; \
        } else { \
            std::cout << "[FAIL] " << #fn << std::endl; \
        } \
    } while (0)

// Helper structure for mock key states
struct MockKeyboard {
    std::array<bool, 256> keys{};

    void Reset() {
        keys.fill(false);
    }

    void SetKeyDown(int vKey, bool down = true) {
        if (vKey >= 0 && vKey < 256) {
            keys[vKey] = down;
        }
    }

    bool IsDown(int vKey) const {
        if (vKey >= 0 && vKey < 256) {
            return keys[vKey];
        }
        return false;
    }
};

// ===========================================================================
// 1. HotkeyManager Initialization & Configuration Tests
// ===========================================================================

bool Test_HotkeyManager_DefaultInitialization() {
    HotkeyManager mgr;

    TEST_ASSERT(!mgr.IsRunning(), "Manager should not be running initially");
    TEST_ASSERT(std::abs(mgr.GetAltitudeStep() - DEFAULT_ALTITUDE_STEP) < 0.001f, "Default altitude step mismatch");
    TEST_ASSERT(std::abs(mgr.GetZoomStep() - DEFAULT_ZOOM_STEP) < 0.001f, "Default zoom step mismatch");
    TEST_ASSERT(std::abs(mgr.GetPitchStep() - DEFAULT_PITCH_STEP) < 0.001f, "Default pitch step mismatch");

    // Check default bindings
    KeyBinding pitchBinding = mgr.GetBinding(HotkeyAction::TogglePitch);
    TEST_ASSERT(pitchBinding.primaryKey == VK_F1, "TogglePitch primary binding mismatch");
    TEST_ASSERT(pitchBinding.secondaryKey == VK_F5, "TogglePitch secondary binding mismatch");

    KeyBinding zoomBinding = mgr.GetBinding(HotkeyAction::ToggleZoom);
    TEST_ASSERT(zoomBinding.primaryKey == VK_F2, "ToggleZoom primary binding mismatch");
    TEST_ASSERT(zoomBinding.secondaryKey == VK_F6, "ToggleZoom secondary binding mismatch");

    KeyBinding altUpBinding = mgr.GetBinding(HotkeyAction::AltitudeUp);
    TEST_ASSERT(altUpBinding.primaryKey == VK_F7, "AltitudeUp primary binding mismatch");
    TEST_ASSERT(altUpBinding.secondaryKey == VK_PRIOR, "AltitudeUp secondary binding mismatch");

    KeyBinding altDownBinding = mgr.GetBinding(HotkeyAction::AltitudeDown);
    TEST_ASSERT(altDownBinding.primaryKey == VK_F8, "AltitudeDown primary binding mismatch");
    TEST_ASSERT(altDownBinding.secondaryKey == VK_NEXT, "AltitudeDown secondary binding mismatch");

    KeyBinding resetBinding = mgr.GetBinding(HotkeyAction::ResetDefaults);
    TEST_ASSERT(resetBinding.primaryKey == VK_F9, "ResetDefaults primary binding mismatch");
    TEST_ASSERT(resetBinding.secondaryKey == VK_HOME, "ResetDefaults secondary binding mismatch");

    KeyBinding exitBinding = mgr.GetBinding(HotkeyAction::Exit);
    TEST_ASSERT(exitBinding.primaryKey == VK_F12, "Exit primary binding mismatch");
    TEST_ASSERT(exitBinding.secondaryKey == VK_END, "Exit secondary binding mismatch");

    return true;
}

// ===========================================================================
// 2. Speed Modifier Tests
// ===========================================================================

bool Test_HotkeyManager_SpeedModifiers_Normal() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    TEST_ASSERT(std::abs(mgr.CalculateSpeedMultiplier() - 1.0f) < 0.001f, "Baseline speed multiplier should be 1.0f");
    return true;
}

bool Test_HotkeyManager_SpeedModifiers_ShiftBoost() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    kbd.SetKeyDown(VK_SHIFT, true);
    TEST_ASSERT(std::abs(mgr.CalculateSpeedMultiplier() - 3.0f) < 0.001f, "Shift speed multiplier should be 3.0f");
    return true;
}

bool Test_HotkeyManager_SpeedModifiers_ControlPrecision() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    kbd.SetKeyDown(VK_CONTROL, true);
    TEST_ASSERT(std::abs(mgr.CalculateSpeedMultiplier() - 0.1f) < 0.001f, "Control speed multiplier should be 0.1f");
    return true;
}

bool Test_HotkeyManager_SpeedModifiers_BothShiftAndControl() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    kbd.SetKeyDown(VK_SHIFT, true);
    kbd.SetKeyDown(VK_CONTROL, true);
    TEST_ASSERT(std::abs(mgr.CalculateSpeedMultiplier() - 0.3f) < 0.001f, "Both Shift and Ctrl should produce 0.3f");
    return true;
}

// ===========================================================================
// 3. Edge-Triggered Debouncing & Lockout Tests
// ===========================================================================

bool Test_HotkeyManager_EdgeTrigger_TogglePitch() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    int pitchToggles = 0;
    mgr.SetOnTogglePitch([&]() { pitchToggles++; });

    auto t0 = std::chrono::steady_clock::now();

    // 1. Initial tick with key UP -> no dispatch
    mgr.Update(t0);
    TEST_ASSERT(pitchToggles == 0, "No dispatch when key is up");

    // 2. Press primary key (Num /) -> fires once
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    TEST_ASSERT(pitchToggles == 1, "First press should fire onTogglePitch");

    // 3. Keep key held down across multiple updates -> must NOT fire again (edge-trigger)
    mgr.Update(t0 + std::chrono::milliseconds(32));
    mgr.Update(t0 + std::chrono::milliseconds(48));
    mgr.Update(t0 + std::chrono::milliseconds(64));
    TEST_ASSERT(pitchToggles == 1, "Held key must not fire repeatedly");

    // 4. Release key
    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t0 + std::chrono::milliseconds(80));
    TEST_ASSERT(pitchToggles == 1, "Release should not fire onTogglePitch");

    return true;
}

bool Test_HotkeyManager_SecondaryKey_ToggleZoom() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    int zoomToggles = 0;
    mgr.SetOnToggleZoom([&]() { zoomToggles++; });

    auto t0 = std::chrono::steady_clock::now();

    // Press secondary key (F6) -> should fire
    kbd.SetKeyDown(VK_F6, true);
    mgr.Update(t0);
    TEST_ASSERT(zoomToggles == 1, "Secondary key F6 should fire onToggleZoom");

    // Release
    kbd.SetKeyDown(VK_F6, false);
    mgr.Update(t0 + std::chrono::milliseconds(20));

    // Press primary key (Num *) after debounce window (250ms)
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(250));
    TEST_ASSERT(zoomToggles == 2, "Primary key Num * should fire onToggleZoom");

    return true;
}

bool Test_HotkeyManager_Debounce_200msLockout() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    int resetCount = 0;
    mgr.SetOnResetDefaults([&]() { resetCount++; });

    auto t0 = std::chrono::steady_clock::now();

    // Press 1 at t = 0 ms -> fires (first fire)
    kbd.SetKeyDown(VK_F9, true);
    mgr.Update(t0);
    TEST_ASSERT(resetCount == 1, "Press 1 should fire");

    // Release at t = 30 ms
    kbd.SetKeyDown(VK_F9, false);
    mgr.Update(t0 + std::chrono::milliseconds(30));

    // Rapid repress at t = 80 ms (< 200 ms lockout) -> must be ignored!
    kbd.SetKeyDown(VK_F9, true);
    mgr.Update(t0 + std::chrono::milliseconds(80));
    TEST_ASSERT(resetCount == 1, "Repress within 80 ms must be suppressed by 200 ms lockout");

    // Release at t = 120 ms
    kbd.SetKeyDown(VK_F9, false);
    mgr.Update(t0 + std::chrono::milliseconds(120));

    // Another rapid repress at t = 180 ms (< 200 ms lockout) -> must be ignored!
    kbd.SetKeyDown(VK_F9, true);
    mgr.Update(t0 + std::chrono::milliseconds(180));
    TEST_ASSERT(resetCount == 1, "Repress at 180 ms must still be suppressed");

    // Release at t = 190 ms
    kbd.SetKeyDown(VK_F9, false);
    mgr.Update(t0 + std::chrono::milliseconds(190));

    // Repress at t = 220 ms (> 200 ms lockout) -> must fire!
    kbd.SetKeyDown(VK_F9, true);
    mgr.Update(t0 + std::chrono::milliseconds(220));
    TEST_ASSERT(resetCount == 2, "Press at 220 ms (> 200 ms lockout) must fire");

    return true;
}

// ===========================================================================
// 4. Continuous Adjustment & Modifier Tests
// ===========================================================================

bool Test_HotkeyManager_Continuous_Altitude() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    float totalDelta = 0.0f;
    mgr.SetOnAdjustAltitude([&](float delta) { totalDelta += delta; });

    auto t0 = std::chrono::steady_clock::now();

    // 1. Hold Num 8 for 3 ticks at normal speed (0.25f per tick)
    kbd.SetKeyDown(VK_F7, true);
    mgr.Update(t0);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    mgr.Update(t0 + std::chrono::milliseconds(32));
    TEST_ASSERT(std::abs(totalDelta - 0.75f) < 0.001f, "3 normal ticks should yield 0.75f");

    // 2. Add Shift modifier (3.0x boost -> 0.75f per tick)
    kbd.SetKeyDown(VK_SHIFT, true);
    mgr.Update(t0 + std::chrono::milliseconds(48));
    TEST_ASSERT(std::abs(totalDelta - 1.50f) < 0.001f, "Shift boosted tick should add 0.75f (total 1.50f)");

    // 3. Switch to Alt Down (Num 2) with Ctrl modifier (0.1x micro -> -0.025f per tick)
    kbd.SetKeyDown(VK_F7, false);
    kbd.SetKeyDown(VK_SHIFT, false);
    kbd.SetKeyDown(VK_F8, true);
    kbd.SetKeyDown(VK_CONTROL, true);

    mgr.Update(t0 + std::chrono::milliseconds(64));
    TEST_ASSERT(std::abs(totalDelta - 1.475f) < 0.001f, "Ctrl micro tick should subtract 0.025f (total 1.475f)");

    return true;
}

bool Test_HotkeyManager_Continuous_AlternativeKeys() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    float totalDelta = 0.0f;
    mgr.SetOnAdjustAltitude([&](float delta) { totalDelta += delta; });

    auto t0 = std::chrono::steady_clock::now();

    // Page Up (VK_PRIOR) raises altitude
    kbd.SetKeyDown(VK_PRIOR, true);
    mgr.Update(t0);
    TEST_ASSERT(std::abs(totalDelta - 0.25f) < 0.001f, "Page Up should raise altitude");

    // Page Down (VK_NEXT) lowers altitude
    kbd.SetKeyDown(VK_PRIOR, false);
    kbd.SetKeyDown(VK_NEXT, true);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    TEST_ASSERT(std::abs(totalDelta - 0.0f) < 0.001f, "Page Down should lower altitude");

    return true;
}

bool Test_HotkeyManager_Continuous_ZoomAndPitch() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    float zoomDelta = 0.0f;
    float pitchDelta = 0.0f;
    mgr.SetOnAdjustZoom([&](float d) { zoomDelta += d; });
    mgr.SetOnAdjustPitch([&](float d) { pitchDelta += d; });

    auto t0 = std::chrono::steady_clock::now();

    // Num 9: Zoom In (negative distance)
    kbd.SetKeyDown(VK_INSERT, true);
    mgr.Update(t0);
    TEST_ASSERT(zoomDelta < 0.0f, "Zoom In should produce negative delta");

    // Num 3: Zoom Out (positive distance)
    kbd.SetKeyDown(VK_INSERT, false);
    kbd.SetKeyDown(VK_DELETE, true);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    TEST_ASSERT(std::abs(zoomDelta) < 0.001f, "Zoom Out should reverse Zoom In");

    // Num 7: Pitch Up
    kbd.SetKeyDown(VK_DELETE, false);
    kbd.SetKeyDown(VK_F3, true);
    mgr.Update(t0 + std::chrono::milliseconds(32));
    TEST_ASSERT(pitchDelta > 0.0f, "Pitch Up should produce positive delta");

    // Num 1: Pitch Down
    kbd.SetKeyDown(VK_F3, false);
    kbd.SetKeyDown(VK_F4, true);
    mgr.Update(t0 + std::chrono::milliseconds(48));
    TEST_ASSERT(std::abs(pitchDelta) < 0.001f, "Pitch Down should reverse Pitch Up");

    return true;
}

bool Test_HotkeyManager_ExitCallback() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    bool exitTriggered = false;
    mgr.SetOnExit([&]() { exitTriggered = true; });

    auto t0 = std::chrono::steady_clock::now();

    // Press Num 0
    kbd.SetKeyDown(VK_F12, true);
    mgr.Update(t0);
    TEST_ASSERT(exitTriggered, "Num 0 should trigger exit callback");

    return true;
}

bool Test_HotkeyManager_StateChangedObserver() {
    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    int stateChanges = 0;
    mgr.SetOnStateChanged([&]() { stateChanges++; });
    mgr.SetOnTogglePitch([]() {});

    auto t0 = std::chrono::steady_clock::now();

    // Trigger toggle action
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0);
    TEST_ASSERT(stateChanges == 1, "Toggle action should trigger stateChanged observer");

    // Trigger continuous action
    mgr.SetOnAdjustAltitude([](float) {});
    kbd.SetKeyDown(VK_F1, false);
    kbd.SetKeyDown(VK_F7, true);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    TEST_ASSERT(stateChanges == 2, "Continuous action should trigger stateChanged observer");

    return true;
}

// ===========================================================================
// 5. Patcher Binding Integration Tests
// ===========================================================================

bool Test_HotkeyManager_PatcherBindingIntegration() {
    // Allocate simulated target process memory
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    ));
    TEST_ASSERT(mem != nullptr, "Failed to allocate mock memory");

    uintptr_t pitchAddr = reinterpret_cast<uintptr_t>(mem + 0x1000);
    uintptr_t zoomAddr = reinterpret_cast<uintptr_t>(mem + 0x2000);
    std::memcpy(reinterpret_cast<void*>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, 16);
    std::memcpy(reinterpret_cast<void*>(zoomAddr), SIG_ZOOM_CLAMP_PATTERN, 9);

    CameraPatcher patcher;
    patcher.SetCustomAddresses(pitchAddr, zoomAddr, 0);
    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(mem)), "Patcher failed to attach");

    HotkeyManager mgr;
    MockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int key) { return kbd.IsDown(key); });

    // Connect hotkeys directly to patcher via BindPatcher
    mgr.BindPatcher(&patcher);

    auto t0 = std::chrono::steady_clock::now();

    // 1. Press Num / -> Toggles pitch unlock
    TEST_ASSERT(!patcher.IsPitchUnlocked(), "Pitch should initially be locked");
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0);
    TEST_ASSERT(patcher.IsPitchUnlocked(), "Num / should unlock pitch");
    TEST_ASSERT(mem[0x1000] == 0x90, "Pitch memory should be patched with NOPs");

    // 2. Press Num * -> Toggles zoom unlock
    TEST_ASSERT(!patcher.IsZoomUnlocked(), "Zoom should initially be locked");
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(250));
    TEST_ASSERT(patcher.IsZoomUnlocked(), "Num * should unlock zoom");
    TEST_ASSERT(mem[0x2000] == 0x90, "Zoom memory should be patched with NOPs");

    // 3. Press Num 5 -> Reset defaults
    kbd.SetKeyDown(VK_F1, false);
    kbd.SetKeyDown(VK_F2, false);
    kbd.SetKeyDown(VK_F9, true);
    mgr.Update(t0 + std::chrono::milliseconds(500));
    TEST_ASSERT(!patcher.IsPitchUnlocked(), "ResetDefaults should relock pitch");
    TEST_ASSERT(!patcher.IsZoomUnlocked(), "ResetDefaults should relock zoom");
    TEST_ASSERT(std::memcmp(mem + 0x1000, SIG_PITCH_CLAMP_PATTERN, 16) == 0, "Pitch original bytes restored");
    TEST_ASSERT(std::memcmp(mem + 0x2000, SIG_ZOOM_CLAMP_PATTERN, 9) == 0, "Zoom original bytes restored");

    patcher.Detach();
    VirtualFree(mem, 0, MEM_RELEASE);
    return true;
}

// ===========================================================================
// 6. Signal Handler Registration Tests
// ===========================================================================

static BOOL WINAPI DummySignalHandler(DWORD) {
    return TRUE;
}

bool Test_SignalHandler_Registration() {
    // Test that SetConsoleCtrlHandler succeeds for installation and uninstallation
    BOOL installed = SetConsoleCtrlHandler(DummySignalHandler, TRUE);
    TEST_ASSERT(installed, "SetConsoleCtrlHandler install failed");

    BOOL uninstalled = SetConsoleCtrlHandler(DummySignalHandler, FALSE);
    TEST_ASSERT(uninstalled, "SetConsoleCtrlHandler uninstall failed");

    return true;
}

// ===========================================================================
// 7. Process Liveness & Termination Tests
// ===========================================================================

bool Test_CameraPatcher_ProcessLiveness_ActiveAndTerminated() {
    CameraPatcher patcher;

    // Phase 1: Unattached patcher
    TEST_ASSERT(!patcher.IsAttached(), "Patcher should not be attached initially");
    TEST_ASSERT(!patcher.IsProcessAlive(), "Unattached patcher must return false for IsProcessAlive");

    // Phase 2: Active handle (current process)
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    ));
    TEST_ASSERT(mem != nullptr, "Memory allocation failed");
    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(mem)), "Attach failed");
    TEST_ASSERT(patcher.IsAttached(), "Should be attached");
    TEST_ASSERT(patcher.IsProcessAlive(), "IsProcessAlive must be true for active process");

    patcher.Detach();
    TEST_ASSERT(!patcher.IsAttached(), "Should be detached");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false after detach");
    VirtualFree(mem, 0, MEM_RELEASE);

    // Phase 3: Real child process lifecycle (live -> terminated)
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char cmdLine[] = "cmd.exe /c pause";
    BOOL created = CreateProcessA(
        nullptr, cmdLine, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi
    );
    TEST_ASSERT(created, "Failed to spawn test child process");

    // Attach to running child process
    TEST_ASSERT(patcher.Attach(pi.hProcess, 0x140000000), "Failed to attach to child process");
    TEST_ASSERT(patcher.IsAttached(), "Should be attached to child process");
    TEST_ASSERT(patcher.IsProcessAlive(), "IsProcessAlive must return true while child process runs");

    // Terminate child process
    TerminateProcess(pi.hProcess, 42);
    WaitForSingleObject(pi.hProcess, 3000);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    TEST_ASSERT(exitCode != STILL_ACTIVE, "Child process must have terminated");

    // Critical check: IsAttached is still true, but IsProcessAlive MUST be false!
    TEST_ASSERT(patcher.IsAttached(), "IsAttached remains true before explicit Detach()");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive MUST return false for terminated process");

    // Phase 4: Recovery via Detach()
    patcher.Detach();
    TEST_ASSERT(!patcher.IsAttached(), "IsAttached must be false after Detach()");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must remain false");

    // Phase 5: Rejection of dead process handle on Attach()
    CameraPatcher deadPatcher;
    bool deadAttach = deadPatcher.Attach(pi.hProcess, 0x140000000);
    TEST_ASSERT(!deadAttach, "Attach must reject already-terminated process handle");
    TEST_ASSERT(!deadPatcher.IsAttached(), "Dead attach must leave IsAttached false");
    TEST_ASSERT(!deadPatcher.IsProcessAlive(), "Dead attach must leave IsProcessAlive false");

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// ===========================================================================
// Main Unit Test Runner
// ===========================================================================

int main() {
    std::cout << "============================================================\n";
    std::cout << " Cold Steel III - Standalone Hotkey & Integration Unit Tests\n";
    std::cout << "============================================================\n";

    RUN_TEST(Test_HotkeyManager_DefaultInitialization);
    RUN_TEST(Test_HotkeyManager_SpeedModifiers_Normal);
    RUN_TEST(Test_HotkeyManager_SpeedModifiers_ShiftBoost);
    RUN_TEST(Test_HotkeyManager_SpeedModifiers_ControlPrecision);
    RUN_TEST(Test_HotkeyManager_SpeedModifiers_BothShiftAndControl);
    RUN_TEST(Test_HotkeyManager_EdgeTrigger_TogglePitch);
    RUN_TEST(Test_HotkeyManager_SecondaryKey_ToggleZoom);
    RUN_TEST(Test_HotkeyManager_Debounce_200msLockout);
    RUN_TEST(Test_HotkeyManager_Continuous_Altitude);
    RUN_TEST(Test_HotkeyManager_Continuous_AlternativeKeys);
    RUN_TEST(Test_HotkeyManager_Continuous_ZoomAndPitch);
    RUN_TEST(Test_HotkeyManager_ExitCallback);
    RUN_TEST(Test_HotkeyManager_StateChangedObserver);
    RUN_TEST(Test_HotkeyManager_PatcherBindingIntegration);
    RUN_TEST(Test_SignalHandler_Registration);
    RUN_TEST(Test_CameraPatcher_ProcessLiveness_ActiveAndTerminated);

    std::cout << "============================================================\n";
    std::cout << " Results: Passed: " << g_passedTests << " | Failed: " << g_failedTests << "\n";
    std::cout << "============================================================\n";

    return (g_failedTests == 0) ? 0 : 1;
}
