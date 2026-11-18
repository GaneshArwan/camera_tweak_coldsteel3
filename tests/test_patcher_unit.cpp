#include "../src/camera_patcher.hpp"
#include "../src/common.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>

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

// ===========================================================================
// Test Memory Buffer Fixture
// Allocates page-aligned memory simulating target process address space.
// ===========================================================================
struct MockProcessContext {
    uint8_t* memory = nullptr;
    size_t size = 0x20000; // 128 KB
    uintptr_t pitchAddr = 0;
    uintptr_t zoomAddr = 0;
    uintptr_t altAddr = 0;

    MockProcessContext() {
        memory = static_cast<uint8_t*>(VirtualAlloc(
            nullptr,
            size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE
        ));
        assert(memory != nullptr);

        pitchAddr = reinterpret_cast<uintptr_t>(memory + 0x1000);
        zoomAddr = reinterpret_cast<uintptr_t>(memory + 0x2000);
        altAddr = reinterpret_cast<uintptr_t>(memory + 0x3000);

        ResetMemory();
    }

    ~MockProcessContext() {
        if (memory) {
            VirtualFree(memory, 0, MEM_RELEASE);
            memory = nullptr;
        }
    }

    void ResetMemory() {
        std::memset(memory, 0xCC, size);

        // Seed original pitch clamp instructions (16 bytes)
        std::memcpy(
            reinterpret_cast<void*>(pitchAddr),
            SIG_PITCH_CLAMP_PATTERN,
            PITCH_PATCH_LENGTH
        );

        // Seed original zoom clamp instructions (9 bytes)
        std::memcpy(
            reinterpret_cast<void*>(zoomAddr),
            SIG_ZOOM_CLAMP_PATTERN,
            ZOOM_PATCH_LENGTH
        );

        // Seed original altitude write instructions (8 bytes)
        std::memcpy(
            reinterpret_cast<void*>(altAddr),
            SIG_ALTITUDE_WRITE_PATTERN,
            8
        );
    }
};

// ===========================================================================
// 1. Lifecycle and Handle Attachment Tests
// ===========================================================================

bool Test_Patcher_AttachDetachLifecycle() {
    MockProcessContext ctx;
    CameraPatcher patcher;

    TEST_ASSERT(!patcher.IsAttached(), "Patcher should not be attached initially");
    TEST_ASSERT(!patcher.IsPitchUnlocked(), "Pitch should not be unlocked initially");
    TEST_ASSERT(!patcher.IsZoomUnlocked(), "Zoom should not be unlocked initially");
    TEST_ASSERT(patcher.GetAltitudeDelta() == 0.0f, "Altitude delta should be 0.0 initially");

    // Attach to current process mock space
    bool ok = patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory));
    TEST_ASSERT(ok, "Attach should succeed with current process handle");
    TEST_ASSERT(patcher.IsAttached(), "IsAttached should be true after attach");
    TEST_ASSERT(patcher.GetProcessHandle() == GetCurrentProcess(), "Handle should match");

    // Detach
    patcher.Detach();
    TEST_ASSERT(!patcher.IsAttached(), "IsAttached should be false after detach");
    TEST_ASSERT(patcher.GetProcessHandle() == nullptr, "Handle should be cleared after detach");

    return true;
}

// ===========================================================================
// 2. Patch Application and Verification (NOPs)
// ===========================================================================

bool Test_Patcher_PatchApplicationVerification() {
    MockProcessContext ctx;
    CameraPatcher patcher;

    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory)), "Attach failed");
    patcher.SetCustomAddresses(ctx.pitchAddr, ctx.zoomAddr);

    // Apply Pitch Unlock (16 NOPs)
    bool pitchOk = patcher.SetPitchUnlocked(true);
    TEST_ASSERT(pitchOk, "SetPitchUnlocked(true) should succeed");
    TEST_ASSERT(patcher.IsPitchUnlocked(), "IsPitchUnlocked should return true");

    // Verify memory contains 16 NOPs (0x90)
    for (size_t i = 0; i < PITCH_PATCH_LENGTH; ++i) {
        uint8_t byteVal = *(reinterpret_cast<const uint8_t*>(ctx.pitchAddr) + i);
        TEST_ASSERT(byteVal == 0x90, "Pitch memory byte must be 0x90 (NOP)");
    }

    // Apply Zoom Unlock (9 NOPs)
    bool zoomOk = patcher.SetZoomUnlocked(true);
    TEST_ASSERT(zoomOk, "SetZoomUnlocked(true) should succeed");
    TEST_ASSERT(patcher.IsZoomUnlocked(), "IsZoomUnlocked should return true");

    // Verify memory contains 9 NOPs (0x90)
    for (size_t i = 0; i < ZOOM_PATCH_LENGTH; ++i) {
        uint8_t byteVal = *(reinterpret_cast<const uint8_t*>(ctx.zoomAddr) + i);
        TEST_ASSERT(byteVal == 0x90, "Zoom memory byte must be 0x90 (NOP)");
    }

    return true;
}

// ===========================================================================
// 3. Patch Restoration (100% Byte-for-Byte Original Bytes)
// ===========================================================================

bool Test_Patcher_PatchRestorationVerification() {
    MockProcessContext ctx;
    CameraPatcher patcher;

    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory)), "Attach failed");
    patcher.SetCustomAddresses(ctx.pitchAddr, ctx.zoomAddr);

    // Apply both patches
    TEST_ASSERT(patcher.SetPitchUnlocked(true), "Pitch unlock failed");
    TEST_ASSERT(patcher.SetZoomUnlocked(true), "Zoom unlock failed");

    // Restore Pitch
    bool restorePitch = patcher.SetPitchUnlocked(false);
    TEST_ASSERT(restorePitch, "SetPitchUnlocked(false) should succeed");
    TEST_ASSERT(!patcher.IsPitchUnlocked(), "IsPitchUnlocked should be false");
    TEST_ASSERT(std::memcmp(
        reinterpret_cast<const void*>(ctx.pitchAddr),
        SIG_PITCH_CLAMP_PATTERN,
        PITCH_PATCH_LENGTH
    ) == 0, "Pitch memory must match original bytes 100%");

    // Restore Zoom
    bool restoreZoom = patcher.SetZoomUnlocked(false);
    TEST_ASSERT(restoreZoom, "SetZoomUnlocked(false) should succeed");
    TEST_ASSERT(!patcher.IsZoomUnlocked(), "IsZoomUnlocked should be false");
    TEST_ASSERT(std::memcmp(
        reinterpret_cast<const void*>(ctx.zoomAddr),
        SIG_ZOOM_CLAMP_PATTERN,
        ZOOM_PATCH_LENGTH
    ) == 0, "Zoom memory must match original bytes 100%");

    return true;
}

// ===========================================================================
// 4. Idempotency Stress Testing
// ===========================================================================

bool Test_Patcher_IdempotencyStress() {
    MockProcessContext ctx;
    CameraPatcher patcher;

    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory)), "Attach failed");
    patcher.SetCustomAddresses(ctx.pitchAddr, ctx.zoomAddr);

    // Multiple unlock calls in succession
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT(patcher.SetPitchUnlocked(true), "Repeated pitch unlock must succeed");
        TEST_ASSERT(patcher.IsPitchUnlocked(), "Pitch must remain unlocked");
        TEST_ASSERT(patcher.SetZoomUnlocked(true), "Repeated zoom unlock must succeed");
        TEST_ASSERT(patcher.IsZoomUnlocked(), "Zoom must remain unlocked");
    }

    // Verify patch bytes remain 0x90
    for (size_t i = 0; i < PITCH_PATCH_LENGTH; ++i) {
        TEST_ASSERT(*(reinterpret_cast<const uint8_t*>(ctx.pitchAddr) + i) == 0x90, "Must be NOP");
    }
    for (size_t i = 0; i < ZOOM_PATCH_LENGTH; ++i) {
        TEST_ASSERT(*(reinterpret_cast<const uint8_t*>(ctx.zoomAddr) + i) == 0x90, "Must be NOP");
    }

    // Multiple restore calls in succession
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT(patcher.SetPitchUnlocked(false), "Repeated pitch restore must succeed");
        TEST_ASSERT(!patcher.IsPitchUnlocked(), "Pitch must remain locked");
        TEST_ASSERT(patcher.SetZoomUnlocked(false), "Repeated zoom restore must succeed");
        TEST_ASSERT(!patcher.IsZoomUnlocked(), "Zoom must remain locked");
    }

    // Verify pristine original bytes intact
    TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0,
                "Original pitch bytes must remain 100% intact after repeated toggles");
    TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0,
                "Original zoom bytes must remain 100% intact after repeated toggles");

    return true;
}

// ===========================================================================
// 5. Reverse LIFO Rollback Order Verification
// ===========================================================================

bool Test_Patcher_ReverseLIFORollbackOrder() {
    MockProcessContext ctx;

    // Test Case A: Apply Pitch first, then Zoom second -> Rollback Zoom first, then Pitch second
    {
        CameraPatcher patcher;
        TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory)), "Attach failed");
        patcher.SetCustomAddresses(ctx.pitchAddr, ctx.zoomAddr);

        TEST_ASSERT(patcher.SetPitchUnlocked(true), "Pitch unlock failed");
        TEST_ASSERT(patcher.SetZoomUnlocked(true), "Zoom unlock failed");

        // Detach triggers rollback
        patcher.Detach();

        const auto& history = patcher.GetRollbackHistory();
        TEST_ASSERT(history.size() == 2, "Expected exactly 2 rollback actions");
        TEST_ASSERT(history[0] == "Zoom Clamp (Max Distance)", "Zoom should be rolled back first (LIFO)");
        TEST_ASSERT(history[1] == "Pitch Clamp (Field)", "Pitch should be rolled back second (LIFO)");

        // Verify both restored
        TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0, "Pitch restored");
        TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0, "Zoom restored");
    }

    ctx.ResetMemory();

    // Test Case B: Apply Zoom first, then Pitch second -> Rollback Pitch first, then Zoom second
    {
        CameraPatcher patcher;
        TEST_ASSERT(patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory)), "Attach failed");
        patcher.SetCustomAddresses(ctx.pitchAddr, ctx.zoomAddr);

        TEST_ASSERT(patcher.SetZoomUnlocked(true), "Zoom unlock failed");
        TEST_ASSERT(patcher.SetPitchUnlocked(true), "Pitch unlock failed");

        patcher.Detach();

        const auto& history = patcher.GetRollbackHistory();
        TEST_ASSERT(history.size() == 2, "Expected exactly 2 rollback actions");
        TEST_ASSERT(history[0] == "Pitch Clamp (Field)", "Pitch should be rolled back first (LIFO)");
        TEST_ASSERT(history[1] == "Zoom Clamp (Max Distance)", "Zoom should be rolled back second (LIFO)");

        TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0, "Pitch restored");
        TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0, "Zoom restored");
    }

    return true;
}

// ===========================================================================
// 6. Dead PID / Invalid Handle Safety Tests
// ===========================================================================

bool Test_Patcher_DeadProcessAndInvalidHandleSafety() {
    CameraPatcher patcher;

    // 1. Invalid handle attach
    TEST_ASSERT(!patcher.Attach(nullptr, 0x140000000), "Attach with nullptr handle must fail");
    TEST_ASSERT(!patcher.Attach(reinterpret_cast<HANDLE>(0xDEADBEEF), 0x140000000), "Attach with bogus handle must fail");
    TEST_ASSERT(!patcher.Attach(static_cast<DWORD>(0)), "Attach with PID 0 must fail");
    TEST_ASSERT(!patcher.Attach(static_cast<DWORD>(0xFFFFFFFE)), "Attach with non-existent PID must fail");

    // 2. Dead process handle verification
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char cmd[] = "cmd.exe /c exit 0";
    if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        // Process is now terminated (exit code 0 != STILL_ACTIVE)
        CameraPatcher deadPatcher;
        bool attached = deadPatcher.Attach(pi.hProcess, 0x140000000);
        TEST_ASSERT(!attached, "Attach to a terminated/dead process must return false");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // 3. Actions when not attached should safely return false without crashing
    TEST_ASSERT(!patcher.SetPitchUnlocked(true), "SetPitchUnlocked when unattached must return false");
    TEST_ASSERT(!patcher.SetZoomUnlocked(true), "SetZoomUnlocked when unattached must return false");
    TEST_ASSERT(!patcher.AdjustAltitude(5.0f), "AdjustAltitude when unattached must return false");
    TEST_ASSERT(!patcher.ResetDefaults(), "ResetDefaults when unattached must return false");

    // 4. Detach when not attached should be a safe no-op
    patcher.Detach();
    TEST_ASSERT(!patcher.IsAttached(), "Should remain unattached");

    return true;
}

// ===========================================================================
// 7. Altitude Pointer Resolution and Delta Tracking
// ===========================================================================

bool Test_Patcher_AltitudePointerResolutionAndDeltaTracking() {
    CameraPatcher patcher;

    // Set up simulated pointer chain:
    // gmGlobalVar -> bufA
    // bufA + 0x1CE8 -> bufGM
    // bufGM + 0x39F48 -> bufCam
    // bufCam + 0x368 = Eye Y float (15.0f)

    std::vector<uint8_t> bufA(0x2000, 0);
    std::vector<uint8_t> bufGM(0x40000, 0);
    std::vector<uint8_t> bufCam(0x1000, 0);

    const float initialEyeY = 15.0f;
    *reinterpret_cast<float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y) = initialEyeY;

    uintptr_t camPtr = reinterpret_cast<uintptr_t>(bufCam.data());
    std::memcpy(bufGM.data() + OFFSET_GM_TO_ACTIVE_CAMERA, &camPtr, sizeof(camPtr));

    uintptr_t gmPtr = reinterpret_cast<uintptr_t>(bufGM.data());
    std::memcpy(bufA.data() + OFFSET_GM_POINTER_TO_INSTANCE, &gmPtr, sizeof(gmPtr));

    uintptr_t rootPtr = reinterpret_cast<uintptr_t>(bufA.data());
    uintptr_t gmGlobalVar = 0;
    std::memcpy(&gmGlobalVar, &rootPtr, sizeof(rootPtr));

    // Allocate variable to hold the root pointer
    uintptr_t* pGlobalVar = new uintptr_t(rootPtr);
    uintptr_t gmGlobalAddr = reinterpret_cast<uintptr_t>(pGlobalVar);

    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");
    patcher.SetCustomAddresses(0, 0, gmGlobalAddr);

    // 1. Altitude increase by +3.5 meters
    TEST_ASSERT(patcher.AdjustAltitude(3.5f), "AdjustAltitude(+3.5) failed");
    float currentY = *reinterpret_cast<const float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y);
    TEST_ASSERT(currentY == 18.5f, "Altitude should be 15.0 + 3.5 = 18.5");
    TEST_ASSERT(patcher.GetAltitudeDelta() == 3.5f, "Cumulative delta should be 3.5");

    // 2. Altitude decrease by -1.0 meters
    TEST_ASSERT(patcher.AdjustAltitude(-1.0f), "AdjustAltitude(-1.0) failed");
    currentY = *reinterpret_cast<const float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y);
    TEST_ASSERT(currentY == 17.5f, "Altitude should be 18.5 - 1.0 = 17.5");
    TEST_ASSERT(patcher.GetAltitudeDelta() == 2.5f, "Cumulative delta should be 2.5");

    // 3. Reset Defaults restores original altitude
    TEST_ASSERT(patcher.ResetDefaults(), "ResetDefaults failed");
    currentY = *reinterpret_cast<const float*>(bufCam.data() + OFFSET_CAMERA_EYE_Y);
    TEST_ASSERT(currentY == initialEyeY, "Altitude should be reset to initial 15.0");
    TEST_ASSERT(patcher.GetAltitudeDelta() == 0.0f, "Cumulative delta should be reset to 0.0");

    delete pGlobalVar;
    return true;
}

// ===========================================================================
// 8. Altitude Null Pointer Chain Handling
// ===========================================================================

bool Test_Patcher_AltitudeNullPointerHandling() {
    CameraPatcher patcher;
    TEST_ASSERT(patcher.Attach(GetCurrentProcess(), 0x140000000), "Attach failed");

    // Set GM global pointer to a variable containing null pointer (0)
    uintptr_t nullPtr = 0;
    patcher.SetCustomAddresses(0, 0, reinterpret_cast<uintptr_t>(&nullPtr));

    // Calling AdjustAltitude should fail gracefully without crashing
    bool ok = patcher.AdjustAltitude(5.0f);
    TEST_ASSERT(!ok, "AdjustAltitude with null pointer chain must safely return false");
    TEST_ASSERT(patcher.GetAltitudeDelta() == 0.0f, "Delta must remain 0.0 on failure");

    return true;
}

// ===========================================================================
// 9. RAII Detach on Destruction
// ===========================================================================

bool Test_Patcher_RAIIDetachOnDestruction() {
    MockProcessContext ctx;

    {
        CameraPatcher scopedPatcher;
        TEST_ASSERT(scopedPatcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(ctx.memory)), "Attach failed");
        scopedPatcher.SetCustomAddresses(ctx.pitchAddr, ctx.zoomAddr);

        TEST_ASSERT(scopedPatcher.SetPitchUnlocked(true), "Pitch unlock failed");
        TEST_ASSERT(scopedPatcher.SetZoomUnlocked(true), "Zoom unlock failed");

        // Verify patches applied in memory
        TEST_ASSERT(*(reinterpret_cast<const uint8_t*>(ctx.pitchAddr)) == 0x90, "Must be NOP");
        TEST_ASSERT(*(reinterpret_cast<const uint8_t*>(ctx.zoomAddr)) == 0x90, "Must be NOP");
        // scopedPatcher goes out of scope here -> destructor must call Detach()
    }

    // Verify memory was automatically restored by RAII destructor
    TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.pitchAddr), SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0,
                "Pitch memory must be restored after destructor runs");
    TEST_ASSERT(std::memcmp(reinterpret_cast<const void*>(ctx.zoomAddr), SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0,
                "Zoom memory must be restored after destructor runs");

    return true;
}

// ===========================================================================
// 10. Real Disk Game Executable Signature Verification
// ===========================================================================

bool Test_Patcher_RealExecutableSignatures() {
    const wchar_t* envPath = _wgetenv(L"CS3_EXE_PATH");
    if (!envPath || envPath[0] == L'\0') {
        std::cout << "       [SKIP] CS3_EXE_PATH not set; skipping real disk executable signature verification." << std::endl;
        return true;
    }
    auto fileOpt = ProcessScanner::ReadFileToBuffer(std::wstring(envPath));
    TEST_ASSERT(fileOpt.has_value(), "Target game binary ed8_3_PC.exe must be present at CS3_EXE_PATH");
    const auto& buffer = *fileOpt;
    TEST_ASSERT(buffer.size() > 10 * 1024 * 1024, "Binary must be > 10MB");

    // 1. Verify Pitch Clamp instruction matches expected original bytes
    TEST_ASSERT(buffer.size() > OFFSET_PITCH_CLAMP + PITCH_PATCH_LENGTH, "Offset out of bounds");
    TEST_ASSERT(std::memcmp(buffer.data() + OFFSET_PITCH_CLAMP, SIG_PITCH_CLAMP_PATTERN, PITCH_PATCH_LENGTH) == 0,
                "Disk pitch clamp instruction must match SIG_PITCH_CLAMP_PATTERN");

    // 2. Verify Zoom Clamp instruction matches expected original bytes
    TEST_ASSERT(buffer.size() > OFFSET_ZOOM_CLAMP + ZOOM_PATCH_LENGTH, "Offset out of bounds");
    TEST_ASSERT(std::memcmp(buffer.data() + OFFSET_ZOOM_CLAMP, SIG_ZOOM_CLAMP_PATTERN, ZOOM_PATCH_LENGTH) == 0,
                "Disk zoom clamp instruction must match SIG_ZOOM_CLAMP_PATTERN");

    // 3. Verify Altitude Write instruction matches expected original bytes
    TEST_ASSERT(buffer.size() > OFFSET_ALTITUDE_WRITE + 8, "Offset out of bounds");
    TEST_ASSERT(std::memcmp(buffer.data() + OFFSET_ALTITUDE_WRITE, SIG_ALTITUDE_WRITE_PATTERN, 8) == 0,
                "Disk altitude write instruction must match SIG_ALTITUDE_WRITE_PATTERN");

    // 4. Verify GM Resolver instruction and displacement target
    TEST_ASSERT(buffer.size() > OFFSET_GM_RESOLVER + 15, "Offset out of bounds");
    // Verify opcode: 48 8B 05 (mov rax, [rip + disp32])
    TEST_ASSERT(buffer[OFFSET_GM_RESOLVER] == 0x48 &&
                buffer[OFFSET_GM_RESOLVER + 1] == 0x8B &&
                buffer[OFFSET_GM_RESOLVER + 2] == 0x05,
                "GM Resolver opcode must be 48 8B 05");

    int32_t disp32 = 0;
    std::memcpy(&disp32, buffer.data() + OFFSET_GM_RESOLVER + 3, sizeof(disp32));
    uintptr_t calculatedRIP = VA_GM_RESOLVER + 7;
    uintptr_t targetPtr = calculatedRIP + disp32;
    TEST_ASSERT(targetPtr == DEFAULT_GM_GLOBAL_PTR_VA,
                "GM Resolver displacement must resolve exactly to 0x140C53290");

    std::cout << "       [VERIFIED] ed8_3_PC.exe camera structures verified on disk:" << std::endl;
    std::cout << "                  - Pitch clamp: 0x" << std::hex << OFFSET_PITCH_CLAMP << " (VA 0x" << VA_PITCH_CLAMP << ")" << std::endl;
    std::cout << "                  - Zoom clamp:  0x" << OFFSET_ZOOM_CLAMP << " (VA 0x" << VA_ZOOM_CLAMP << ")" << std::endl;
    std::cout << "                  - GM global:   0x" << targetPtr << std::dec << std::endl;

    return true;
}

// ===========================================================================
// Main Unit Test Runner
// ===========================================================================

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << " Cold Steel III - CameraPatcher Unit & Mock Regression Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    RUN_TEST(Test_Patcher_AttachDetachLifecycle);
    RUN_TEST(Test_Patcher_PatchApplicationVerification);
    RUN_TEST(Test_Patcher_PatchRestorationVerification);
    RUN_TEST(Test_Patcher_IdempotencyStress);
    RUN_TEST(Test_Patcher_ReverseLIFORollbackOrder);
    RUN_TEST(Test_Patcher_DeadProcessAndInvalidHandleSafety);
    RUN_TEST(Test_Patcher_AltitudePointerResolutionAndDeltaTracking);
    RUN_TEST(Test_Patcher_AltitudeNullPointerHandling);
    RUN_TEST(Test_Patcher_RAIIDetachOnDestruction);
    RUN_TEST(Test_Patcher_RealExecutableSignatures);

    std::cout << "============================================================" << std::endl;
    std::cout << " Results: Passed: " << g_passedTests << " | Failed: " << g_failedTests << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
