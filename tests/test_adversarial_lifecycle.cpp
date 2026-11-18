#include "../src/camera_patcher.hpp"
#include "../src/scanner.hpp"
#include "../src/common.hpp"

#include <windows.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
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

// Helper: Spawn a test child process ("cmd.exe /c pause")
static bool SpawnTestProcess(PROCESS_INFORMATION& outPi) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    char cmdLine[] = "cmd.exe /c pause";
    BOOL success = CreateProcessA(
        nullptr, cmdLine, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &outPi
    );
    return success == TRUE;
}

// ===========================================================================
// Test 1: Pathological Handle Invalidation
// ===========================================================================
bool Test_PathologicalHandle_Invalidation() {
    CameraPatcher patcher;

    // 1.1 NULL handle
    TEST_ASSERT(!patcher.Attach(nullptr, 0x140000000), "Attach with NULL handle must fail");
    TEST_ASSERT(!patcher.IsAttached(), "Patcher must not be attached after NULL handle");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false after NULL handle");
    patcher.Detach(); // Must be a clean no-op
    TEST_ASSERT(!patcher.IsAttached(), "Patcher must remain unattached");

    // 1.2 Arbitrary invalid handle (0xDEADBEEF)
    HANDLE hBogus = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xDEADBEEF));
    TEST_ASSERT(!patcher.Attach(hBogus, 0x140000000), "Attach with bogus handle must fail");
    TEST_ASSERT(!patcher.IsAttached(), "Patcher must not be attached after bogus handle");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false after bogus handle");
    patcher.Detach();

    // 1.3 Closed Process Handle
    PROCESS_INFORMATION pi{};
    TEST_ASSERT(SpawnTestProcess(pi), "Failed to spawn test process");
    HANDLE hDup = nullptr;
    DuplicateHandle(
        GetCurrentProcess(), pi.hProcess,
        GetCurrentProcess(), &hDup,
        0, FALSE, DUPLICATE_SAME_ACCESS
    );
    TEST_ASSERT(hDup != nullptr, "Failed to duplicate handle");
    CloseHandle(hDup); // Immediately close it

    TEST_ASSERT(!patcher.Attach(hDup, 0x140000000), "Attach with closed handle must fail");
    TEST_ASSERT(!patcher.IsAttached(), "Patcher must not be attached after closed handle");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false after closed handle");

    // 1.4 Non-process handle (Event handle)
    HANDLE hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    TEST_ASSERT(hEvent != nullptr, "Failed to create event");
    TEST_ASSERT(!patcher.Attach(hEvent, 0x140000000), "Attach with event handle must fail");
    TEST_ASSERT(!patcher.IsAttached(), "Patcher must not be attached after event handle");
    TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false after event handle");
    CloseHandle(hEvent);

    // 1.5 Handle with only SYNCHRONIZE access (no query rights)
    HANDLE hSyncOnly = OpenProcess(SYNCHRONIZE, FALSE, pi.dwProcessId);
    if (hSyncOnly) {
        // Since GetExitCodeProcess requires query rights, attaching with SYNCHRONIZE only
        // must fail cleanly without crash
        bool attached = patcher.Attach(hSyncOnly, 0x140000000);
        TEST_ASSERT(!attached, "Attach with SYNCHRONIZE-only handle must fail cleanly");
        TEST_ASSERT(!patcher.IsAttached(), "Patcher must not be attached with SYNCHRONIZE-only handle");
        TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false with SYNCHRONIZE-only handle");
        CloseHandle(hSyncOnly);
    }

    // 1.6 Handle with PROCESS_QUERY_LIMITED_INFORMATION
    HANDLE hQueryLimited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId);
    if (hQueryLimited) {
        bool attached = patcher.Attach(hQueryLimited, 0x140000000);
        TEST_ASSERT(attached, "Attach with PROCESS_QUERY_LIMITED_INFORMATION should succeed");
        TEST_ASSERT(patcher.IsAttached(), "Patcher must be attached");
        TEST_ASSERT(patcher.IsProcessAlive(), "IsProcessAlive must be true for running process with query rights");
        patcher.Detach();
        CloseHandle(hQueryLimited);
    }

    // Clean up test child process
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return true;
}

// ===========================================================================
// Test 2: Rapid Spawning, Abrupt Termination, and Zero Handle Leaks
// ===========================================================================
bool Test_RapidSpawning_SuddenKill_And_HandleLeaks() {
    CameraPatcher patcher;

    DWORD initialHandleCount = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &initialHandleCount)) {
        initialHandleCount = 0;
    }

    constexpr int ITERATIONS = 30;
    for (int i = 0; i < ITERATIONS; ++i) {
        PROCESS_INFORMATION pi{};
        TEST_ASSERT(SpawnTestProcess(pi), "Failed to spawn test process in rapid loop");

        // Allocate mock memory inside the child process address space
        LPVOID childMem = VirtualAllocEx(
            pi.hProcess, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        TEST_ASSERT(childMem != nullptr, "VirtualAllocEx in child process failed");

        uintptr_t pitchAddr = reinterpret_cast<uintptr_t>(childMem) + 0x100;
        uintptr_t zoomAddr = reinterpret_cast<uintptr_t>(childMem) + 0x200;

        // Initialize target bytes inside child process memory
        SIZE_T written = 0;
        WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, 16, &written);
        WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(zoomAddr), SIG_ZOOM_CLAMP_PATTERN, 9, &written);

        patcher.SetCustomAddresses(pitchAddr, zoomAddr, 0);
        TEST_ASSERT(patcher.Attach(pi.hProcess, reinterpret_cast<uintptr_t>(childMem)), "Attach failed");
        TEST_ASSERT(patcher.IsAttached(), "Should be attached");
        TEST_ASSERT(patcher.IsProcessAlive(), "Should be alive");

        // Apply patches while process runs
        TEST_ASSERT(patcher.SetPitchUnlocked(true), "SetPitchUnlocked failed");
        TEST_ASSERT(patcher.SetZoomUnlocked(true), "SetZoomUnlocked failed");
        TEST_ASSERT(patcher.IsPitchUnlocked(), "Pitch should be unlocked");
        TEST_ASSERT(patcher.IsZoomUnlocked(), "Zoom should be unlocked");

        // Sudden termination (SIGKILL equivalent: TerminateProcess with arbitrary exit code)
        DWORD termExitCode = 42 + i;
        TerminateProcess(pi.hProcess, termExitCode);
        WaitForSingleObject(pi.hProcess, 1000);

        // Immediate check: IsProcessAlive MUST transition to false!
        TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive MUST be false immediately after termination");

        // Detach on dead process: must NOT crash, must NOT try writing to memory, must reset state
        patcher.Detach();
        TEST_ASSERT(!patcher.IsAttached(), "Must be unattached after Detach");
        TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must remain false");

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    DWORD finalHandleCount = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &finalHandleCount) && initialHandleCount > 0) {
        int diff = static_cast<int>(finalHandleCount) - static_cast<int>(initialHandleCount);
        std::cout << "       Handle count before: " << initialHandleCount 
                  << ", after: " << finalHandleCount << " (diff: " << diff << ")" << std::endl;
        TEST_ASSERT(diff < 5, "Detected handle leak across rapid process lifecycle cycles");
    }

    return true;
}

// ===========================================================================
// Test 3: ExitCode 259 (STILL_ACTIVE) Edge Case Analysis
// ===========================================================================
bool Test_ExitCode259_Pathology() {
    CameraPatcher patcher;
    PROCESS_INFORMATION pi{};
    TEST_ASSERT(SpawnTestProcess(pi), "Failed to spawn test process");

    LPVOID childMem = VirtualAllocEx(
        pi.hProcess, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    );
    TEST_ASSERT(childMem != nullptr, "VirtualAllocEx in child process failed");

    TEST_ASSERT(patcher.Attach(pi.hProcess, reinterpret_cast<uintptr_t>(childMem)), "Attach failed");
    TEST_ASSERT(patcher.IsAttached(), "Should be attached");
    TEST_ASSERT(patcher.IsProcessAlive(), "Should be alive while running");

    // Kill process specifically with exit code 259 (STILL_ACTIVE)
    std::cout << "       Terminating process with exit code 259 (STILL_ACTIVE)..." << std::endl;
    TerminateProcess(pi.hProcess, 259);
    WaitForSingleObject(pi.hProcess, 1000);

    // Empirically observe what GetExitCodeProcess and CameraPatcher report
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    std::cout << "       Raw GetExitCodeProcess result: " << exitCode << " (STILL_ACTIVE=" << STILL_ACTIVE << ")" << std::endl;

    bool reportedAlive = patcher.IsProcessAlive();
    std::cout << "       CameraPatcher::IsProcessAlive() reports: " << (reportedAlive ? "ALIVE (true)" : "DEAD (false)") << std::endl;

    patcher.Detach();
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Must report false even when process terminated with exitCode 259 (STILL_ACTIVE)
    TEST_ASSERT(!reportedAlive, "CameraPatcher::IsProcessAlive() must return false for dead process even if exit code is 259");

    return true;
}

// ===========================================================================
// Test 4: Concurrent Access Under Process Death Stress
// ===========================================================================
bool Test_ConcurrentStress_UnderProcessDeath() {
    constexpr int NUM_CYCLES = 5;

    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        CameraPatcher patcher;
        PROCESS_INFORMATION pi{};
        TEST_ASSERT(SpawnTestProcess(pi), "Failed to spawn process for concurrency test");

        LPVOID childMem = VirtualAllocEx(
            pi.hProcess, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        TEST_ASSERT(childMem != nullptr, "VirtualAllocEx failed");

        uintptr_t pitchAddr = reinterpret_cast<uintptr_t>(childMem) + 0x100;
        uintptr_t zoomAddr = reinterpret_cast<uintptr_t>(childMem) + 0x200;
        SIZE_T written = 0;
        WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, 16, &written);
        WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(zoomAddr), SIG_ZOOM_CLAMP_PATTERN, 9, &written);
        patcher.SetCustomAddresses(pitchAddr, zoomAddr, 0);

        TEST_ASSERT(patcher.Attach(pi.hProcess, reinterpret_cast<uintptr_t>(childMem)), "Attach failed");

        std::atomic<bool> stopWorkers{false};
        std::atomic<int> queryCount{0};

        // Worker 1: Rapid query loop
        std::thread queryWorker([&]() {
            while (!stopWorkers.load()) {
                patcher.IsProcessAlive();
                patcher.IsAttached();
                patcher.IsPitchUnlocked();
                patcher.IsZoomUnlocked();
                patcher.GetAltitudeDelta();
                queryCount.fetch_add(1);
            }
        });

        // Worker 2: Rapid patch toggle loop
        std::thread patchWorker([&]() {
            bool state = false;
            while (!stopWorkers.load()) {
                state = !state;
                patcher.SetPitchUnlocked(state);
                patcher.SetZoomUnlocked(state);
            }
        });

        // Main thread: let workers run for 50ms, then suddenly kill process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TerminateProcess(pi.hProcess, 123);
        WaitForSingleObject(pi.hProcess, 500);

        // Let workers continue querying dead process for another 50ms
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Stop workers
        stopWorkers.store(true);
        queryWorker.join();
        patchWorker.join();

        // Must accurately reflect dead state
        TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must be false after death under concurrency");

        // Concurrent detach test
        std::thread detachThread1([&]() { patcher.Detach(); });
        std::thread detachThread2([&]() { patcher.Detach(); });
        detachThread1.join();
        detachThread2.join();

        TEST_ASSERT(!patcher.IsAttached(), "Must be unattached after concurrent detach");

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    return true;
}

// ===========================================================================
// Test 5: Multiple Consecutive Detach Calls (Idempotency)
// ===========================================================================
bool Test_MultipleDetach_Idempotency() {
    CameraPatcher patcher;

    // 5.1 Call Detach on unattached patcher repeatedly
    for (int i = 0; i < 50; ++i) {
        patcher.Detach();
        TEST_ASSERT(!patcher.IsAttached(), "Should remain unattached");
        TEST_ASSERT(!patcher.IsProcessAlive(), "Should remain dead");
    }

    // 5.2 Attach to live process and detach repeatedly
    PROCESS_INFORMATION pi{};
    TEST_ASSERT(SpawnTestProcess(pi), "Failed to spawn process");

    LPVOID childMem = VirtualAllocEx(
        pi.hProcess, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    );
    TEST_ASSERT(childMem != nullptr, "VirtualAllocEx failed");

    TEST_ASSERT(patcher.Attach(pi.hProcess, reinterpret_cast<uintptr_t>(childMem)), "Attach failed");
    TEST_ASSERT(patcher.IsAttached(), "Should be attached");

    for (int i = 0; i < 50; ++i) {
        patcher.Detach();
        TEST_ASSERT(!patcher.IsAttached(), "Should be unattached on iteration");
        TEST_ASSERT(!patcher.IsProcessAlive(), "Should be false on iteration");
    }

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return true;
}

// ===========================================================================
// Test 6: Process Auto-Recovery Simulation Loop
// ===========================================================================
bool Test_ProcessAutoRecovery_Simulation() {
    CameraPatcher patcher;

    constexpr int CYCLES = 5;
    for (int c = 0; c < CYCLES; ++c) {
        // Step 1: Game process starts
        PROCESS_INFORMATION pi{};
        TEST_ASSERT(SpawnTestProcess(pi), "Failed to spawn process in cycle");

        LPVOID childMem = VirtualAllocEx(
            pi.hProcess, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        TEST_ASSERT(childMem != nullptr, "VirtualAllocEx failed");

        uintptr_t pitchAddr = reinterpret_cast<uintptr_t>(childMem) + 0x100;
        uintptr_t zoomAddr = reinterpret_cast<uintptr_t>(childMem) + 0x200;
        SIZE_T written = 0;
        WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, 16, &written);
        WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(zoomAddr), SIG_ZOOM_CLAMP_PATTERN, 9, &written);
        patcher.SetCustomAddresses(pitchAddr, zoomAddr, 0);

        // Step 2: Utility detects and hooks
        TEST_ASSERT(patcher.Attach(pi.hProcess, reinterpret_cast<uintptr_t>(childMem)), "Attach failed");
        TEST_ASSERT(patcher.IsAttached(), "Must be attached");
        TEST_ASSERT(patcher.IsProcessAlive(), "Must be alive");

        // Step 3: Toggles active
        TEST_ASSERT(patcher.SetPitchUnlocked(true), "Pitch unlock failed");
        TEST_ASSERT(patcher.SetZoomUnlocked(true), "Zoom unlock failed");
        TEST_ASSERT(patcher.IsPitchUnlocked(), "Pitch unlocked");
        TEST_ASSERT(patcher.IsZoomUnlocked(), "Zoom unlocked");

        // Step 4: Game crashes or closes
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 500);

        // Step 5: Monitoring loop detects termination
        TEST_ASSERT(!patcher.IsProcessAlive(), "IsProcessAlive must detect termination");

        // Step 6: Detach and reset to search state
        patcher.Detach();
        TEST_ASSERT(!patcher.IsAttached(), "Must be detached");
        TEST_ASSERT(!patcher.IsProcessAlive(), "Must be dead");

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    return true;
}

// ===========================================================================
// Main Runner
// ===========================================================================
int main() {
    std::cout << "============================================================\n";
    std::cout << " Adversarial Process Lifecycle & Auto-Recovery Stress Suite\n";
    std::cout << "============================================================\n";

    RUN_TEST(Test_PathologicalHandle_Invalidation);
    RUN_TEST(Test_RapidSpawning_SuddenKill_And_HandleLeaks);
    RUN_TEST(Test_ExitCode259_Pathology);
    RUN_TEST(Test_ConcurrentStress_UnderProcessDeath);
    RUN_TEST(Test_MultipleDetach_Idempotency);
    RUN_TEST(Test_ProcessAutoRecovery_Simulation);

    std::cout << "============================================================\n";
    std::cout << " Results: Passed: " << g_passedTests << " | Failed: " << g_failedTests << "\n";
    std::cout << "============================================================\n";

    return (g_failedTests == 0) ? 0 : 1;
}
