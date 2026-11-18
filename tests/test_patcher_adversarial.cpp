#include "../src/camera_patcher.hpp"
#include "../src/common.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstring>
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

struct MockBufferFixture {
    uint8_t* memory = nullptr;
    size_t size = 0x20000;
    uintptr_t pitchAddr = 0;
    uintptr_t zoomAddr = 0;

    MockBufferFixture() {
        memory = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        assert(memory != nullptr);
        pitchAddr = reinterpret_cast<uintptr_t>(memory + 0x1000);
        zoomAddr = reinterpret_cast<uintptr_t>(memory + 0x2000);
        Reset();
    }

    ~MockBufferFixture() {
        if (memory) {
            VirtualFree(memory, 0, MEM_RELEASE);
            memory = nullptr;
        }
    }

    void Reset() {
        std::memset(memory, 0xCC, size);
        std::memcpy(reinterpret_cast<void*>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH);
        std::memcpy(reinterpret_cast<void*>(zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH);
    }
};

// 1. Stress Test: Rapid Concurrent Multi-Threaded Toggling
bool Test_Adv_ConcurrentThreadSafety() {
    MockBufferFixture fix;
    CameraPatcher patcher;
    ADV_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(fix.memory)), "Attach failed");
    patcher.SetCustomAddresses(fix.pitchAddr, fix.zoomAddr);

    std::atomic<bool> startFlag{false};
    std::atomic<int> errorCount{0};

    auto worker1 = [&]() {
        while (!startFlag.load()) {}
        for (int i = 0; i < 2000; ++i) {
            if (!patcher.SetPitchUnlocked(i % 2 == 0)) {
                errorCount++;
            }
        }
    };

    auto worker2 = [&]() {
        while (!startFlag.load()) {}
        for (int i = 0; i < 2000; ++i) {
            if (!patcher.SetZoomUnlocked(i % 2 == 0)) {
                errorCount++;
            }
        }
    };

    auto worker3 = [&]() {
        while (!startFlag.load()) {}
        for (int i = 0; i < 2000; ++i) {
            patcher.IsPitchUnlocked();
            patcher.IsZoomUnlocked();
            patcher.GetAltitudeDelta();
        }
    };

    std::thread t1(worker1);
    std::thread t2(worker2);
    std::thread t3(worker3);

    startFlag.store(true);
    t1.join();
    t2.join();
    t3.join();

    ADV_ASSERT(errorCount.load() == 0, "No errors should occur during concurrent toggling");

    // Clean restore
    patcher.Detach();
    ADV_ASSERT(std::memcmp(reinterpret_cast<void*>(fix.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0, "Pitch restored");
    ADV_ASSERT(std::memcmp(reinterpret_cast<void*>(fix.zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0, "Zoom restored");

    return true;
}

// 2. Stress Test: Invalid/Unmapped Memory Addresses Safety
bool Test_Adv_InvalidMemoryAddresses() {
    CameraPatcher patcher;
    ADV_ASSERT(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");

    // Test with unmapped or guarded address
    uintptr_t invalidAddr = 0x1000; // Unmapped null-page adjacent
    patcher.SetCustomAddresses(invalidAddr, invalidAddr);

    // Write should fail gracefully without crashing or causing AV
    bool pitchRes = patcher.SetPitchUnlocked(true);
    ADV_ASSERT(!pitchRes, "SetPitchUnlocked on invalid address must return false");
    ADV_ASSERT(!patcher.IsPitchUnlocked(), "Must not mark as applied on failure");

    bool zoomRes = patcher.SetZoomUnlocked(true);
    ADV_ASSERT(!zoomRes, "SetZoomUnlocked on invalid address must return false");
    ADV_ASSERT(!patcher.IsZoomUnlocked(), "Must not mark as applied on failure");

    patcher.Detach();
    return true;
}

// 3. Stress Test: Double Attach and Re-Attach
bool Test_Adv_ReAttachLifecycle() {
    MockBufferFixture fix1;
    MockBufferFixture fix2;
    CameraPatcher patcher;

    // Attach 1
    ADV_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(fix1.memory)), "Attach 1 failed");
    patcher.SetCustomAddresses(fix1.pitchAddr, fix1.zoomAddr);
    ADV_ASSERT(patcher.SetPitchUnlocked(true), "Pitch unlock 1 failed");
    ADV_ASSERT(patcher.SetZoomUnlocked(true), "Zoom unlock 1 failed");

    // Re-attach to fix2 without manual detach -> should automatically detach & restore fix1
    ADV_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(fix2.memory)), "Re-attach failed");
    ADV_ASSERT(std::memcmp(reinterpret_cast<void*>(fix1.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0,
               "Fix1 pitch must be restored upon re-attach");
    ADV_ASSERT(std::memcmp(reinterpret_cast<void*>(fix1.zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0,
               "Fix1 zoom must be restored upon re-attach");

    patcher.SetCustomAddresses(fix2.pitchAddr, fix2.zoomAddr);
    ADV_ASSERT(!patcher.IsPitchUnlocked(), "State should be clean after re-attach");
    ADV_ASSERT(!patcher.IsZoomUnlocked(), "State should be clean after re-attach");

    // Apply on fix2
    ADV_ASSERT(patcher.SetPitchUnlocked(true), "Pitch unlock 2 failed");
    patcher.Detach();
    ADV_ASSERT(std::memcmp(reinterpret_cast<void*>(fix2.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0,
               "Fix2 pitch must be restored on detach");

    return true;
}

// 4. Stress Test: Altitude Numerical Stability
bool Test_Adv_AltitudeNumericalStability() {
    CameraPatcher patcher;

    std::vector<uint8_t> bufA(0x2000, 0);
    std::vector<uint8_t> bufGM(0x40000, 0);
    std::vector<uint8_t> bufCam(0x1000, 0);

    float initialEyeY = 100.0f;
    *reinterpret_cast<float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y) = initialEyeY;

    uintptr_t camPtr = reinterpret_cast<uintptr_t>(bufCam.data());
    std::memcpy(bufGM.data() + OFFSET_GM_TO_ACTIVE_CAMERA, &camPtr, sizeof(camPtr));

    uintptr_t gmPtr = reinterpret_cast<uintptr_t>(bufGM.data());
    std::memcpy(bufA.data() + OFFSET_GM_POINTER_TO_INSTANCE, &gmPtr, sizeof(gmPtr));

    uintptr_t rootPtr = reinterpret_cast<uintptr_t>(bufA.data());
    uintptr_t* pGlobalVar = new uintptr_t(rootPtr);

    ADV_ASSERT(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, reinterpret_cast<uintptr_t>(pGlobalVar));

    // Test extreme deltas
    ADV_ASSERT(patcher.AdjustAltitude(10000.0f), "High altitude delta failed");
    ADV_ASSERT(patcher.AdjustAltitude(-20000.0f), "Low altitude delta failed");
    ADV_ASSERT(patcher.AdjustAltitude(10000.0f), "Return altitude delta failed");

    float currentY = *reinterpret_cast<const float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y);
    ADV_ASSERT(std::fabs(currentY - initialEyeY) < 0.01f, "Net altitude should return to initial");
    ADV_ASSERT(std::fabs(patcher.GetAltitudeDelta()) < 0.01f, "Net delta tracker should be ~0.0");

    ADV_ASSERT(patcher.ResetDefaults(), "ResetDefaults failed");
    currentY = *reinterpret_cast<const float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y);
    ADV_ASSERT(currentY == initialEyeY, "Altitude should be initialEyeY");

    delete pGlobalVar;
    return true;
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << " Cold Steel III - CameraPatcher Adversarial & Stress Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    RUN_ADV_TEST(Test_Adv_ConcurrentThreadSafety);
    RUN_ADV_TEST(Test_Adv_InvalidMemoryAddresses);
    RUN_ADV_TEST(Test_Adv_ReAttachLifecycle);
    RUN_ADV_TEST(Test_Adv_AltitudeNumericalStability);

    std::cout << "============================================================" << std::endl;
    std::cout << " Adversarial Results: Passed: " << g_passedTests << " | Failed: " << g_failedTests << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
