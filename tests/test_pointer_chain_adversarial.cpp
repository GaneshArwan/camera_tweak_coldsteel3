#include "../src/camera_patcher.hpp"
#include "../src/common.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <limits>
#include <cmath>
#include <cstring>
#include <cassert>
#include <windows.h>

using namespace ColdSteel3;

static int g_passCount = 0;
static int g_failCount = 0;

#define TEST_CHECK(cond, desc) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] Line " << __LINE__ << ": " << desc << std::endl; \
            g_failCount++; \
            return false; \
        } \
    } while (0)

#define RUN_SUBTEST(fn) \
    do { \
        std::cout << "[RUN ] " << #fn << "..." << std::endl; \
        if (fn()) { \
            std::cout << "[PASS] " << #fn << std::endl; \
            g_passCount++; \
        } else { \
            std::cout << "[FAIL] " << #fn << std::endl; \
        } \
    } while (0)

// ===========================================================================
// Memory Environment Helper
// Sets up a full 3-tier pointer chain in local process memory:
// gmGlobalPtr -> gmRoot (bufA)
// bufA + 0x1CE8 -> gameManager (bufB)
// bufB + 0x39F48 -> activeCam (bufC)
// bufC + 0x368 -> Eye Y float
// ===========================================================================
struct PointerChainEnvironment {
    uint8_t* bufA = nullptr; // gmRoot
    uint8_t* bufB = nullptr; // gameManager
    uint8_t* bufC = nullptr; // activeCam
    uintptr_t* pGlobalVar = nullptr;

    const size_t sizeA = 0x3000;
    const size_t sizeB = 0x45000;
    const size_t sizeC = 0x1000;

    const float defaultY = 25.0f;

    PointerChainEnvironment() {
        bufA = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        bufB = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        bufC = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeC, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        pGlobalVar = new uintptr_t(0);

        assert(bufA && bufB && bufC && pGlobalVar);
        ResetToValid();
    }

    ~PointerChainEnvironment() {
        if (bufA) VirtualFree(bufA, 0, MEM_RELEASE);
        if (bufB) VirtualFree(bufB, 0, MEM_RELEASE);
        if (bufC) VirtualFree(bufC, 0, MEM_RELEASE);
        delete pGlobalVar;
    }

    void ResetToValid() {
        std::memset(bufA, 0, sizeA);
        std::memset(bufB, 0, sizeB);
        std::memset(bufC, 0, sizeC);

        // ActiveCam Eye Y
        *reinterpret_cast<float*>(bufC + OFFSET_CAMERA_EYE_Y) = defaultY;

        // bufB + 0x39F48 -> activeCam
        uintptr_t camPtr = reinterpret_cast<uintptr_t>(bufC);
        std::memcpy(bufB + OFFSET_GM_TO_ACTIVE_CAMERA, &camPtr, sizeof(camPtr));

        // bufA + 0x1CE8 -> gameManager
        uintptr_t gmPtr = reinterpret_cast<uintptr_t>(bufB);
        std::memcpy(bufA + OFFSET_GM_POINTER_TO_INSTANCE, &gmPtr, sizeof(gmPtr));

        // pGlobalVar -> gmRoot
        *pGlobalVar = reinterpret_cast<uintptr_t>(bufA);
    }

    uintptr_t GetGlobalAddr() const {
        return reinterpret_cast<uintptr_t>(pGlobalVar);
    }

    float GetCurrentEyeY() const {
        return *reinterpret_cast<const float*>(bufC + OFFSET_CAMERA_EYE_Y);
    }
};

// ===========================================================================
// Test 1: Null Pointers at Every Level of the Chain
// ===========================================================================
bool Test_Null_Pointers_At_Every_Level() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");

    // Level 0: Global Pointer Address is 0
    patcher.SetCustomAddresses(0, 0, 0);
    TEST_CHECK(!patcher.AdjustAltitude(10.0f), "AdjustAltitude with gmGlobalAddr=0 must return false");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

    // Level 1: Global Pointer points to NULL (gmRoot == 0)
    *env.pGlobalVar = 0;
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());
    TEST_CHECK(!patcher.AdjustAltitude(10.0f), "AdjustAltitude with gmRoot=0 must return false");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

    // Level 2: gmRoot is valid, but [gmRoot + 0x1CE8] (gameManager) is NULL
    env.ResetToValid();
    uintptr_t nullGm = 0;
    std::memcpy(env.bufA + OFFSET_GM_POINTER_TO_INSTANCE, &nullGm, sizeof(nullGm));
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());
    TEST_CHECK(!patcher.AdjustAltitude(10.0f), "AdjustAltitude with gameManager=0 must return false");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

    // Level 3: gameManager is valid, but [gameManager + 0x39F48] (activeCam) is NULL
    env.ResetToValid();
    uintptr_t nullCam = 0;
    std::memcpy(env.bufB + OFFSET_GM_TO_ACTIVE_CAMERA, &nullCam, sizeof(nullCam));
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());
    TEST_CHECK(!patcher.AdjustAltitude(10.0f), "AdjustAltitude with activeCam=0 must return false");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

    // Verify system can immediately recover when pointers become valid again
    env.ResetToValid();
    TEST_CHECK(patcher.AdjustAltitude(5.0f), "AdjustAltitude must succeed once chain is valid");
    TEST_CHECK(env.GetCurrentEyeY() == 30.0f, "Eye Y must be 25.0 + 5.0 = 30.0");
    TEST_CHECK(patcher.GetAltitudeDelta() == 5.0f, "Delta must be 5.0");

    patcher.Detach();
    return true;
}

// ===========================================================================
// Test 2: Bad / Corrupt Memory Addresses at Every Level
// ===========================================================================
bool Test_Corrupt_Memory_Addresses() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");

    const std::vector<uintptr_t> corruptAddresses = {
        0x1ULL,                     // Near null
        0x7FULL,
        0xDEADBEEFULL,              // 32-bit poison
        0xDEADBEEFCAFEBABEULL,      // 64-bit non-canonical user address
        0x00007FFFFFFFFFFFULL,      // High user space unmapped boundary
        0x8000000000000000ULL,      // Kernel space boundary
        0xFFFFFFFFFFFFFFFFULL,      // Maximum 64-bit integer
        0xFFFFFFFFFFFFF000ULL       // Integer overflow risk with offsets
    };

    for (uintptr_t badAddr : corruptAddresses) {
        // Corrupt Global Pointer
        patcher.SetCustomAddresses(0, 0, badAddr);
        TEST_CHECK(!patcher.AdjustAltitude(1.0f), "Corrupt global pointer must fail safely");
        TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

        // Corrupt gmRoot
        env.ResetToValid();
        *env.pGlobalVar = badAddr;
        patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());
        TEST_CHECK(!patcher.AdjustAltitude(1.0f), "Corrupt gmRoot must fail safely");
        TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

        // Corrupt gameManager
        env.ResetToValid();
        std::memcpy(env.bufA + OFFSET_GM_POINTER_TO_INSTANCE, &badAddr, sizeof(badAddr));
        patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());
        TEST_CHECK(!patcher.AdjustAltitude(1.0f), "Corrupt gameManager must fail safely");
        TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");

        // Corrupt activeCam
        env.ResetToValid();
        std::memcpy(env.bufB + OFFSET_GM_TO_ACTIVE_CAMERA, &badAddr, sizeof(badAddr));
        patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());
        TEST_CHECK(!patcher.AdjustAltitude(1.0f), "Corrupt activeCam must fail safely");
        TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0");
    }

    patcher.Detach();
    return true;
}

// ===========================================================================
// Test 3: Unaligned Pointer Indirections
// ===========================================================================
bool Test_Unaligned_Pointers() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");

    // Odd alignment pointers (e.g. bufA + 1)
    env.ResetToValid();
    uintptr_t unalignedGm = reinterpret_cast<uintptr_t>(env.bufB) + 1;
    std::memcpy(env.bufA + OFFSET_GM_POINTER_TO_INSTANCE, &unalignedGm, sizeof(unalignedGm));
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());

    // Reading from unaligned address in user memory must NEVER crash the process!
    bool result = patcher.AdjustAltitude(1.0f);
    if (!result) {
        TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0 on unaligned failure");
    }

    patcher.Detach();
    return true;
}

// ===========================================================================
// Test 4: Protected / Read-Only Page Safety
// ===========================================================================
bool Test_ReadOnly_And_NoAccess_Memory_Safety() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());

    // Mark bufC (containing Eye Y) as PAGE_READONLY
    DWORD oldProtect = 0;
    TEST_CHECK(VirtualProtect(env.bufC, env.sizeC, PAGE_READONLY, &oldProtect), "VirtualProtect to READONLY failed");

    // Read will succeed, but WriteProcessMemory will fail because page is read-only
    bool writeResult = patcher.AdjustAltitude(10.0f);
    TEST_CHECK(!writeResult, "AdjustAltitude into PAGE_READONLY must fail gracefully");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must NOT be incremented if write failed");

    // Mark bufC as PAGE_NOACCESS
    TEST_CHECK(VirtualProtect(env.bufC, env.sizeC, PAGE_NOACCESS, &oldProtect), "VirtualProtect to NOACCESS failed");
    bool noAccessResult = patcher.AdjustAltitude(10.0f);
    TEST_CHECK(!noAccessResult, "AdjustAltitude into PAGE_NOACCESS must fail gracefully");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0 on NOACCESS");

    // Restore PAGE_READWRITE and verify normal operation resumes
    TEST_CHECK(VirtualProtect(env.bufC, env.sizeC, PAGE_READWRITE, &oldProtect), "VirtualProtect back to READWRITE failed");
    TEST_CHECK(patcher.AdjustAltitude(10.0f), "AdjustAltitude must succeed after page restored to READWRITE");
    TEST_CHECK(env.GetCurrentEyeY() == 35.0f, "Eye Y must be 25.0 + 10.0 = 35.0");
    TEST_CHECK(patcher.GetAltitudeDelta() == 10.0f, "Delta must be 10.0");

    patcher.Detach();
    return true;
}

// ===========================================================================
// Test 5: Extreme Deltas & Precision Integrity
// ===========================================================================
bool Test_Extreme_Altitude_Deltas() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());

    // Large positive delta: +10,000.0f
    TEST_CHECK(patcher.AdjustAltitude(10000.0f), "AdjustAltitude(+10000) failed");
    TEST_CHECK(env.GetCurrentEyeY() == 10025.0f, "Eye Y must be 10025.0f");
    TEST_CHECK(patcher.GetAltitudeDelta() == 10000.0f, "Delta must be 10000.0f");

    // Large negative delta: -20,000.0f
    TEST_CHECK(patcher.AdjustAltitude(-20000.0f), "AdjustAltitude(-20000) failed");
    TEST_CHECK(env.GetCurrentEyeY() == -9975.0f, "Eye Y must be -9975.0f");
    TEST_CHECK(patcher.GetAltitudeDelta() == -10000.0f, "Cumulative delta must be -10000.0f");

    // Restore defaults
    TEST_CHECK(patcher.ResetDefaults(), "ResetDefaults failed");
    TEST_CHECK(env.GetCurrentEyeY() == 25.0f, "Eye Y must be reset to initial 25.0f");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Cumulative delta must be reset to 0.0f");

    // Very small sub-millimeter delta: 0.001f
    TEST_CHECK(patcher.AdjustAltitude(0.001f), "AdjustAltitude(0.001f) failed");
    TEST_CHECK(std::abs(env.GetCurrentEyeY() - 25.001f) < 1e-4f, "Eye Y precision check");
    TEST_CHECK(patcher.ResetDefaults(), "ResetDefaults failed");

    patcher.Detach();
    return true;
}

// ===========================================================================
// Test 6: Special IEEE 754 Float Handling (NaN, Infinity, Denormals)
// ===========================================================================
bool Test_Float_Special_Values_And_Guards() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());

    float initialY = env.GetCurrentEyeY(); // 25.0f

    // 1. Denormal / Subnormal float
    float denormal = 1.401298464e-45f;
    TEST_CHECK(patcher.AdjustAltitude(denormal), "Subnormal float adjustment should succeed without crash");
    TEST_CHECK(patcher.ResetDefaults(), "ResetDefaults after denormal should succeed");
    TEST_CHECK(env.GetCurrentEyeY() == initialY, "Altitude should match initial");

    // 2. IEEE 754 NaN handling
    float qnan = std::numeric_limits<float>::quiet_NaN();
    float snan = std::numeric_limits<float>::signaling_NaN();
    float inf = std::numeric_limits<float>::infinity();
    float negInf = -std::numeric_limits<float>::infinity();

    // Verify neither quiet nor signaling NaN cause hardware exceptions or crashes
    bool qnanOk = patcher.AdjustAltitude(qnan);
    TEST_CHECK(qnanOk, "AdjustAltitude(quiet_NaN) executes without SEH exception");
    
    // Reset state for signaling NaN test
    env.ResetToValid();
    patcher.ResetDefaults();

    bool snanOk = patcher.AdjustAltitude(snan);
    TEST_CHECK(snanOk, "AdjustAltitude(signaling_NaN) executes without SEH exception");

    // Reset state for Infinity tests
    env.ResetToValid();
    patcher.ResetDefaults();

    bool infOk = patcher.AdjustAltitude(inf);
    TEST_CHECK(infOk, "AdjustAltitude(+Infinity) executes without SEH exception");

    env.ResetToValid();
    patcher.ResetDefaults();

    bool negInfOk = patcher.AdjustAltitude(negInf);
    TEST_CHECK(negInfOk, "AdjustAltitude(-Infinity) executes without SEH exception");

    env.ResetToValid();
    patcher.ResetDefaults();
    patcher.Detach();
    return true;
}

// ===========================================================================
// Test 7: Broken Pointer Chain During ResetDefaults and Detach
// ===========================================================================
bool Test_Broken_Chain_During_ResetDefaults_And_Detach() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());

    // Step 1: Elevate altitude by +12.5m
    TEST_CHECK(patcher.AdjustAltitude(12.5f), "Initial altitude adjust failed");
    TEST_CHECK(patcher.GetAltitudeDelta() == 12.5f, "Delta must be 12.5");
    TEST_CHECK(env.GetCurrentEyeY() == 37.5f, "Eye Y must be 37.5");

    // Step 2: Break the pointer chain (simulate loading screen or level transition)
    // Game sets GameManager instance pointer to null
    uintptr_t zeroPtr = 0;
    std::memcpy(env.bufA + OFFSET_GM_POINTER_TO_INSTANCE, &zeroPtr, sizeof(zeroPtr));

    // Step 3: Call ResetDefaults while pointer chain is broken
    // Patcher must NOT crash or throw access violation. It should gracefully reset delta to 0.0f
    TEST_CHECK(patcher.ResetDefaults(), "ResetDefaults with broken chain must succeed gracefully");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must be reset to 0.0f");

    // Step 4: Break pointer chain again for Detach test
    env.ResetToValid();
    TEST_CHECK(patcher.AdjustAltitude(8.0f), "Second altitude adjust failed");
    TEST_CHECK(patcher.GetAltitudeDelta() == 8.0f, "Delta must be 8.0");

    // Break activeCam pointer
    std::memcpy(env.bufB + OFFSET_GM_TO_ACTIVE_CAMERA, &zeroPtr, sizeof(zeroPtr));

    // Step 5: Detach while pointer chain is broken
    // Destructor / Detach must NOT crash
    patcher.Detach();
    TEST_CHECK(!patcher.IsAttached(), "Patcher must be detached");
    TEST_CHECK(patcher.GetAltitudeDelta() == 0.0f, "Delta must be reset on detach");

    return true;
}

// ===========================================================================
// Test 8: Rapid Pointer Flapping / Race Condition Stress
// ===========================================================================
bool Test_Rapid_Pointer_Flapping_Stress() {
    PointerChainEnvironment env;
    CameraPatcher patcher;
    TEST_CHECK(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, env.GetGlobalAddr());

    const int ITERATIONS = 5000;
    int successCount = 0;
    int failCount = 0;

    for (int i = 0; i < ITERATIONS; ++i) {
        int state = i % 5;
        switch (state) {
            case 0:
                // Valid chain
                env.ResetToValid();
                break;
            case 1:
                // Null gmRoot
                *env.pGlobalVar = 0;
                break;
            case 2:
                // Null gameManager
                env.ResetToValid();
                {
                    uintptr_t zero = 0;
                    std::memcpy(env.bufA + OFFSET_GM_POINTER_TO_INSTANCE, &zero, sizeof(zero));
                }
                break;
            case 3:
                // Null activeCam
                env.ResetToValid();
                {
                    uintptr_t zero = 0;
                    std::memcpy(env.bufB + OFFSET_GM_TO_ACTIVE_CAMERA, &zero, sizeof(zero));
                }
                break;
            case 4:
                // Corrupt pointer
                *env.pGlobalVar = 0xDEADBEEF00000000ULL | (i * 0x1000);
                break;
        }

        if (patcher.AdjustAltitude(1.0f)) {
            successCount++;
        } else {
            failCount++;
        }
    }

    std::cout << "       [STRESS] " << ITERATIONS << " rapid pointer perturbations completed: "
              << successCount << " successes, " << failCount << " safe failures (0 crashes)" << std::endl;

    TEST_CHECK(successCount > 0, "Expected some successful updates");
    TEST_CHECK(failCount > 0, "Expected some safe failures");

    patcher.Detach();
    return true;
}

// ===========================================================================
// Main Runner
// ===========================================================================
int main() {
    std::cout << "======================================================================" << std::endl;
    std::cout << " Cold Steel III - Pointer Traversal & Altitude Adversarial Harness    " << std::endl;
    std::cout << "======================================================================" << std::endl;

    RUN_SUBTEST(Test_Null_Pointers_At_Every_Level);
    RUN_SUBTEST(Test_Corrupt_Memory_Addresses);
    RUN_SUBTEST(Test_Unaligned_Pointers);
    RUN_SUBTEST(Test_ReadOnly_And_NoAccess_Memory_Safety);
    RUN_SUBTEST(Test_Extreme_Altitude_Deltas);
    RUN_SUBTEST(Test_Float_Special_Values_And_Guards);
    RUN_SUBTEST(Test_Broken_Chain_During_ResetDefaults_And_Detach);
    RUN_SUBTEST(Test_Rapid_Pointer_Flapping_Stress);

    std::cout << "======================================================================" << std::endl;
    std::cout << " Results: Passed: " << g_passCount << " | Failed: " << g_failCount << std::endl;
    std::cout << "======================================================================" << std::endl;

    return (g_failCount == 0) ? 0 : 1;
}
