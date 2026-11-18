#define private public
#include "../src/hotkey_manager.hpp"
#undef private
#include "../src/camera_patcher.hpp"
#include "../src/common.hpp"

#include <windows.h>
#include <iostream>
#include <vector>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <atomic>
#include <random>
#include <mutex>
#include <cassert>

using namespace ColdSteel3;

static int g_passedTests = 0;
static int g_failedTests = 0;

#define ADV_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] Line " << __LINE__ << ": " << msg << std::endl; \
            g_failedTests++; \
            return false; \
        } \
    } while (0)

#define RUN_ADV_TEST(fn) \
    do { \
        std::cout << "[RUN ] " << #fn << "..." << std::endl; \
        if (fn()) { \
            std::cout << "[PASS] " << #fn << std::endl; \
            g_passedTests++; \
        } else { \
            std::cout << "[FAIL] " << #fn << std::endl; \
        } \
    } while (0)

// Helper structure for thread-safe mock key states
struct ThreadSafeMockKeyboard {
    mutable std::mutex mtx;
    std::array<bool, 256> keys{};

    void Reset() {
        std::lock_guard<std::mutex> lock(mtx);
        keys.fill(false);
    }

    void SetKeyDown(int vKey, bool down = true) {
        if (vKey >= 0 && vKey < 256) {
            std::lock_guard<std::mutex> lock(mtx);
            keys[vKey] = down;
        }
    }

    bool IsDown(int vKey) const {
        if (vKey >= 0 && vKey < 256) {
            std::lock_guard<std::mutex> lock(mtx);
            return keys[vKey];
        }
        return false;
    }
};

// ===========================================================================
// GROUP 1: Adversarial Debounce & Rapid Keypress Stress
// ===========================================================================

// 1. Rapid Keypresses: 10,000 sub-window pulses (< 200ms) must register only once
bool Test_Adv_RapidKeypresses_SubDebounceWindow_SingleDispatch() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int toggleCount = 0;
    mgr.SetOnTogglePitch([&]() { toggleCount++; });

    auto t0 = std::chrono::steady_clock::now();

    // Initial press at t = 0 ms -> MUST fire
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0);
    ADV_ASSERT(toggleCount == 1, "Initial press at t=0ms must fire exactly once");

    // Rapid chatter: 5,000 release-press cycles within 0..199 ms
    // Spaced at 30 microsecond intervals simulated
    for (int i = 1; i <= 5000; ++i) {
        // Compute millisecond offset between 1 and 199 ms
        int64_t msOffset = 1 + (i % 198); // 1..198 ms
        auto tNow = t0 + std::chrono::milliseconds(msOffset);

        // Chatter: release then press
        kbd.SetKeyDown(VK_F1, false);
        mgr.Update(tNow);

        kbd.SetKeyDown(VK_F1, true);
        mgr.Update(tNow);

        ADV_ASSERT(toggleCount == 1, "Chatter within 200ms debounce lockout must be suppressed");
    }

    ADV_ASSERT(toggleCount == 1, "Total toggle dispatches after 5,000 chatter bursts must remain strictly 1");

    // Now advance past the 200ms lockout (e.g. t = 205 ms)
    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t0 + std::chrono::milliseconds(201));

    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0 + std::chrono::milliseconds(205));
    ADV_ASSERT(toggleCount == 2, "Press after 200ms lockout must fire second dispatch");

    return true;
}

// 2. Exact Lockout Boundaries: 199ms (rejected) vs 200ms (accepted) vs 201ms (accepted)
bool Test_Adv_Debounce_ExactLockoutBoundaries() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int triggerCount = 0;
    mgr.SetOnToggleZoom([&]() { triggerCount++; });

    auto t0 = std::chrono::steady_clock::now();

    // Event 1: Fire at t = 0 ms
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0);
    ADV_ASSERT(triggerCount == 1, "Event 1 at t=0ms must fire");

    // Release at t = 50 ms
    kbd.SetKeyDown(VK_F2, false);
    mgr.Update(t0 + std::chrono::milliseconds(50));

    // Boundary Test A: Repress at exactly 199 ms -> MUST BE SUPPRESSED
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(199));
    ADV_ASSERT(triggerCount == 1, "Boundary at 199 ms (< 200 ms) must be suppressed");

    // Release at 199 ms
    kbd.SetKeyDown(VK_F2, false);
    mgr.Update(t0 + std::chrono::milliseconds(199));

    // Boundary Test B: Repress at exactly 200 ms -> MUST FIRE (elapsed == 200)
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(200));
    ADV_ASSERT(triggerCount == 2, "Boundary at exactly 200 ms must fire");

    // Release at 250 ms
    kbd.SetKeyDown(VK_F2, false);
    mgr.Update(t0 + std::chrono::milliseconds(250));

    // Boundary Test C: Repress at 399 ms (elapsed = 199 ms since last fire at 200 ms) -> SUPPRESSED
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(399));
    ADV_ASSERT(triggerCount == 2, "Boundary at 399 ms (elapsed 199 ms) must be suppressed");

    // Release at 399 ms
    kbd.SetKeyDown(VK_F2, false);
    mgr.Update(t0 + std::chrono::milliseconds(399));

    // Boundary Test D: Repress at 401 ms (elapsed = 201 ms since last fire at 200 ms) -> MUST FIRE
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(401));
    ADV_ASSERT(triggerCount == 3, "Boundary at 401 ms (elapsed 201 ms) must fire");

    return true;
}

// 3. Multi-Action Independent Debounce: Overlapping chatter on distinct actions
bool Test_Adv_MultiAction_IndependentDebounce() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int pitchCount = 0;
    int zoomCount = 0;
    int resetCount = 0;
    int exitCount = 0;

    mgr.SetOnTogglePitch([&]() { pitchCount++; });
    mgr.SetOnToggleZoom([&]() { zoomCount++; });
    mgr.SetOnResetDefaults([&]() { resetCount++; });
    mgr.SetOnExit([&]() { exitCount++; });

    auto t0 = std::chrono::steady_clock::now();

    // Staggered triggers:
    // Pitch at t = 0 ms
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0);
    ADV_ASSERT(pitchCount == 1 && zoomCount == 0 && resetCount == 0 && exitCount == 0, "Only pitch fires at t=0");

    // Zoom at t = 50 ms
    kbd.SetKeyDown(VK_F1, false);
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(50));
    ADV_ASSERT(pitchCount == 1 && zoomCount == 1 && resetCount == 0 && exitCount == 0, "Zoom fires at t=50 without pitch debounce cross-talk");

    // Reset at t = 100 ms
    kbd.SetKeyDown(VK_F2, false);
    kbd.SetKeyDown(VK_F9, true);
    mgr.Update(t0 + std::chrono::milliseconds(100));
    ADV_ASSERT(pitchCount == 1 && zoomCount == 1 && resetCount == 1 && exitCount == 0, "Reset fires at t=100 without cross-talk");

    // Exit at t = 150 ms
    kbd.SetKeyDown(VK_F9, false);
    kbd.SetKeyDown(VK_F12, true);
    mgr.Update(t0 + std::chrono::milliseconds(150));
    ADV_ASSERT(pitchCount == 1 && zoomCount == 1 && resetCount == 1 && exitCount == 1, "Exit fires at t=150 without cross-talk");

    // Now, at t = 205 ms: Pitch is past its 200 ms lockout (0 -> 205), but Zoom is NOT (50 -> 205 = 155ms)
    kbd.SetKeyDown(VK_F12, false);
    kbd.SetKeyDown(VK_F1, true);   // Pitch
    kbd.SetKeyDown(VK_F2, true); // Zoom
    mgr.Update(t0 + std::chrono::milliseconds(205));

    ADV_ASSERT(pitchCount == 2, "Pitch elapsed is 205ms >= 200ms -> must fire");
    ADV_ASSERT(zoomCount == 1, "Zoom elapsed is 155ms < 200ms -> must still be locked out");

    // At t = 255 ms: Zoom is now past its 200 ms lockout (50 -> 255 = 205ms)
    kbd.SetKeyDown(VK_F2, false);
    mgr.Update(t0 + std::chrono::milliseconds(251));
    kbd.SetKeyDown(VK_F2, true);
    mgr.Update(t0 + std::chrono::milliseconds(255));
    ADV_ASSERT(zoomCount == 2, "Zoom now fires after its own independent lockout");

    return true;
}

// 4. Key Held Across Debounce Window: Edge-trigger guarantees zero flutter
bool Test_Adv_KeyHeld_Across_Debounce_Window_NoFlutter() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int pitchToggles = 0;
    mgr.SetOnTogglePitch([&]() { pitchToggles++; });

    auto t0 = std::chrono::steady_clock::now();

    // Press key at t = 0
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0);
    ADV_ASSERT(pitchToggles == 1, "Initial press fires");

    // Hold key continuously across 500 ticks at 16ms increments (8,000 ms total)
    for (int tick = 1; tick <= 500; ++tick) {
        mgr.Update(t0 + std::chrono::milliseconds(tick * 16));
        ADV_ASSERT(pitchToggles == 1, "Held key must NEVER trigger repeated toggle events (zero flutter)");
    }

    // Release at t = 8016 ms
    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t0 + std::chrono::milliseconds(8016));
    ADV_ASSERT(pitchToggles == 1, "Release does not fire toggle");

    // Press again at t = 8032 ms
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0 + std::chrono::milliseconds(8032));
    ADV_ASSERT(pitchToggles == 2, "Re-press after release fires cleanly");

    return true;
}

// ===========================================================================
// GROUP 2: Continuous Key Holding & Smooth Repetitive Deltas
// ===========================================================================

// 5. Continuous Holding: 10,000 consecutive ticks with exact linearity
bool Test_Adv_ContinuousHolding_10000Ticks_ExactLinearity() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int callbackCount = 0;
    double totalDelta = 0.0;
    mgr.SetOnAdjustAltitude([&](float delta) {
        callbackCount++;
        totalDelta += static_cast<double>(delta);
    });

    auto t0 = std::chrono::steady_clock::now();
    kbd.SetKeyDown(VK_F7, true); // AltitudeUp

    const int totalTicks = 10000;
    for (int i = 0; i < totalTicks; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }

    ADV_ASSERT(callbackCount == totalTicks, "All 10,000 ticks must trigger continuous altitude callback without drops");
    const double expectedDelta = totalTicks * 0.25; // 2500.0
    ADV_ASSERT(std::abs(totalDelta - expectedDelta) < 0.01, "Total cumulative delta must equal 2500.0 exactly");

    // When released, next 100 ticks must produce 0 callbacks
    kbd.SetKeyDown(VK_F7, false);
    for (int i = 0; i < 100; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds((totalTicks + i) * 16));
    }
    ADV_ASSERT(callbackCount == totalTicks, "Zero callbacks after key release");

    return true;
}

// 6. Continuous Holding: Bidirectional cancellation across long sequences
bool Test_Adv_ContinuousHolding_BidirectionalCancellation() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    float altitudeDelta = 0.0f;
    float zoomDelta = 0.0f;
    float pitchDelta = 0.0f;

    mgr.SetOnAdjustAltitude([&](float d) { altitudeDelta += d; });
    mgr.SetOnAdjustZoom([&](float d) { zoomDelta += d; });
    mgr.SetOnAdjustPitch([&](float d) { pitchDelta += d; });

    auto t0 = std::chrono::steady_clock::now();

    // 1. Altitude: 1,000 Up ticks then 1,000 Down ticks
    kbd.SetKeyDown(VK_F7, true);
    for (int i = 0; i < 1000; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(std::abs(altitudeDelta - 250.0f) < 0.01f, "1000 Up ticks should equal 250.0f");

    kbd.SetKeyDown(VK_F7, false);
    kbd.SetKeyDown(VK_F8, true);
    for (int i = 1000; i < 2000; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(std::abs(altitudeDelta) < 0.001f, "1000 Down ticks must perfectly cancel 1000 Up ticks (net 0.0f)");

    // 2. Zoom: 500 In ticks (-0.50f) then 500 Out ticks (+0.50f)
    kbd.SetKeyDown(VK_F8, false);
    kbd.SetKeyDown(VK_OEM_4, true); // ZoomIn ('[')
    for (int i = 2000; i < 2500; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(std::abs(zoomDelta - (-250.0f)) < 0.01f, "500 ZoomIn ticks should equal -250.0f");

    kbd.SetKeyDown(VK_OEM_4, false);
    kbd.SetKeyDown(VK_OEM_6, true); // ZoomOut (']')
    for (int i = 2500; i < 3000; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(std::abs(zoomDelta) < 0.001f, "500 ZoomOut ticks must perfectly cancel ZoomIn (net 0.0f)");

    // 3. Pitch: 500 Up ticks (+0.50f) then 500 Down ticks (-0.50f)
    kbd.SetKeyDown(VK_OEM_6, false);
    kbd.SetKeyDown(VK_F10, true); // PitchUp
    for (int i = 3000; i < 3500; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(std::abs(pitchDelta - 250.0f) < 0.01f, "500 PitchUp ticks should equal 250.0f");

    kbd.SetKeyDown(VK_F10, false);
    kbd.SetKeyDown(VK_F11, true); // PitchDown
    for (int i = 3500; i < 4000; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(std::abs(pitchDelta) < 0.001f, "500 PitchDown ticks must perfectly cancel PitchUp (net 0.0f)");

    return true;
}

// 7. Long Horizon Horizon Simulation: steady_clock spanning 24+ hours
bool Test_Adv_ContinuousHolding_LongHorizon_NoOverflow() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int toggleCount = 0;
    mgr.SetOnTogglePitch([&]() { toggleCount++; });

    auto t0 = std::chrono::steady_clock::now();

    // Trigger at t = 0
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0);
    ADV_ASSERT(toggleCount == 1, "t=0 fires");

    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t0 + std::chrono::milliseconds(50));

    // Fast-forward 24 hours: 86,400,000 ms
    auto t24h = t0 + std::chrono::hours(24);
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t24h);
    ADV_ASSERT(toggleCount == 2, "Press after 24h of simulated gameplay fires smoothly");

    // Fast-forward another 48 hours
    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t24h + std::chrono::milliseconds(100));

    auto t72h = t0 + std::chrono::hours(72);
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t72h);
    ADV_ASSERT(toggleCount == 3, "Press after 72h fires without duration overflow");

    return true;
}

// ===========================================================================
// GROUP 3: Simultaneous Conflicting Keypresses
// ===========================================================================

// 8. Simultaneous Opposing Continuous Keys: Net delta cancellation
bool Test_Adv_SimultaneousOpposingContinuousKeys() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    float altitudeDelta = 0.0f;
    float zoomDelta = 0.0f;
    float pitchDelta = 0.0f;

    mgr.SetOnAdjustAltitude([&](float d) { altitudeDelta += d; });
    mgr.SetOnAdjustZoom([&](float d) { zoomDelta += d; });
    mgr.SetOnAdjustPitch([&](float d) { pitchDelta += d; });

    auto t0 = std::chrono::steady_clock::now();

    // Press BOTH AltitudeUp AND AltitudeDown simultaneously for 500 ticks
    kbd.SetKeyDown(VK_F7, true);
    kbd.SetKeyDown(VK_F8, true);

    // Press BOTH ZoomIn AND ZoomOut simultaneously
    kbd.SetKeyDown(VK_OEM_4, true);
    kbd.SetKeyDown(VK_OEM_6, true);

    // Press BOTH PitchUp AND PitchDown simultaneously
    kbd.SetKeyDown(VK_F10, true);
    kbd.SetKeyDown(VK_F11, true);

    for (int i = 0; i < 500; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }

    // Both opposing keys fire their callbacks in the same tick (+delta and -delta)
    // The net cumulative effect must be exactly 0.0f
    ADV_ASSERT(std::abs(altitudeDelta) < 0.001f, "Simultaneous Altitude Up and Down must cancel out to 0.0f");
    ADV_ASSERT(std::abs(zoomDelta) < 0.001f, "Simultaneous Zoom In and Out must cancel out to 0.0f");
    ADV_ASSERT(std::abs(pitchDelta) < 0.001f, "Simultaneous Pitch Up and Down must cancel out to 0.0f");

    return true;
}

// 9. Simultaneous Primary and Secondary Keys for the same action
bool Test_Adv_SimultaneousPrimaryAndSecondaryKeys() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int pitchToggles = 0;
    mgr.SetOnTogglePitch([&]() { pitchToggles++; });

    auto t0 = std::chrono::steady_clock::now();

    // 1. Press BOTH primary (VK_F1) AND secondary (VK_F5) on the EXACT same tick
    kbd.SetKeyDown(VK_F1, true);
    kbd.SetKeyDown(VK_F5, true);
    mgr.Update(t0);
    ADV_ASSERT(pitchToggles == 1, "Simultaneous primary + secondary press must fire exactly ONCE");

    // 2. Both held for next tick -> no fire
    mgr.Update(t0 + std::chrono::milliseconds(16));
    ADV_ASSERT(pitchToggles == 1, "Holding both must not re-fire");

    // 3. Release primary key (VK_F1), but KEEP secondary (VK_F5) held down
    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t0 + std::chrono::milliseconds(32));
    ADV_ASSERT(pitchToggles == 1, "Releasing primary while secondary held must not fire or flutter");

    // 4. Re-press primary key (VK_F1) while secondary (VK_F5) is STILL held down
    kbd.SetKeyDown(VK_F1, true);
    mgr.Update(t0 + std::chrono::milliseconds(48));
    ADV_ASSERT(pitchToggles == 1, "Re-pressing primary while secondary is already held must not double fire");

    // 5. Release secondary key (VK_F5) while primary is STILL held down
    kbd.SetKeyDown(VK_F5, false);
    mgr.Update(t0 + std::chrono::milliseconds(64));
    ADV_ASSERT(pitchToggles == 1, "Releasing secondary while primary held must not fire");

    // 6. Release primary key -> now BOTH are UP
    kbd.SetKeyDown(VK_F1, false);
    mgr.Update(t0 + std::chrono::milliseconds(80));
    ADV_ASSERT(pitchToggles == 1, "Releasing both leaves count at 1");

    // 7. Repress either key after 200ms debounce lockout (t = 250 ms) -> must fire!
    kbd.SetKeyDown(VK_F5, true);
    mgr.Update(t0 + std::chrono::milliseconds(250));
    ADV_ASSERT(pitchToggles == 2, "Press after 200ms lockout fires second toggle");

    return true;
}

// 10. All Actions Active Simultaneously: Massive Contention
bool Test_Adv_AllActionsSimultaneous_MassiveContention() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    std::atomic<int> callbacksDispatched{0};
    mgr.SetOnTogglePitch([&]() { callbacksDispatched++; });
    mgr.SetOnToggleZoom([&]() { callbacksDispatched++; });
    mgr.SetOnAdjustAltitude([&](float) { callbacksDispatched++; });
    mgr.SetOnAdjustZoom([&](float) { callbacksDispatched++; });
    mgr.SetOnAdjustPitch([&](float) { callbacksDispatched++; });
    mgr.SetOnResetDefaults([&]() { callbacksDispatched++; });
    mgr.SetOnExit([&]() { callbacksDispatched++; });

    auto t0 = std::chrono::steady_clock::now();

    // Turn ON every virtual key mapped to every action
    kbd.SetKeyDown(VK_F1, true);
    kbd.SetKeyDown(VK_F5, true);
    kbd.SetKeyDown(VK_F2, true);
    kbd.SetKeyDown(VK_F6, true);
    kbd.SetKeyDown(VK_F7, true);
    kbd.SetKeyDown(VK_F8, true);
    kbd.SetKeyDown(VK_OEM_4, true);
    kbd.SetKeyDown(VK_OEM_6, true);
    kbd.SetKeyDown(VK_F10, true);
    kbd.SetKeyDown(VK_F11, true);
    kbd.SetKeyDown(VK_F9, true);
    kbd.SetKeyDown(VK_F12, true);

    // Also turn on speed modifiers
    kbd.SetKeyDown(VK_SHIFT, true);
    kbd.SetKeyDown(VK_CONTROL, true);

    // Execute update: must not crash, deadlock, or throw
    mgr.Update(t0);

    // Callbacks dispatched:
    // 4 toggles: TogglePitch (1), ToggleZoom (1), ResetDefaults (1), Exit (1) = 4
    // 6 adjustments: AltUp (1), AltDown (1), ZoomIn (1), ZoomOut (1), PitchUp (1), PitchDown (1) = 6
    // Total callbacks = 10
    ADV_ASSERT(callbacksDispatched == 10, "All 10 distinct hotkey actions must dispatch correctly under total contention");

    return true;
}

// ===========================================================================
// GROUP 4: Speed Modifier Extremes & Dynamic Flapping
// ===========================================================================

// 11. Speed Modifiers: Shift (3.0x), Ctrl (0.1x), Both (0.3x)
bool Test_Adv_SpeedModifiers_Shift_Control_Dual_Extremes() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    float lastDelta = 0.0f;
    mgr.SetOnAdjustAltitude([&](float d) { lastDelta = d; });

    auto t0 = std::chrono::steady_clock::now();
    kbd.SetKeyDown(VK_F7, true);

    // 1. Normal (1.0x): 0.25f
    mgr.Update(t0);
    ADV_ASSERT(std::abs(lastDelta - 0.25f) < 0.001f, "Baseline delta should be 0.25f");

    // 2. Shift (3.0x): 0.75f
    kbd.SetKeyDown(VK_SHIFT, true);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    ADV_ASSERT(std::abs(lastDelta - 0.75f) < 0.001f, "Shift boost delta should be 0.75f");

    // 3. Ctrl only (0.1x): 0.025f
    kbd.SetKeyDown(VK_SHIFT, false);
    kbd.SetKeyDown(VK_CONTROL, true);
    mgr.Update(t0 + std::chrono::milliseconds(32));
    ADV_ASSERT(std::abs(lastDelta - 0.025f) < 0.0001f, "Ctrl micro precision delta should be 0.025f");

    // 4. Both Shift AND Ctrl (0.3x): 0.075f
    kbd.SetKeyDown(VK_SHIFT, true);
    kbd.SetKeyDown(VK_CONTROL, true);
    mgr.Update(t0 + std::chrono::milliseconds(48));
    ADV_ASSERT(std::abs(lastDelta - 0.075f) < 0.0001f, "Dual Shift+Ctrl delta should be 0.075f");

    return true;
}

// 12. High Frequency Modifier Flapping during continuous holding
bool Test_Adv_SpeedModifiers_HighFrequencyFlapping() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    float deltaRecorded = 0.0f;
    mgr.SetOnAdjustAltitude([&](float d) { deltaRecorded = d; });

    auto t0 = std::chrono::steady_clock::now();
    kbd.SetKeyDown(VK_F7, true);

    // Over 1,000 ticks, flap modifiers rapidly in 4-step cycles:
    // tick 0: Normal -> 0.25f
    // tick 1: Shift  -> 0.75f
    // tick 2: Ctrl   -> 0.025f
    // tick 3: Both   -> 0.075f
    for (int i = 0; i < 1000; ++i) {
        const int mode = i % 4;
        if (mode == 0) {
            kbd.SetKeyDown(VK_SHIFT, false);
            kbd.SetKeyDown(VK_CONTROL, false);
        } else if (mode == 1) {
            kbd.SetKeyDown(VK_SHIFT, true);
            kbd.SetKeyDown(VK_CONTROL, false);
        } else if (mode == 2) {
            kbd.SetKeyDown(VK_SHIFT, false);
            kbd.SetKeyDown(VK_CONTROL, true);
        } else {
            kbd.SetKeyDown(VK_SHIFT, true);
            kbd.SetKeyDown(VK_CONTROL, true);
        }

        mgr.Update(t0 + std::chrono::milliseconds(i * 16));

        float expected = 0.25f;
        if (mode == 1) expected = 0.75f;
        else if (mode == 2) expected = 0.025f;
        else if (mode == 3) expected = 0.075f;

        ADV_ASSERT(std::abs(deltaRecorded - expected) < 0.0001f, "Flapping modifier tick delta mismatch");
    }

    return true;
}

// 13. Step Size Extremes & Dynamic Resizing
bool Test_Adv_StepSizeExtremes() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    float deltaRecorded = 0.0f;
    mgr.SetOnAdjustAltitude([&](float d) { deltaRecorded = d; });

    auto t0 = std::chrono::steady_clock::now();
    kbd.SetKeyDown(VK_F7, true);

    // 1. Zero Step Size: delta must be exactly 0.0f
    mgr.SetAltitudeStep(0.0f);
    ADV_ASSERT(std::abs(mgr.GetAltitudeStep() - 0.0f) < 0.0001f, "GetAltitudeStep matches 0.0f");
    mgr.Update(t0);
    ADV_ASSERT(std::abs(deltaRecorded) < 0.0001f, "Zero step size produces zero delta");

    // 2. Huge Step Size: 1,000,000.0f
    mgr.SetAltitudeStep(1000000.0f);
    kbd.SetKeyDown(VK_SHIFT, true); // 3.0x
    mgr.Update(t0 + std::chrono::milliseconds(16));
    ADV_ASSERT(std::abs(deltaRecorded - 3000000.0f) < 1.0f, "Huge step size with shift boost = 3,000,000");

    // 3. Micro Step Size: 1e-6f with Ctrl (0.1x)
    mgr.SetAltitudeStep(1e-6f);
    kbd.SetKeyDown(VK_SHIFT, false);
    kbd.SetKeyDown(VK_CONTROL, true);
    mgr.Update(t0 + std::chrono::milliseconds(32));
    ADV_ASSERT(std::abs(deltaRecorded - 1e-7f) < 1e-9f, "Micro step size with ctrl precision = 1e-7");

    // 4. Inverted / Negative Step Size: -0.5f
    mgr.SetAltitudeStep(-0.5f);
    kbd.SetKeyDown(VK_CONTROL, false);
    mgr.Update(t0 + std::chrono::milliseconds(48));
    ADV_ASSERT(std::abs(deltaRecorded - (-0.5f)) < 0.0001f, "Negative step size produces inverted delta");

    return true;
}

// ===========================================================================
// GROUP 5: Thread Concurrency & Contention Stress
// ===========================================================================

// 14. Multi-threaded Concurrency: Background thread polling while 4 threads hammer configuration
bool Test_Adv_ConcurrentDynamicReconfiguration() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    std::atomic<bool> stopThreads{false};
    std::atomic<int> callbacksDispatched{0};

    mgr.SetOnTogglePitch([&]() { callbacksDispatched++; });
    mgr.SetOnAdjustAltitude([&](float) { callbacksDispatched++; });

    // Start background 60 Hz polling thread
    mgr.Start();
    ADV_ASSERT(mgr.IsRunning(), "Manager background thread must be running");

    const int iterations = 10000;

    // Worker 1: Rapid keypress mutations
    std::thread tKeyMutator([&]() {
        for (int i = 0; i < iterations && !stopThreads.load(); ++i) {
            kbd.SetKeyDown(VK_F1, (i % 2 == 0));
            kbd.SetKeyDown(VK_F7, (i % 3 == 0));
            kbd.SetKeyDown(VK_SHIFT, (i % 5 == 0));
            kbd.SetKeyDown(VK_CONTROL, (i % 7 == 0));
            std::this_thread::yield();
        }
    });

    // Worker 2: Rapid step size reconfiguration
    std::thread tStepReconfig([&]() {
        for (int i = 0; i < iterations && !stopThreads.load(); ++i) {
            mgr.SetAltitudeStep(0.1f * (i % 10));
            mgr.SetZoomStep(0.2f * (i % 5));
            mgr.SetPitchStep(0.05f * (i % 20));
            float s = mgr.GetAltitudeStep();
            (void)s;
            std::this_thread::yield();
        }
    });

    // Worker 3: Rapid key binding mutations
    std::thread tBindingMutator([&]() {
        for (int i = 0; i < iterations && !stopThreads.load(); ++i) {
            if (i % 2 == 0) {
                mgr.SetBinding(HotkeyAction::TogglePitch, VK_F1, VK_F5);
            } else {
                mgr.SetBinding(HotkeyAction::TogglePitch, VK_F7, 0);
            }
            KeyBinding b = mgr.GetBinding(HotkeyAction::TogglePitch);
            (void)b;
            std::this_thread::yield();
        }
    });

    // Worker 4: Rapid callback updates
    std::thread tCallbackMutator([&]() {
        for (int i = 0; i < iterations && !stopThreads.load(); ++i) {
            mgr.SetOnTogglePitch([&]() { callbacksDispatched++; });
            mgr.SetOnAdjustAltitude([&](float) { callbacksDispatched++; });
            std::this_thread::yield();
        }
    });

    tKeyMutator.join();
    tStepReconfig.join();
    tBindingMutator.join();
    tCallbackMutator.join();

    stopThreads.store(true);
    mgr.Stop();
    ADV_ASSERT(!mgr.IsRunning(), "Manager background thread stopped cleanly");

    return true;
}

// 15. Callback Re-entrancy Safety: Callbacks querying/modifying HotkeyManager
bool Test_Adv_CallbackReentrancySafety() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    bool reentrancySuccess = false;

    // Inside callback, call HotkeyManager public methods that acquire m_mutex
    mgr.SetOnTogglePitch([&]() {
        // Query step
        float currentStep = mgr.GetAltitudeStep();
        // Modify step
        mgr.SetAltitudeStep(currentStep + 1.0f);
        // Query binding
        KeyBinding b = mgr.GetBinding(HotkeyAction::TogglePitch);
        (void)b;
        reentrancySuccess = true;
    });

    auto t0 = std::chrono::steady_clock::now();
    kbd.SetKeyDown(VK_F1, true);

    // Call Update(): must NOT deadlock!
    mgr.Update(t0);

    ADV_ASSERT(reentrancySuccess, "Callback re-entrancy into HotkeyManager succeeded without mutex deadlock");
    ADV_ASSERT(std::abs(mgr.GetAltitudeStep() - (DEFAULT_ALTITUDE_STEP + 1.0f)) < 0.001f, "Step updated from within callback");

    return true;
}

// 16. Concurrently call SetKeyStateProvider while multiple worker threads invoke IsKeyDown (100,000 iterations each)
bool Test_Adv_ConcurrentSetKeyStateProviderAndIsKeyDown_100k() {
    HotkeyManager mgr;
    const int iterationsPerWorker = 100000;
    const int numWorkerThreads = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> workersDone{false};
    std::atomic<int> completedWorkers{0};
    std::atomic<uint64_t> totalQueries{0};

    std::vector<std::thread> workers;
    workers.reserve(numWorkerThreads);
    for (int t = 0; t < numWorkerThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            uint64_t count = 0;
            const int testKeys[] = { VK_SHIFT, VK_CONTROL, VK_F1, VK_F2, VK_F7, VK_F8, 0, 0x41 };
            const size_t numKeys = sizeof(testKeys) / sizeof(testKeys[0]);

            for (int i = 0; i < iterationsPerWorker; ++i) {
                int key = testKeys[(i + t) % numKeys];
                bool down = mgr.IsKeyDown(key);
                (void)down;
                count++;
            }
            totalQueries.fetch_add(count, std::memory_order_relaxed);
            if (completedWorkers.fetch_add(1, std::memory_order_acq_rel) + 1 == numWorkerThreads) {
                workersDone.store(true, std::memory_order_release);
            }
        });
    }

    std::thread mutator([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        uint64_t swapCount = 0;
        while (!workersDone.load(std::memory_order_acquire)) {
            if (swapCount % 3 == 0) {
                auto state = std::make_shared<std::string>("state_" + std::to_string(swapCount));
                mgr.SetKeyStateProvider([state, swapCount](int k) -> bool {
                    return (k + static_cast<int>(swapCount) + static_cast<int>(state->length())) % 2 == 0;
                });
            } else if (swapCount % 3 == 1) {
                mgr.SetKeyStateProvider([swapCount](int k) -> bool {
                    return (k == VK_SHIFT) && (swapCount % 2 == 0);
                });
            } else {
                mgr.ResetKeyStateProvider();
            }
            swapCount++;
        }
    });

    startFlag.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    mutator.join();

    ADV_ASSERT(totalQueries.load() == static_cast<uint64_t>(iterationsPerWorker * numWorkerThreads), 
               "All 400,000 queries completed across provider mutations");
    return true;
}

// 17. Concurrently call SetBinding while polling loop / worker threads read IsActionKeyDown (100,000 iterations each)
bool Test_Adv_ConcurrentSetBindingAndIsActionKeyDown_100k() {
    HotkeyManager mgr;
    mgr.SetKeyStateProvider([](int k) -> bool {
        return (k == VK_F1 || k == VK_F5 || k == VK_F7 || k == 0x41);
    });

    const int iterationsPerReader = 100000;
    const int numReaderThreads = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> readersDone{false};
    std::atomic<int> completedReaders{0};
    std::atomic<uint64_t> totalActionQueries{0};

    std::vector<std::thread> readers;
    readers.reserve(numReaderThreads);
    for (int t = 0; t < numReaderThreads; ++t) {
        readers.emplace_back([&, t]() {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            uint64_t count = 0;
            const size_t actionCount = static_cast<size_t>(HotkeyAction::ActionCount);

            for (int i = 0; i < iterationsPerReader; ++i) {
                HotkeyAction action = static_cast<HotkeyAction>((i + t) % actionCount);
                bool down = mgr.IsActionKeyDown(action);
                (void)down;
                count++;
            }
            totalActionQueries.fetch_add(count, std::memory_order_relaxed);
            if (completedReaders.fetch_add(1, std::memory_order_acq_rel) + 1 == numReaderThreads) {
                readersDone.store(true, std::memory_order_release);
            }
        });
    }

    std::thread mutator([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        const size_t actionCount = static_cast<size_t>(HotkeyAction::ActionCount);
        uint64_t mutCount = 0;
        while (!readersDone.load(std::memory_order_acquire)) {
            HotkeyAction action = static_cast<HotkeyAction>(mutCount % actionCount);
            if (mutCount % 2 == 0) {
                mgr.SetBinding(action, VK_F1, VK_F5);
            } else {
                mgr.SetBinding(action, 0x41 + (mutCount % 26), (mutCount % 4 == 0) ? VK_F7 : 0);
            }
            mutCount++;
        }
    });

    startFlag.store(true, std::memory_order_release);
    for (auto& r : readers) r.join();
    mutator.join();

    ADV_ASSERT(totalActionQueries.load() == static_cast<uint64_t>(iterationsPerReader * numReaderThreads),
               "All 400,000 action queries completed across binding mutations");
    return true;
}

// 18. Concurrently call SetKeyStateProvider while multiple worker threads continuously invoke Update() & IsKeyDown() (100,000 iterations each)
bool Test_Adv_ConcurrentProviderSwapsWithUpdateAndQueries_100k() {
    HotkeyManager mgr;
    const int iterationsPerWorker = 100000;
    const int numWorkerThreads = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> workersDone{false};
    std::atomic<int> completedWorkers{0};
    std::atomic<uint64_t> totalWorkUnits{0};

    std::vector<std::thread> workers;
    workers.reserve(numWorkerThreads);
    for (int t = 0; t < numWorkerThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            uint64_t count = 0;
            for (int i = 0; i < iterationsPerWorker; ++i) {
                if (i % 3 == 0) {
                    mgr.Update();
                } else if (i % 3 == 1) {
                    bool k = mgr.IsKeyDown(VK_F1);
                    (void)k;
                } else {
                    float mult = mgr.CalculateSpeedMultiplier();
                    (void)mult;
                }
                count++;
            }
            totalWorkUnits.fetch_add(count, std::memory_order_relaxed);
            if (completedWorkers.fetch_add(1, std::memory_order_acq_rel) + 1 == numWorkerThreads) {
                workersDone.store(true, std::memory_order_release);
            }
        });
    }

    std::thread mutator([&]() {
        while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
        uint64_t count = 0;
        while (!workersDone.load(std::memory_order_acquire)) {
            if (count % 2 == 0) {
                mgr.SetKeyStateProvider([count](int k) -> bool {
                    return (k == VK_F1 || k == VK_SHIFT) && (count % 4 != 0);
                });
            } else {
                mgr.ResetKeyStateProvider();
            }
            count++;
        }
    });

    startFlag.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    mutator.join();

    ADV_ASSERT(totalWorkUnits.load() == static_cast<uint64_t>(iterationsPerWorker * numWorkerThreads),
               "All 400,000 worker operations completed without race or crash");
    return true;
}

// ===========================================================================
// GROUP 6: Full State Transition Completeness & Zero Flutter Verification
// ===========================================================================

// 16. Comprehensive FSM State Transition Testing: UP -> DOWN -> HELD -> RELEASED -> UP
bool Test_Adv_FullStateTransitionMatrix_FSM() {
    HotkeyManager mgr;
    ThreadSafeMockKeyboard kbd;
    mgr.SetKeyStateProvider([&](int k) { return kbd.IsDown(k); });

    int toggleCount = 0;
    int continuousCount = 0;
    int stateChangeCount = 0;

    mgr.SetOnTogglePitch([&]() { toggleCount++; });
    mgr.SetOnAdjustAltitude([&](float) { continuousCount++; });
    mgr.SetOnStateChanged([&]() { stateChangeCount++; });

    auto t0 = std::chrono::steady_clock::now();

    // State 1: UP (Neither key pressed)
    // Expectation: 0 toggle, 0 continuous, 0 stateChanges
    mgr.Update(t0);
    ADV_ASSERT(toggleCount == 0 && continuousCount == 0 && stateChangeCount == 0, "State UP must produce 0 events");

    // State 2: Transition UP -> DOWN (Rising Edge)
    kbd.SetKeyDown(VK_F1, true);
    kbd.SetKeyDown(VK_F7, true);
    mgr.Update(t0 + std::chrono::milliseconds(16));
    ADV_ASSERT(toggleCount == 1, "State UP->DOWN: Toggle must fire exactly 1");
    ADV_ASSERT(continuousCount == 1, "State UP->DOWN: Continuous must fire 1");
    ADV_ASSERT(stateChangeCount == 1, "State UP->DOWN: StateChanged must fire 1");

    // State 3: Transition DOWN -> HELD (Level)
    // Over next 10 ticks: Toggle MUST NOT fire again. Continuous MUST fire 10 times.
    for (int i = 2; i <= 11; ++i) {
        mgr.Update(t0 + std::chrono::milliseconds(i * 16));
    }
    ADV_ASSERT(toggleCount == 1, "State HELD: Toggle must remain strictly 1 (zero flutter)");
    ADV_ASSERT(continuousCount == 11, "State HELD: Continuous fired 1 + 10 = 11");
    ADV_ASSERT(stateChangeCount == 11, "State HELD: StateChanged fired 1 + 10 = 11");

    // State 4: Transition HELD -> RELEASED (Falling Edge)
    kbd.SetKeyDown(VK_F1, false);
    kbd.SetKeyDown(VK_F7, false);
    mgr.Update(t0 + std::chrono::milliseconds(12 * 16));
    ADV_ASSERT(toggleCount == 1, "State RELEASED: Toggle must not fire");
    ADV_ASSERT(continuousCount == 11, "State RELEASED: Continuous must not fire");
    ADV_ASSERT(stateChangeCount == 11, "State RELEASED: StateChanged must not fire");

    // State 5: UP again
    mgr.Update(t0 + std::chrono::milliseconds(13 * 16));
    ADV_ASSERT(toggleCount == 1 && continuousCount == 11 && stateChangeCount == 11, "State UP after release is quiescent");

    return true;
}

// ===========================================================================
// Main Adversarial Test Runner
// ===========================================================================

int main() {
    std::cout << "============================================================\n";
    std::cout << " Cold Steel III - Adversarial Hotkey & Concurrency Suite    \n";
    std::cout << "============================================================\n";

    // Group 1: Debouncing & Rapid Keypresses
    RUN_ADV_TEST(Test_Adv_RapidKeypresses_SubDebounceWindow_SingleDispatch);
    RUN_ADV_TEST(Test_Adv_Debounce_ExactLockoutBoundaries);
    RUN_ADV_TEST(Test_Adv_MultiAction_IndependentDebounce);
    RUN_ADV_TEST(Test_Adv_KeyHeld_Across_Debounce_Window_NoFlutter);

    // Group 2: Continuous Key Holding
    RUN_ADV_TEST(Test_Adv_ContinuousHolding_10000Ticks_ExactLinearity);
    RUN_ADV_TEST(Test_Adv_ContinuousHolding_BidirectionalCancellation);
    RUN_ADV_TEST(Test_Adv_ContinuousHolding_LongHorizon_NoOverflow);

    // Group 3: Simultaneous Conflicting Keypresses
    RUN_ADV_TEST(Test_Adv_SimultaneousOpposingContinuousKeys);
    RUN_ADV_TEST(Test_Adv_SimultaneousPrimaryAndSecondaryKeys);
    RUN_ADV_TEST(Test_Adv_AllActionsSimultaneous_MassiveContention);

    // Group 4: Speed Modifier Extremes
    RUN_ADV_TEST(Test_Adv_SpeedModifiers_Shift_Control_Dual_Extremes);
    RUN_ADV_TEST(Test_Adv_SpeedModifiers_HighFrequencyFlapping);
    RUN_ADV_TEST(Test_Adv_StepSizeExtremes);

    // Group 5: Thread Concurrency & Contention Stress
    RUN_ADV_TEST(Test_Adv_ConcurrentDynamicReconfiguration);
    RUN_ADV_TEST(Test_Adv_CallbackReentrancySafety);
    RUN_ADV_TEST(Test_Adv_ConcurrentSetKeyStateProviderAndIsKeyDown_100k);
    RUN_ADV_TEST(Test_Adv_ConcurrentSetBindingAndIsActionKeyDown_100k);
    RUN_ADV_TEST(Test_Adv_ConcurrentProviderSwapsWithUpdateAndQueries_100k);

    // Group 6: Full State Transition Completeness
    RUN_ADV_TEST(Test_Adv_FullStateTransitionMatrix_FSM);

    std::cout << "============================================================\n";
    std::cout << " Results: Passed: " << g_passedTests << " | Failed: " << g_failedTests << "\n";
    std::cout << "============================================================\n";

    return (g_failedTests == 0) ? 0 : 1;
}
