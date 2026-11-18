#include "common.hpp"
#include "scanner.hpp"
#include "camera_patcher.hpp"
#include "hotkey_manager.hpp"
#include "logger.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstring>
#include <optional>

namespace ColdSteel3 {

// Global synchronization for Win32 console signal termination
static std::atomic<bool> g_running{true};
static CameraPatcher* g_activePatcher = nullptr;
static std::mutex g_signalMutex;

// Win32 Console Control Handler for clean detachment across all OS signals
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            g_running.store(false);
            std::lock_guard<std::mutex> lock(g_signalMutex);
            if (g_activePatcher) {
                g_activePatcher->Detach();
                g_activePatcher = nullptr;
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

// Enable Windows 10/11 ANSI / Virtual Terminal processing for colored console text
static void EnableVirtualTerminal() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
}

// Clear screen or reposition cursor
static void ResetConsoleCursor() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        COORD coord = { 0, 0 };
        SetConsoleCursorPosition(hOut, coord);
    }
}

// Render Interactive HUD
static void RenderHUD(const CameraPatcher& patcher, bool isSearching, DWORD pid, uintptr_t moduleBase) {
    ResetConsoleCursor();

    std::cout << "\033[1;36m======================================================================\033[0m\n";
    std::cout << "\033[1;37m   Cold Steel III Camera Tweak Utility (v1.0.7 x64)                  \033[0m\n";
    std::cout << "\033[1;36m======================================================================\033[0m\n";

    if (isSearching) {
        std::cout << "  Process Status : \033[1;33mSearching for ed8_3_PC.exe...\033[0m                     \n";
        std::cout << "  Module Base    : \033[90mWaiting for game start...                   \033[0m\n";
    } else {
        std::cout << "  Process Status : \033[1;32mAttached to ed8_3_PC.exe (PID: " << pid << ")\033[0m         \n";
        std::cout << "  Module Base    : \033[1;37m0x" << std::hex << std::uppercase << moduleBase << std::dec << "\033[0m                               \n";
    }

    std::cout << "\033[1;36m----------------------------------------------------------------------\033[0m\n";
    std::cout << "\033[1;37m  LIVE TWEAK STATUS INDICATORS:\033[0m                                        \n";

    // Pitch status
    if (patcher.IsPitchUnlocked()) {
        std::cout << "  Pitch Angle    : \033[1;32m[UNLOCKED]\033[0m (Full 360 vertical tilt)           \n";
    } else {
        std::cout << "  Pitch Angle    : \033[1;33m[DEFAULT]\033[0m  (Game vertical limits active)     \n";
    }

    // Zoom status
    if (patcher.IsZoomUnlocked()) {
        std::cout << "  Zoom Distance  : \033[1;32m[UNLOCKED]\033[0m (Uncapped camera distance)        \n";
    } else {
        std::cout << "  Zoom Distance  : \033[1;33m[DEFAULT]\033[0m  (Max zoom distance clamped)       \n";
    }

    // Altitude delta status
    const float altDelta = patcher.GetAltitudeDelta();
    if (altDelta == 0.0f) {
        std::cout << "  Altitude Delta : \033[1;33m[DEFAULT]\033[0m  (+0.00 units)                     \n";
    } else {
        std::cout << "  Altitude Delta : \033[1;32m[" << ((altDelta > 0.0f) ? "+" : "")
                  << std::fixed << std::setprecision(2) << altDelta << " units]\033[0m                      \n";
    }

    std::cout << "\033[1;36m----------------------------------------------------------------------\033[0m\n";
    std::cout << "\033[1;37m  HOTKEY CHEAT SHEET (No NumPad / No Insert / No F4):\033[0m                  \n";
    std::cout << "  \033[1;35m[F1]\033[0m or \033[1;35m[F5]\033[0m   : Toggle 360 Pitch Unlock (Overhead & Under Shoes)\n";
    std::cout << "  \033[1;35m[F2]\033[0m or \033[1;35m[F6]\033[0m   : Toggle Zoom Distance Unlock (Uncap distance)    \n";
    std::cout << "  \033[1;35m[PgUp]\033[0m / \033[1;35m[PgDn]\033[0m : Camera Height (Altitude) Up / Down              \n";
    std::cout << "  \033[1;35m[F7]\033[0m / \033[1;35m[F8]\033[0m     : Camera Height (Altitude) Up / Down (Alt keys)   \n";
    std::cout << "  \033[1;35m[ [ ]\033[0m / \033[1;35m[ ] ]\033[0m   : Zoom In / Zoom Out (or \033[1;35m[=]\033[0m / \033[1;35m[-]\033[0m keys)          \n";
    std::cout << "  \033[1;35m[F9]\033[0m or \033[1;35m[Home]\033[0m : Reset Camera to Game Defaults                   \n";
    std::cout << "  \033[1;35m[F12]\033[0m or \033[1;35m[End] \033[0m : Clean Detach & Exit                             \n";
    std::cout << "  \033[1;34m[Shift]\033[0m (Hold)    : 3.0x Speed Boost                                \n";
    std::cout << "  \033[1;34m[Ctrl] \033[0m (Hold)    : 0.1x Micro Precision                            \n";
    std::cout << "\033[1;36m======================================================================\033[0m\n";
    std::cout.flush();
}

// Headless self-test routine for automated CI / test validation
static int RunHeadlessSelfTest(const std::wstring& gameBinPath) {
    std::cout << "============================================================\n";
    std::cout << " Cold Steel III Camera Tweak - Headless Self-Test Suite\n";
    std::cout << "============================================================\n";

    // 1. Test AOB signature definitions
    std::cout << "[TEST] Validating memory signatures definitions...\n";
    if (std::string(SIG_PITCH_CLAMP.name).empty() || SIG_PITCH_CLAMP.length != 16) {
        std::cerr << "[FAIL] Pitch clamp signature invalid.\n";
        return 1;
    }
    if (std::string(SIG_ZOOM_CLAMP.name).empty() || SIG_ZOOM_CLAMP.length != 9) {
        std::cerr << "[FAIL] Zoom clamp signature invalid.\n";
        return 1;
    }
    std::cout << "       Pitch clamp length: " << SIG_PITCH_CLAMP.length << " bytes\n";
    std::cout << "       Zoom clamp length:  " << SIG_ZOOM_CLAMP.length << " bytes\n";
    std::cout << "[PASS] Signatures verified.\n";

    // 2. Test On-disk verification if binary is available
    std::cout << "[TEST] Checking for game executable on disk...\n";
    std::optional<std::vector<uint8_t>> buf;
    if (!gameBinPath.empty()) {
        buf = ProcessScanner::ReadFileToBuffer(gameBinPath);
    }
    if (buf.has_value() && !buf->empty()) {
        std::cout << "       Found game binary (" << buf->size() << " bytes)\n";
        size_t offPitch = ProcessScanner::ScanPatternOnDisk(buf->data(), buf->size(), SIG_PITCH_CLAMP);
        size_t offZoom = ProcessScanner::ScanPatternOnDisk(buf->data(), buf->size(), SIG_ZOOM_CLAMP);
        if (offPitch != OFFSET_PITCH_CLAMP || offZoom != OFFSET_ZOOM_CLAMP) {
            std::cerr << "[FAIL] Pattern match offset mismatch on disk.\n";
            return 1;
        }
        std::cout << "       Pitch clamp matched at offset 0x" << std::hex << offPitch << std::dec << "\n";
        std::cout << "       Zoom clamp matched at offset  0x" << std::hex << offZoom << std::dec << "\n";
        std::cout << "[PASS] On-disk binary signatures matched 100% uniquely.\n";
    } else {
        std::cout << "       (No game path provided or file unreadable; skipping disk verification. Use: --test \"<path to ed8_3_PC.exe>\")\n";
    }

    // 3. Test CameraPatcher mock memory workflow
    std::cout << "[TEST] Validating CameraPatcher memory transactional mock...\n";
    {
        uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        ));
        if (!mem) {
            std::cerr << "[FAIL] Failed to allocate mock test buffer.\n";
            return 1;
        }

        uintptr_t pitchAddr = reinterpret_cast<uintptr_t>(mem + 0x1000);
        uintptr_t zoomAddr = reinterpret_cast<uintptr_t>(mem + 0x2000);
        std::memcpy(reinterpret_cast<void*>(pitchAddr), SIG_PITCH_CLAMP_PATTERN, 16);
        std::memcpy(reinterpret_cast<void*>(zoomAddr), SIG_ZOOM_CLAMP_PATTERN, 9);

        CameraPatcher patcher;
        patcher.SetCustomAddresses(pitchAddr, zoomAddr, 0);
        if (!patcher.Attach(GetCurrentProcess(), reinterpret_cast<uintptr_t>(mem))) {
            std::cerr << "[FAIL] Failed to attach patcher to self-process.\n";
            VirtualFree(mem, 0, MEM_RELEASE);
            return 1;
        }

        if (!patcher.IsProcessAlive()) {
            std::cerr << "[FAIL] Attached patcher reported dead process.\n";
            VirtualFree(mem, 0, MEM_RELEASE);
            return 1;
        }

        // Test pitch toggle
        patcher.SetPitchUnlocked(true);
        if (!patcher.IsPitchUnlocked() || mem[0x1000] != 0x90) {
            std::cerr << "[FAIL] Pitch unlock patch failed.\n";
            VirtualFree(mem, 0, MEM_RELEASE);
            return 1;
        }

        // Test zoom toggle
        patcher.SetZoomUnlocked(true);
        if (!patcher.IsZoomUnlocked() || mem[0x2000] != 0x90) {
            std::cerr << "[FAIL] Zoom unlock patch failed.\n";
            VirtualFree(mem, 0, MEM_RELEASE);
            return 1;
        }

        // Test clean detach rollback
        patcher.Detach();
        if (patcher.IsProcessAlive()) {
            std::cerr << "[FAIL] Detached patcher reported alive process.\n";
            VirtualFree(mem, 0, MEM_RELEASE);
            return 1;
        }

        if (std::memcmp(mem + 0x1000, SIG_PITCH_CLAMP_PATTERN, 16) != 0 ||
            std::memcmp(mem + 0x2000, SIG_ZOOM_CLAMP_PATTERN, 9) != 0) {
            std::cerr << "[FAIL] Patch restoration mismatch.\n";
            VirtualFree(mem, 0, MEM_RELEASE);
            return 1;
        }

        VirtualFree(mem, 0, MEM_RELEASE);
        std::cout << "[PASS] CameraPatcher transactional rollback verified.\n";
    }

    // 4. Test HotkeyManager debounce and modifiers
    std::cout << "[TEST] Validating HotkeyManager debouncing and modifiers...\n";
    {
        HotkeyManager hotkeyMgr;
        int togglesFired = 0;
        hotkeyMgr.SetOnTogglePitch([&]() { togglesFired++; });

        std::array<bool, 256> mockKeys{};
        hotkeyMgr.SetKeyStateProvider([&](int key) {
            return (key >= 0 && key < 256) ? mockKeys[key] : false;
        });

        // Test modifier calculation
        if (hotkeyMgr.CalculateSpeedMultiplier() != 1.0f) {
            std::cerr << "[FAIL] Baseline speed modifier expected 1.0f.\n";
            return 1;
        }
        mockKeys[VK_SHIFT] = true;
        if (hotkeyMgr.CalculateSpeedMultiplier() != 3.0f) {
            std::cerr << "[FAIL] Shift speed modifier expected 3.0f.\n";
            return 1;
        }
        mockKeys[VK_SHIFT] = false;
        mockKeys[VK_CONTROL] = true;
        if (hotkeyMgr.CalculateSpeedMultiplier() != 0.1f) {
            std::cerr << "[FAIL] Ctrl speed modifier expected 0.1f.\n";
            return 1;
        }
        mockKeys[VK_CONTROL] = false;

        // Test debounce
        auto t0 = std::chrono::steady_clock::now();
        mockKeys[VK_F1] = true;
        hotkeyMgr.Update(t0); // 1st press fires
        if (togglesFired != 1) {
            std::cerr << "[FAIL] First toggle press failed to fire.\n";
            return 1;
        }

        // Key held down -> should not fire again
        hotkeyMgr.Update(t0 + std::chrono::milliseconds(50));
        if (togglesFired != 1) {
            std::cerr << "[FAIL] Edge-trigger failed: key held down fired repeatedly.\n";
            return 1;
        }

        // Release and rapid repress within 100ms (< 200ms lockout)
        mockKeys[VK_F1] = false;
        hotkeyMgr.Update(t0 + std::chrono::milliseconds(60));
        mockKeys[VK_F1] = true;
        hotkeyMgr.Update(t0 + std::chrono::milliseconds(100));
        if (togglesFired != 1) {
            std::cerr << "[FAIL] 200ms lockout debounce failed: rapid repress fired.\n";
            return 1;
        }

        // Repress after 250ms (> 200ms lockout)
        mockKeys[VK_F1] = false;
        hotkeyMgr.Update(t0 + std::chrono::milliseconds(210));
        mockKeys[VK_F1] = true;
        hotkeyMgr.Update(t0 + std::chrono::milliseconds(250));
        if (togglesFired != 2) {
            std::cerr << "[FAIL] Debounce failed: press after lockout did not fire.\n";
            return 1;
        }

        std::cout << "[PASS] HotkeyManager edge-trigger and debouncing verified.\n";
    }

    std::cout << "============================================================\n";
    std::cout << "[PASS] All internal self-tests passed successfully.\n";
    std::cout << "============================================================\n";
    return 0;
}

// Print version information
static void PrintVersion() {
    std::cout << "Cold Steel III Camera Tweak Utility (v1.0.7 x64)\n";
    std::cout << "Target: The Legend of Heroes: Trails of Cold Steel III (ed8_3_PC.exe v1.0.7)\n";
    std::cout << "Architecture: x86_64 Windows (Static C++17 standalone binary)\n";
}

// Print command-line help
static void PrintHelp() {
    std::cout << "Usage: coldsteel3_camera_tweak.exe [\"<path to ed8_3_PC.exe>\"] [options]\n\n";
    std::cout << "If no path is given, the tool asks for it at startup.\n\n";
    std::cout << "Options:\n";
    std::cout << "  --help, -h       Display this help message and exit\n";
    std::cout << "  --version, -v    Display version information and exit\n";
    std::cout << "  --test [path]    Run headless self-tests and exit (optional path to ed8_3_PC.exe for disk check)\n\n";
    std::cout << "Hotkeys (when attached to game):\n";
    std::cout << "  F1 or F5       Toggle 360 Pitch Unlock (Overhead & Under Shoes)\n";
    std::cout << "  F2 or F6       Toggle Zoom Distance Unlock (Uncap distance)\n";
    std::cout << "  PgUp / PgDn    Camera Height (Altitude) Up / Down\n";
    std::cout << "  F7 / F8        Camera Height (Altitude) Up / Down (Alt keys)\n";
    std::cout << "  [ or =         Zoom In dynamically\n";
    std::cout << "  ] or -         Zoom Out dynamically\n";
    std::cout << "  F9 or Home     Reset Camera to Game Defaults\n";
    std::cout << "  F12 or End     Clean Detach & Exit\n";
    std::cout << "  Shift (Hold)   3.0x Speed Boost\n";
    std::cout << "  Ctrl  (Hold)   0.1x Micro Precision\n";
}

// Read the game executable path from the user (command-line argument or interactive prompt)
static std::wstring PromptForGamePath(const std::wstring& initial) {
    std::wstring path = initial;
    for (;;) {
        if (path.empty()) {
            std::wcout << L"Enter the full path to the game executable (e.g. ...\\bin\\x64\\ed8_3_PC.exe):\n> ";
            std::wcout.flush();
            if (!std::getline(std::wcin, path)) {
                return L"";
            }
        }
        // Trim whitespace and surrounding quotes (drag-and-drop / copy-as-path)
        const wchar_t* ws = L" \t\r\n\"";
        const size_t first = path.find_first_not_of(ws);
        const size_t last = path.find_last_not_of(ws);
        path = (first == std::wstring::npos) ? L"" : path.substr(first, last - first + 1);

        const DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return path;
        }
        std::wcout << L"[!] File not found. Please try again.\n";
        path.clear();
    }
}

// Find the PID of a running process whose executable is exactly the user-supplied path
static DWORD FindProcessByPath(const std::wstring& fullPath) {
    const size_t slash = fullPath.find_last_of(L"\\/");
    const std::wstring fileName = (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD found = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, fileName.c_str()) != 0) {
                continue;
            }
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) {
                continue;
            }
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
            if (QueryFullProcessImageNameW(h, 0, buf, &len) && _wcsicmp(buf, fullPath.c_str()) == 0) {
                found = pe.th32ProcessID;
            }
            CloseHandle(h);
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace ColdSteel3

int main(int argc, char* argv[]) {
    using namespace ColdSteel3;

    // Check command-line flags
    std::string firstArg;
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--version" || arg == "-v") {
            PrintVersion();
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            PrintHelp();
            return 0;
        }
        if (arg == "--test") {
            std::wstring gamePath;
            if (argc > 2) {
                const int len = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, nullptr, 0);
                if (len > 1) {
                    gamePath.resize(static_cast<size_t>(len));
                    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, gamePath.data(), len);
                    gamePath.resize(static_cast<size_t>(len - 1));
                }
            }
            return RunHeadlessSelfTest(gamePath);
        }
        firstArg = arg;
    }

    // Ask the user for the game executable path (no automatic system search)
    std::wstring initialPath;
    if (!firstArg.empty()) {
        const int len = MultiByteToWideChar(CP_ACP, 0, firstArg.c_str(), -1, nullptr, 0);
        if (len > 1) {
            initialPath.resize(static_cast<size_t>(len));
            MultiByteToWideChar(CP_ACP, 0, firstArg.c_str(), -1, initialPath.data(), len);
            initialPath.resize(static_cast<size_t>(len - 1));
        }
    }
    const std::wstring gameExePath = PromptForGamePath(initialPath);
    if (gameExePath.empty()) {
        return 1;
    }

    // Set console title and enable virtual terminal
    SetConsoleTitleW(L"Cold Steel III Camera Tweak Utility (v1.0.7 x64)");
    EnableVirtualTerminal();

    Logger::Instance().Log("STARTUP", "Cold Steel III Camera Tweak Utility started (silent background logging active)");

    // Clear console screen at launch
    std::cout << "\033[2J\033[H";
    std::cout.flush();

    // Register Win32 console control signal handler
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::cerr << "[!] Warning: Failed to register console control handler.\n";
    }

    CameraPatcher patcher;
    {
        std::lock_guard<std::mutex> lock(g_signalMutex);
        g_activePatcher = &patcher;
    }

    HotkeyManager hotkeyMgr;
    hotkeyMgr.BindPatcher(&patcher);

    hotkeyMgr.SetOnExit([&]() {
        Logger::Instance().Log("HOTKEY", "Exit hotkey pressed -> shutting down");
        g_running.store(false);
    });

    DWORD attachedPid = 0;
    uintptr_t attachedBase = 0;

    hotkeyMgr.SetOnStateChanged([&]() {
        RenderHUD(patcher, !patcher.IsAttached(), attachedPid, attachedBase);
    });

    // Initial HUD render
    RenderHUD(patcher, true, 0, 0);

    // Auto-hook and monitoring loop
    while (g_running.load()) {
        if (!patcher.IsAttached()) {
            DWORD pid = FindProcessByPath(gameExePath);
            if (pid != 0) {
                if (patcher.Attach(pid)) {
                    attachedPid = pid;
                    attachedBase = patcher.GetModuleBase();
                    hotkeyMgr.Start();
                    RenderHUD(patcher, false, attachedPid, attachedBase);
                } else {
                    Sleep(1000);
                }
            } else {
                Sleep(1000);
            }
        } else {
            // Process attached: monitor liveness
            if (!patcher.IsProcessAlive()) {
                // Target process terminated
                hotkeyMgr.Stop();
                patcher.Detach();
                attachedPid = 0;
                attachedBase = 0;
                RenderHUD(patcher, true, 0, 0);
            }
            Sleep(250);
        }
    }

    // Clean exit
    hotkeyMgr.Stop();
    {
        std::lock_guard<std::mutex> lock(g_signalMutex);
        patcher.Detach();
        g_activePatcher = nullptr;
    }

    std::cout << "\n\033[1;32m[OK] Cold Steel III Camera Tweak utility detached cleanly. Goodbye!\033[0m\n";
    return 0;
}
