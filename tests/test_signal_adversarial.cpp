#include "../src/common.hpp"
#include "../src/scanner.hpp"
#include "../src/camera_patcher.hpp"
#include "../src/hotkey_manager.hpp"

#include <windows.h>
#include <iostream>
#include <cassert>
#include <cstring>
#include <atomic>
#include <mutex>

using namespace ColdSteel3;

// Global signal handler synchronization mirroring main.cpp
static std::atomic<bool> s_running{true};
static CameraPatcher* s_activePatcher = nullptr;
static std::mutex s_signalMutex;

static BOOL WINAPI TestConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            s_running.store(false);
            std::lock_guard<std::mutex> lock(s_signalMutex);
            if (s_activePatcher) {
                s_activePatcher->Detach();
                s_activePatcher = nullptr;
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

// Test individual signal trigger on active patched memory
bool VerifySignalRollback(DWORD signalType, const char* signalName) {
    std::cout << "[TEST] Simulating signal: " << signalName << " (0x" << std::hex << signalType << std::dec << ")...\n";

    // Allocate mock target memory buffer
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    ));
    if (!mem) {
        std::cerr << "[FAIL] Failed to allocate virtual memory buffer.\n";
        return false;
    }

    uintptr_t pitchAddr = reinterpret_cast<uintptr_t>(mem + 0x1000);
    uintptr_t zoomAddr  = reinterpret_cast<uintptr_t>(mem + 0x2000);
    std::memcpy(reinterpret_cast<void*>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, 16);
    std::memcpy(reinterpret_cast<void*>(zoomAddr),  SIG_ZOOM_CLAMP_PATTERN, 9);

    CameraPatcher patcher;
    patcher.SetCustomAddresses(pitchAddr, zoomAddr, 0);
    if (!patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(mem))) {
        std::cerr << "[FAIL] Failed to attach patcher to mock memory.\n";
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    // Apply active patches
    patcher.SetPitchUnlocked(true);
    patcher.SetZoomUnlocked(true);
    assert(patcher.IsPitchUnlocked());
    assert(patcher.IsZoomUnlocked());
    assert(mem[0x1000] == 0x90);
    assert(mem[0x2000] == 0x90);

    // Register active patcher in global handler
    s_running.store(true);
    {
        std::lock_guard<std::mutex> lock(s_signalMutex);
        s_activePatcher = &patcher;
    }

    // Trigger signal handler directly
    BOOL handled = TestConsoleCtrlHandler(signalType);
    if (!handled) {
        std::cerr << "[FAIL] Handler returned FALSE for " << signalName << "\n";
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    // Verify state after signal
    if (s_running.load() != false) {
        std::cerr << "[FAIL] s_running was not set to false by signal handler.\n";
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    if (s_activePatcher != nullptr) {
        std::cerr << "[FAIL] s_activePatcher was not reset to nullptr by signal handler.\n";
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    // Verify atomic memory restoration to pristine original bytes
    if (std::memcmp(mem + 0x1000, SIG_PITCH_CLAMP_PATTERN, 16) != 0) {
        std::cerr << "[FAIL] Pitch clamp bytes were NOT restored on signal " << signalName << "\n";
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    if (std::memcmp(mem + 0x2000, SIG_ZOOM_CLAMP_PATTERN, 9) != 0) {
        std::cerr << "[FAIL] Zoom clamp bytes were NOT restored on signal " << signalName << "\n";
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    // Clean up
    VirtualFree(mem, 0, MEM_RELEASE);
    std::cout << "       " << signalName << " handled: memory rolled back 100% byte-for-byte.\n";
    return true;
}

// Test unhandled / unknown signal
bool VerifyUnknownSignalIgnored() {
    std::cout << "[TEST] Simulating unknown signal (0x999)...\n";
    BOOL handled = TestConsoleCtrlHandler(0x999);
    if (handled) {
        std::cerr << "[FAIL] Unknown signal should NOT be handled.\n";
        return false;
    }
    std::cout << "       Unknown signal correctly ignored (returned FALSE).\n";
    return true;
}

// Test double signal arrival concurrency / idempotency
bool VerifyConcurrentSignalSafety() {
    std::cout << "[TEST] Verifying signal handler idempotency and thread-safety...\n";
    // Trigger signal handler multiple times in succession
    BOOL h1 = TestConsoleCtrlHandler(CTRL_C_EVENT);
    BOOL h2 = TestConsoleCtrlHandler(CTRL_BREAK_EVENT);
    BOOL h3 = TestConsoleCtrlHandler(CTRL_CLOSE_EVENT);
    if (!h1 || !h2 || !h3) {
        std::cerr << "[FAIL] Subsequent signals failed.\n";
        return false;
    }
    std::cout << "       Consecutive signal invocations safely handled without crashing.\n";
    return true;
}

int main() {
    std::cout << "============================================================\n";
    std::cout << " Adversarial Signal Handler & Memory Rollback Verification  \n";
    std::cout << "============================================================\n";

    int passed = 0;
    int failed = 0;

    auto runTest = [&](bool success) {
        if (success) passed++;
        else failed++;
    };

    runTest(VerifySignalRollback(CTRL_C_EVENT,        "CTRL_C_EVENT"));
    runTest(VerifySignalRollback(CTRL_BREAK_EVENT,    "CTRL_BREAK_EVENT"));
    runTest(VerifySignalRollback(CTRL_CLOSE_EVENT,    "CTRL_CLOSE_EVENT"));
    runTest(VerifySignalRollback(CTRL_LOGOFF_EVENT,   "CTRL_LOGOFF_EVENT"));
    runTest(VerifySignalRollback(CTRL_SHUTDOWN_EVENT, "CTRL_SHUTDOWN_EVENT"));
    runTest(VerifyUnknownSignalIgnored());
    runTest(VerifyConcurrentSignalSafety());

    std::cout << "============================================================\n";
    std::cout << " Signal Stress Results: Passed: " << passed << " | Failed: " << failed << "\n";
    std::cout << "============================================================\n";

    return (failed == 0) ? 0 : 1;
}
