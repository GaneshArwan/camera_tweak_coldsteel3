#include "camera_patcher.hpp"
#include "logger.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace ColdSteel3 {

CameraPatcher::CameraPatcher() {
    InitializePatchRecords();
    InitializeSubPatches();
}

CameraPatcher::~CameraPatcher() {
    Detach();
}

void CameraPatcher::InitializePatchRecords() {
    m_patches.resize(PATCH_COUNT);

    // 1. Pitch Clamp (16 bytes: minss [rbx+0x124] + maxss [rbx+0x120])
    m_patches[PATCH_PITCH].name = "Pitch Clamp (Field)";
    m_patches[PATCH_PITCH].address = m_pitchClampAddr;
    m_patches[PATCH_PITCH].originalBytes.assign(
        reinterpret_cast<const uint8_t*>(SIG_PITCH_CLAMP_PATTERN),
        reinterpret_cast<const uint8_t*>(SIG_PITCH_CLAMP_PATTERN) + PITCH_PATCH_LENGTH
    );
    m_patches[PATCH_PITCH].patchBytes.assign(PITCH_PATCH_LENGTH, 0x90); // 16 NOPs
    m_patches[PATCH_PITCH].isApplied = false;

    // 2. Zoom Clamp (9 bytes: minss xmm10, [r14+0x3A7C4])
    m_patches[PATCH_ZOOM].name = "Zoom Clamp (Max Distance)";
    m_patches[PATCH_ZOOM].address = m_zoomClampAddr;
    m_patches[PATCH_ZOOM].originalBytes.assign(
        reinterpret_cast<const uint8_t*>(SIG_ZOOM_CLAMP_PATTERN),
        reinterpret_cast<const uint8_t*>(SIG_ZOOM_CLAMP_PATTERN) + ZOOM_PATCH_LENGTH
    );
    m_patches[PATCH_ZOOM].patchBytes.assign(ZOOM_PATCH_LENGTH, 0x90); // 9 NOPs
    m_patches[PATCH_ZOOM].isApplied = false;
}

void CameraPatcher::InitializeSubPatches() {
    m_pitchSubPatches.clear();
    m_zoomSubPatches.clear();
    m_altitudeSubPatches.clear();

    if (m_moduleBase == 0) {
        return;
    }

    // --- Pitch 360° Vertical Camera Sub-Patches (ported from ayuanX v1.05 -> v1.07) ---
    // 1. Max Pitch Constant in .rdata (RVA 0x7FA320, 4 bytes: 0.87266463f -> 1.50000000f rad)
    // Allows camera to go straight over the character's head looking straight down.
    {
        PatchRecord rec;
        rec.name = "FieldCamera Max Pitch Constant (Overhead)";
        rec.address = m_moduleBase + RVA_AYUAN_PITCH_MAX_CONST;
        rec.originalBytes = { 0xF3, 0x66, 0x5F, 0x3F };
        rec.patchBytes    = { 0x00, 0x00, 0xC0, 0x3F };
        m_pitchSubPatches.push_back(rec);
    }

    // 2. Min Pitch Constant in .rdata Cave (RVA 0x7FA3B4, 4 bytes: 0.0f -> -75.0f deg)
    // Allows camera to go under the character's shoes looking straight up.
    {
        PatchRecord rec;
        rec.name = "FieldCamera Min Pitch Cave Constant (Under Shoes)";
        rec.address = m_moduleBase + RVA_AYUAN_CAVE_PITCH_MIN;
        rec.originalBytes = { 0x00, 0x00, 0x00, 0x00 };
        rec.patchBytes    = { 0x00, 0x00, 0x96, 0xC2 };
        m_pitchSubPatches.push_back(rec);
    }

    // 3. Redirect FieldCamera Min Pitch Read [rdi+0x7c8] -> [0x1407FA3B4] (RVA 0x214635, 4 bytes)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Min Pitch Redirect";
        rec.address = m_moduleBase + RVA_AYUAN_PITCH_MIN_REDIR;
        rec.originalBytes = { 0xA7, 0xC8, 0x07, 0x00 };
        rec.patchBytes    = { 0x25, 0x7A, 0x5D, 0x5E };
        m_pitchSubPatches.push_back(rec);
    }

    // 4. Bypass Ground/Terrain Collision Raycast (RVA 0x21287F, 1 byte: test rcx,rcx -> xor rcx,rcx)
    // Prevents the ground/floor from pushing the camera up when looking from under the character's shoes.
    {
        PatchRecord rec;
        rec.name = "FieldCamera Ground Collision Bypass";
        rec.address = m_moduleBase + RVA_AYUAN_COLLISION_BYPASS;
        rec.originalBytes = { 0x85 };
        rec.patchBytes    = { 0x31 };
        m_pitchSubPatches.push_back(rec);
    }

    // 5. Bypass Pitch Follow Clamp (RVA 0x213FD6, 1 byte: comiss xmm1,xmm0 -> comiss xmm1,xmm1)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Pitch Follow Clamp Bypass";
        rec.address = m_moduleBase + RVA_AYUAN_PITCH_FOLLOW;
        rec.originalBytes = { 0xC8 };
        rec.patchBytes    = { 0xC9 };
        m_pitchSubPatches.push_back(rec);
    }

    // 6. Expand Pitch Ratio Threshold (RVA 0x21F6D7, 2 bytes: load -0.75f instead of -0.30f)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Pitch Ratio Limit Expand";
        rec.address = m_moduleBase + RVA_AYUAN_PITCH_LIMIT1;
        rec.originalBytes = { 0xB5, 0xAC };
        rec.patchBytes    = { 0xC1, 0xAC };
        m_pitchSubPatches.push_back(rec);
    }

    // 7. Expand Negative Pitch Angle Clamp (RVA 0x21F6F2, 1 byte: load -75.0f from 0x1407FA3B4 instead of -5.0f)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Lower Pitch Clamp Expand";
        rec.address = m_moduleBase + RVA_AYUAN_PITCH_LIMIT2;
        rec.originalBytes = { 0xAA };
        rec.patchBytes    = { 0xBE };
        m_pitchSubPatches.push_back(rec);
    }

    // --- Zoom Sub-Patches (ported from ayuanX v1.05 -> v1.07) ---
    // 1. Zoom Multiplier Cave Float in .rdata (RVA 0x7FA3B8, 4 bytes: 0.0f -> 0.6981317f)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Zoom Cave Value";
        rec.address = m_moduleBase + RVA_AYUAN_CAVE_ZOOM_VAL;
        rec.originalBytes = { 0x00, 0x00, 0x00, 0x00 };
        rec.patchBytes    = { 0xC2, 0xB8, 0x32, 0x3F };
        m_zoomSubPatches.push_back(rec);
    }

    // 2. Redirect FieldCamera Zoom Read [rdi+0x824] -> [0x1407FA3B8] (RVA 0x213FE5, 4 bytes)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Zoom Redirect";
        rec.address = m_moduleBase + RVA_AYUAN_ZOOM_REDIRECT;
        rec.originalBytes = { 0x87, 0x24, 0x08, 0x00 };
        rec.patchBytes    = { 0x05, 0xCE, 0x63, 0x5E };
        m_zoomSubPatches.push_back(rec);
    }

    // --- Altitude / Camera Height Sub-Patches (ported from ayuanX v1.05 -> v1.07) ---
    // 1. Camera Height Cave Float in .rdata (RVA 0x7FA3BC, 4 bytes: 0.0f -> 1.400000f)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Altitude Cave Value";
        rec.address = m_moduleBase + RVA_AYUAN_CAVE_ALTITUDE_VAL;
        rec.originalBytes = { 0x00, 0x00, 0x00, 0x00 };
        rec.patchBytes    = { 0x33, 0x33, 0xB3, 0x3F };
        m_altitudeSubPatches.push_back(rec);
    }

    // 2. Redirect FieldCamera Height Add [rdi+0x7c0] -> [0x1407FA3BC] (RVA 0x2D108A, 4 bytes)
    {
        PatchRecord rec;
        rec.name = "FieldCamera Altitude Redirect";
        rec.address = m_moduleBase + RVA_AYUAN_ALTITUDE_REDIRECT;
        rec.originalBytes = { 0x8F, 0xC0, 0x07, 0x00 };
        rec.patchBytes    = { 0x0D, 0x2D, 0x93, 0x52 };
        m_altitudeSubPatches.push_back(rec);
    }
}

void CameraPatcher::ApplySubPatches(std::vector<PatchRecord>& subPatches, bool apply) {
    if (!m_hProcess) {
        return;
    }

    for (auto& sub : subPatches) {
        if (sub.address == 0 || sub.originalBytes.empty()) {
            continue;
        }

        std::vector<uint8_t> current(sub.originalBytes.size());
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(sub.address),
                current.data(),
                current.size(),
                &bytesRead) || bytesRead != current.size()) {
            continue;
        }

        const bool matchesOrig = (current == sub.originalBytes);
        const bool matchesPatch = (current == sub.patchBytes);

        if (apply) {
            if (!sub.isApplied) {
                // Only apply if memory matches originalBytes or patchBytes
                // (protects mock unit tests that allocate small buffers without full game code)
                if (!matchesOrig && !matchesPatch) {
                    continue;
                }
                if (WriteMemorySafe(sub.address, sub.patchBytes)) {
                    sub.isApplied = true;
                    Logger::Instance().Log("SUBPATCH", "Applied " + sub.name + " at " + Logger::Hex(sub.address));
                }
            }
        } else {
            // Always restore if sub.isApplied is true (even if a dynamic .rdata float cave was adjusted)
            if (sub.isApplied || matchesPatch) {
                if (WriteMemorySafe(sub.address, sub.originalBytes)) {
                    sub.isApplied = false;
                    Logger::Instance().Log("SUBPATCH", "Restored " + sub.name + " at " + Logger::Hex(sub.address));
                }
            }
        }
    }
}

bool CameraPatcher::IsProcessAliveInternal() const {
    if (!m_hProcess) {
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_hProcess, &exitCode)) {
        return false;
    }
    if (exitCode != STILL_ACTIVE) {
        return false;
    }
    return (WaitForSingleObject(m_hProcess, 0) != WAIT_OBJECT_0);
}

bool CameraPatcher::ResolveAddresses() {
    if (!m_hProcess || m_moduleBase == 0) {
        return false;
    }

    // 1. Resolve Pitch Clamp Address
    if (m_pitchClampAddr == 0) {
        uintptr_t scanned = (m_moduleSize > 0)
            ? ProcessScanner::ScanPattern(m_hProcess, m_moduleBase, m_moduleSize, SIG_PITCH_CLAMP)
            : 0;
        m_pitchClampAddr = (scanned != 0) ? scanned : (m_moduleBase + RVA_PITCH_CLAMP);
    }
    m_patches[PATCH_PITCH].address = m_pitchClampAddr;

    // 2. Resolve Zoom Clamp Address
    if (m_zoomClampAddr == 0) {
        uintptr_t scanned = (m_moduleSize > 0)
            ? ProcessScanner::ScanPattern(m_hProcess, m_moduleBase, m_moduleSize, SIG_ZOOM_CLAMP)
            : 0;
        m_zoomClampAddr = (scanned != 0) ? scanned : (m_moduleBase + RVA_ZOOM_CLAMP);
    }
    m_patches[PATCH_ZOOM].address = m_zoomClampAddr;

    // 3. Resolve Altitude Write Address
    if (m_altitudeWriteAddr == 0) {
        uintptr_t scanned = (m_moduleSize > 0)
            ? ProcessScanner::ScanPattern(m_hProcess, m_moduleBase, m_moduleSize, SIG_ALTITUDE_WRITE)
            : 0;
        m_altitudeWriteAddr = (scanned != 0) ? scanned : (m_moduleBase + RVA_ALTITUDE_WRITE);
    }

    // 4. Resolve GameManager Global Pointer Address
    if (m_gmGlobalPtr == 0) {
        uintptr_t scanned = (m_moduleSize > 0)
            ? ProcessScanner::ScanPattern(m_hProcess, m_moduleBase, m_moduleSize, SIG_GM_RESOLVER)
            : 0;
        if (scanned != 0) {
            // Instruction: 48 8B 05 [disp32] -> RIP = scanned + 7
            int32_t disp32 = 0;
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(
                    m_hProcess,
                    reinterpret_cast<LPCVOID>(scanned + 3),
                    &disp32,
                    sizeof(disp32),
                    &bytesRead) && bytesRead == sizeof(disp32)) {
                m_gmGlobalPtr = scanned + 7 + disp32;
            }
        }
        if (m_gmGlobalPtr == 0) {
            m_gmGlobalPtr = m_moduleBase + RVA_GM_GLOBAL_PTR;
        }
    }

    InitializeSubPatches();

    Logger::Instance().Log(
        "RESOLVE",
        "ModuleBase=" + Logger::Hex(m_moduleBase) +
        " PitchClamp=" + Logger::Hex(m_pitchClampAddr) +
        " ZoomClamp=" + Logger::Hex(m_zoomClampAddr) +
        " GMGlobalPtr=" + Logger::Hex(m_gmGlobalPtr)
    );

    return true;
}

bool CameraPatcher::ResolveMapCameraInternal(uintptr_t& outGameManager, uintptr_t& outMapCamera) const {
    outGameManager = 0;
    outMapCamera = 0;
    if (!m_hProcess || m_gmGlobalPtr == 0) {
        return false;
    }

    uintptr_t gmRoot = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            m_hProcess,
            reinterpret_cast<LPCVOID>(m_gmGlobalPtr),
            &gmRoot,
            sizeof(gmRoot),
            &bytesRead) || bytesRead != sizeof(gmRoot) || gmRoot == 0) {
        return false;
    }

    if (!ReadProcessMemory(
            m_hProcess,
            reinterpret_cast<LPCVOID>(gmRoot + OFFSET_GM_POINTER_TO_INSTANCE),
            &outGameManager,
            sizeof(outGameManager),
            &bytesRead) || bytesRead != sizeof(outGameManager) || outGameManager == 0) {
        return false;
    }

    uintptr_t mapMgr = 0;
    if (ReadProcessMemory(
            m_hProcess,
            reinterpret_cast<LPCVOID>(outGameManager + OFFSET_GM_TO_MAP_MANAGER),
            &mapMgr,
            sizeof(mapMgr),
            &bytesRead) && bytesRead == sizeof(mapMgr) && mapMgr != 0) {
        uintptr_t mapCam = 0;
        if (ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(mapMgr + OFFSET_MAP_MANAGER_TO_MAP_CAMERA),
                &mapCam,
                sizeof(mapCam),
                &bytesRead) && bytesRead == sizeof(mapCam) && mapCam != 0) {
            outMapCamera = mapCam;
        }
    }

    return true;
}

bool CameraPatcher::Attach(DWORD pid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isAttached) {
        DetachInternal();
    }

    if (pid == 0) {
        return false;
    }

    m_hProcess = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE,
        pid
    );
    if (!m_hProcess || m_hProcess == INVALID_HANDLE_VALUE) {
        m_hProcess = nullptr;
        Logger::Instance().Log("ATTACH", "OpenProcess failed for PID=" + std::to_string(pid) + " err=" + std::to_string(GetLastError()));
        return false;
    }
    m_ownsHandle = true;
    m_pid = pid;

    if (!IsProcessAliveInternal()) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
        m_ownsHandle = false;
        return false;
    }

    BOOL isWow64 = FALSE;
    if (IsWow64Process(m_hProcess, &isWow64) && isWow64) {
        // Target is 32-bit under emulation, but ed8_3_PC.exe is native 64-bit
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
        m_ownsHandle = false;
        return false;
    }

    m_moduleBase = ProcessScanner::GetModuleBase(pid, DEFAULT_MODULE_NAME);
    if (m_moduleBase == 0) {
        m_moduleBase = ProcessScanner::GetModuleBase(pid, L"");
    }
    if (m_moduleBase == 0) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
        m_ownsHandle = false;
        return false;
    }

    m_moduleSize = ProcessScanner::GetModuleSize(pid, DEFAULT_MODULE_NAME);
    if (m_moduleSize == 0) {
        m_moduleSize = ProcessScanner::GetModuleSize(pid, L"");
    }

    InitializePatchRecords();
    ResolveAddresses();

    m_isAttached = true;
    m_cumulativeAltitudeDelta = 0.0f;
    m_appliedOrder.clear();
    m_rollbackHistory.clear();

    Logger::Instance().Log("ATTACH", "Attached to PID=" + std::to_string(pid) + " Base=" + Logger::Hex(m_moduleBase) + " Size=" + Logger::Hex(m_moduleSize));
    return true;
}

bool CameraPatcher::Attach(HANDLE hExistingProcess, uintptr_t moduleBase) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isAttached) {
        DetachInternal();
    }

    if (!hExistingProcess) {
        return false;
    }

    m_hProcess = hExistingProcess;
    m_ownsHandle = false;
    m_pid = ::GetProcessId(hExistingProcess);
    m_moduleBase = (moduleBase != 0) ? moduleBase : DEFAULT_IMAGE_BASE;
    m_moduleSize = 0x2000000;

    if (!IsProcessAliveInternal()) {
        m_hProcess = nullptr;
        return false;
    }

    InitializePatchRecords();
    ResolveAddresses();

    m_isAttached = true;
    m_cumulativeAltitudeDelta = 0.0f;
    m_appliedOrder.clear();
    m_rollbackHistory.clear();

    Logger::Instance().Log("ATTACH", "Attached (existing handle) PID=" + std::to_string(m_pid) + " Base=" + Logger::Hex(m_moduleBase));
    return true;
}

void CameraPatcher::Detach() {
    std::lock_guard<std::mutex> lock(m_mutex);
    DetachInternal();
}

void CameraPatcher::DetachInternal() {
    if (!m_isAttached && !m_hProcess) {
        return;
    }

    const bool alive = IsProcessAliveInternal();
    if (alive && m_hProcess) {
        // Restore sub-patches
        ApplySubPatches(m_pitchSubPatches, false);
        ApplySubPatches(m_zoomSubPatches, false);
        ApplySubPatches(m_altitudeSubPatches, false);

        // Restore all active primary patches in strict reverse LIFO order
        for (auto it = m_appliedOrder.rbegin(); it != m_appliedOrder.rend(); ++it) {
            const size_t idx = *it;
            if (idx < m_patches.size() && m_patches[idx].isApplied) {
                WriteMemorySafe(m_patches[idx].address, m_patches[idx].originalBytes);
                m_patches[idx].isApplied = false;
                m_rollbackHistory.push_back(m_patches[idx].name);
            }
        }

        // Revert cumulative altitude offset if applied
        if (m_cumulativeAltitudeDelta != 0.0f) {
            uintptr_t gameManager = 0;
            uintptr_t mapCam = 0;
            SIZE_T bytesRead = 0;
            if (ResolveMapCameraInternal(gameManager, mapCam)) {
                if (mapCam != 0) {
                    float zero = 0.0f;
                    SIZE_T bytesWritten = 0;
                    WriteProcessMemory(
                        m_hProcess,
                        reinterpret_cast<LPVOID>(mapCam + OFFSET_MAP_CAMERA_PAN_Y),
                        &zero,
                        sizeof(zero),
                        &bytesWritten
                    );
                } else if (gameManager != 0) {
                    uintptr_t activeCam = 0;
                    if (ReadProcessMemory(
                            m_hProcess,
                            reinterpret_cast<LPCVOID>(gameManager + OFFSET_GM_TO_ACTIVE_CAMERA),
                            &activeCam,
                            sizeof(activeCam),
                            &bytesRead) && bytesRead == sizeof(activeCam) && activeCam != 0) {
                        const uintptr_t eyeYAddr = activeCam + OFFSET_CAMERA_EYE_Y;
                        float currentY = 0.0f;
                        if (ReadProcessMemory(
                                m_hProcess,
                                reinterpret_cast<LPCVOID>(eyeYAddr),
                                &currentY,
                                sizeof(currentY),
                                &bytesRead) && bytesRead == sizeof(currentY)) {
                            float restoredY = currentY - m_cumulativeAltitudeDelta;
                            SIZE_T bytesWritten = 0;
                            WriteProcessMemory(
                                m_hProcess,
                                reinterpret_cast<LPVOID>(eyeYAddr),
                                &restoredY,
                                sizeof(restoredY),
                                &bytesWritten
                            );
                        }
                    }
                }
            }
            m_cumulativeAltitudeDelta = 0.0f;
        }

        FlushInstructionCache(m_hProcess, nullptr, 0);
    } else {
        // Dead PID: do not attempt to write to defunct process memory
        for (auto& patch : m_patches) {
            patch.isApplied = false;
        }
        for (auto& sub : m_pitchSubPatches) sub.isApplied = false;
        for (auto& sub : m_zoomSubPatches) sub.isApplied = false;
        for (auto& sub : m_altitudeSubPatches) sub.isApplied = false;
    }

    m_appliedOrder.clear();

    if (m_ownsHandle && m_hProcess && m_hProcess != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hProcess);
    }

    Logger::Instance().Log("DETACH", "Detached from PID=" + std::to_string(m_pid) + " (alive=" + (alive ? "true" : "false") + ")");

    m_hProcess = nullptr;
    m_ownsHandle = false;
    m_isAttached = false;
    m_pid = 0;
}

bool CameraPatcher::WriteMemorySafe(uintptr_t address, const std::vector<uint8_t>& bytes) {
    if (!m_hProcess || address == 0 || bytes.empty()) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtectEx(
            m_hProcess,
            reinterpret_cast<LPVOID>(address),
            bytes.size(),
            PAGE_EXECUTE_READWRITE,
            &oldProtect)) {
        return false;
    }

    SIZE_T bytesWritten = 0;
    BOOL writeOk = WriteProcessMemory(
        m_hProcess,
        reinterpret_cast<LPVOID>(address),
        bytes.data(),
        bytes.size(),
        &bytesWritten
    );

    DWORD dummy = 0;
    VirtualProtectEx(
        m_hProcess,
        reinterpret_cast<LPVOID>(address),
        bytes.size(),
        oldProtect,
        &dummy
    );

    if (writeOk && bytesWritten == bytes.size()) {
        FlushInstructionCache(m_hProcess, reinterpret_cast<LPCVOID>(address), bytes.size());
        return true;
    }

    return false;
}

bool CameraPatcher::SetPitchUnlocked(bool unlock) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess) {
        return false;
    }
    if (!IsProcessAliveInternal()) {
        return false;
    }

    PatchRecord& rec = m_patches[PATCH_PITCH];
    if (rec.address == 0) {
        return false;
    }

    if (unlock) {
        // Idempotent: already unlocked
        if (rec.isApplied) {
            return true;
        }

        // Backup current bytes before first patch if not set
        std::vector<uint8_t> current(PITCH_PATCH_LENGTH);
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(rec.address),
                current.data(),
                PITCH_PATCH_LENGTH,
                &bytesRead) && bytesRead == PITCH_PATCH_LENGTH) {
            
            bool isAllNops = true;
            for (uint8_t b : current) {
                if (b != 0x90) {
                    isAllNops = false;
                    break;
                }
            }
            if (!isAllNops) {
                rec.originalBytes = current;
            }
        }

        if (WriteMemorySafe(rec.address, rec.patchBytes)) {
            rec.isApplied = true;
            m_appliedOrder.push_back(PATCH_PITCH);
            ApplySubPatches(m_pitchSubPatches, true);
            Logger::Instance().Log("PITCH", "Pitch UNLOCKED (360 vertical tilt + raycast collision bypass enabled) at " + Logger::Hex(rec.address));
            return true;
        }
        Logger::Instance().Log("PITCH", "Failed to unlock pitch at " + Logger::Hex(rec.address));
        return false;
    } else {
        // Idempotent: already locked / default
        if (!rec.isApplied) {
            return true;
        }

        ApplySubPatches(m_pitchSubPatches, false);
        if (WriteMemorySafe(rec.address, rec.originalBytes)) {
            rec.isApplied = false;
            auto it = std::find(m_appliedOrder.begin(), m_appliedOrder.end(), PATCH_PITCH);
            if (it != m_appliedOrder.end()) {
                m_appliedOrder.erase(it);
            }
            m_rollbackHistory.push_back(rec.name);
            Logger::Instance().Log("PITCH", "Pitch LOCKED (restored defaults) at " + Logger::Hex(rec.address));
            return true;
        }
        return false;
    }
}

bool CameraPatcher::SetZoomUnlocked(bool unlock) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess) {
        return false;
    }
    if (!IsProcessAliveInternal()) {
        return false;
    }

    PatchRecord& rec = m_patches[PATCH_ZOOM];
    if (rec.address == 0) {
        return false;
    }

    if (unlock) {
        // Idempotent: already unlocked
        if (rec.isApplied) {
            return true;
        }

        std::vector<uint8_t> current(ZOOM_PATCH_LENGTH);
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(rec.address),
                current.data(),
                ZOOM_PATCH_LENGTH,
                &bytesRead) && bytesRead == ZOOM_PATCH_LENGTH) {
            
            bool isAllNops = true;
            for (uint8_t b : current) {
                if (b != 0x90) {
                    isAllNops = false;
                    break;
                }
            }
            if (!isAllNops) {
                rec.originalBytes = current;
            }
        }

        if (WriteMemorySafe(rec.address, rec.patchBytes)) {
            rec.isApplied = true;
            m_appliedOrder.push_back(PATCH_ZOOM);
            ApplySubPatches(m_zoomSubPatches, true);
            Logger::Instance().Log("ZOOM", "Zoom UNLOCKED (field & world distance limits bypassed) at " + Logger::Hex(rec.address));
            return true;
        }
        Logger::Instance().Log("ZOOM", "Failed to unlock zoom at " + Logger::Hex(rec.address));
        return false;
    } else {
        // Idempotent: already locked / default
        if (!rec.isApplied) {
            return true;
        }

        ApplySubPatches(m_zoomSubPatches, false);
        if (WriteMemorySafe(rec.address, rec.originalBytes)) {
            rec.isApplied = false;
            auto it = std::find(m_appliedOrder.begin(), m_appliedOrder.end(), PATCH_ZOOM);
            if (it != m_appliedOrder.end()) {
                m_appliedOrder.erase(it);
            }
            m_rollbackHistory.push_back(rec.name);
            Logger::Instance().Log("ZOOM", "Zoom LOCKED (restored defaults) at " + Logger::Hex(rec.address));
            return true;
        }
        return false;
    }
}

bool CameraPatcher::AdjustAltitude(float delta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess) {
        return false;
    }
    if (!IsProcessAliveInternal()) {
        return false;
    }

    SIZE_T bytesRead = 0;
    SIZE_T bytesWritten = 0;

    // 1. Live Game Path (ayuanX v1.0.7 FieldCamera Altitude Redirect to .rdata 0x1407FA3BC)
    ApplySubPatches(m_altitudeSubPatches, true);
    if (!m_altitudeSubPatches.empty() && m_altitudeSubPatches.back().isApplied) {
        const uintptr_t altCaveAddr = m_moduleBase + RVA_AYUAN_CAVE_ALTITUDE_VAL;
        float currentAlt = DEFAULT_AYUAN_ALTITUDE_VAL;
        if (!ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(altCaveAddr),
                &currentAlt,
                sizeof(currentAlt),
                &bytesRead) || bytesRead != sizeof(currentAlt)) {
            Logger::Instance().Log("ALTITUDE", "Failed reading FieldCamera altitude cave at " + Logger::Hex(altCaveAddr));
            return false;
        }

        // Scale 60Hz tick delta (0.25f default) by 0.12f -> ~0.03m per 16ms tick (~1.8m/sec normal, ~5.4m/sec Shift)
        const float stepAlt = delta * 0.12f;
        const float newAlt = std::clamp(currentAlt + stepAlt, -50.0f, 50.0f);
        std::vector<uint8_t> floatBytes(sizeof(float));
        std::memcpy(floatBytes.data(), &newAlt, sizeof(float));
        if (!WriteMemorySafe(altCaveAddr, floatBytes)) {
            Logger::Instance().Log("ALTITUDE", "Failed writing FieldCamera altitude cave at " + Logger::Hex(altCaveAddr));
            return false;
        }

        m_cumulativeAltitudeDelta = newAlt - DEFAULT_AYUAN_ALTITUDE_VAL;
        Logger::Instance().Log(
            "ALTITUDE",
            "Adjusted FieldCamera Height (" + Logger::Hex(altCaveAddr) + "): " +
            Logger::Float(currentAlt) + " -> " + Logger::Float(newAlt) +
            " (step=" + Logger::Float(stepAlt) + ", deltaFromDefault=" + Logger::Float(m_cumulativeAltitudeDelta) + ")"
        );
        return true;
    }

    // 2. Fallback Path (Mock Unit Test Harnesses where +0x39F48 is populated):
    uintptr_t gameManager = 0;
    uintptr_t mapCam = 0;
    if (!ResolveMapCameraInternal(gameManager, mapCam)) {
        Logger::Instance().Log("ALTITUDE", "Failed to resolve GameManager from " + Logger::Hex(m_gmGlobalPtr));
        return false;
    }

    uintptr_t activeCam = 0;
    if (!ReadProcessMemory(
            m_hProcess,
            reinterpret_cast<LPCVOID>(gameManager + OFFSET_GM_TO_ACTIVE_CAMERA),
            &activeCam,
            sizeof(activeCam),
            &bytesRead) || bytesRead != sizeof(activeCam) || activeCam == 0) {
        Logger::Instance().Log("ALTITUDE", "Fallback +0x39F48 is null");
        return false;
    }

    const uintptr_t eyeYAddr = activeCam + OFFSET_CAMERA_EYE_Y;
    float currentY = 0.0f;
    if (!ReadProcessMemory(
            m_hProcess,
            reinterpret_cast<LPCVOID>(eyeYAddr),
            &currentY,
            sizeof(currentY),
            &bytesRead) || bytesRead != sizeof(currentY)) {
        return false;
    }

    float newY = currentY + delta;
    if (!WriteProcessMemory(
            m_hProcess,
            reinterpret_cast<LPVOID>(eyeYAddr),
            &newY,
            sizeof(newY),
            &bytesWritten) || bytesWritten != sizeof(newY)) {
        return false;
    }

    m_cumulativeAltitudeDelta += delta;
    return true;
}

bool CameraPatcher::AdjustZoom(float delta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess || !IsProcessAliveInternal()) {
        return false;
    }

    // Auto-enable zoom sub-patches (ayuanX v1.0.7 FieldCamera Zoom Redirect to .rdata 0x1407FA3B8)
    ApplySubPatches(m_zoomSubPatches, true);
    if (!m_zoomSubPatches.empty() && m_zoomSubPatches.back().isApplied) {
        // Also mark primary zoom patch as applied if not yet unlocked
        PatchRecord& zoomRec = m_patches[PATCH_ZOOM];
        if (!zoomRec.isApplied && zoomRec.address != 0) {
            if (WriteMemorySafe(zoomRec.address, zoomRec.patchBytes)) {
                zoomRec.isApplied = true;
                m_appliedOrder.push_back(PATCH_ZOOM);
            }
        }

        const uintptr_t zoomCaveAddr = m_moduleBase + RVA_AYUAN_CAVE_ZOOM_VAL;
        float currentZoom = DEFAULT_AYUAN_ZOOM_VAL;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                m_hProcess,
                reinterpret_cast<LPCVOID>(zoomCaveAddr),
                &currentZoom,
                sizeof(currentZoom),
                &bytesRead) || bytesRead != sizeof(currentZoom)) {
            Logger::Instance().Log("ZOOM_ADJ", "Failed reading FieldCamera zoom cave at " + Logger::Hex(zoomCaveAddr));
            return false;
        }

        // Scale 60Hz tick delta (0.50f default) by 0.03f -> 0.015f per 16ms tick (~0.90f/sec normal)
        const float stepZoom = delta * 0.03f;
        const float newZoom = std::clamp(currentZoom + stepZoom, 0.05f, 2.50f);
        std::vector<uint8_t> floatBytes(sizeof(float));
        std::memcpy(floatBytes.data(), &newZoom, sizeof(float));
        if (!WriteMemorySafe(zoomCaveAddr, floatBytes)) {
            Logger::Instance().Log("ZOOM_ADJ", "Failed writing FieldCamera zoom cave at " + Logger::Hex(zoomCaveAddr));
            return false;
        }

        Logger::Instance().Log(
            "ZOOM_ADJ",
            "Adjusted FieldCamera Zoom (" + Logger::Hex(zoomCaveAddr) + "): " +
            Logger::Float(currentZoom) + " -> " + Logger::Float(newZoom)
        );
        return true;
    }

    Logger::Instance().Log("ZOOM_ADJ", "FieldCamera zoom cave not available in target process");
    return false;
}

bool CameraPatcher::AdjustPitch(float delta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess || !IsProcessAliveInternal()) {
        return false;
    }

    // Ensure 360° pitch unlock is enabled so mouse / right-stick can freely tilt overhead and under shoes
    ApplySubPatches(m_pitchSubPatches, true);
    PatchRecord& pitchRec = m_patches[PATCH_PITCH];
    if (!pitchRec.isApplied && pitchRec.address != 0) {
        if (WriteMemorySafe(pitchRec.address, pitchRec.patchBytes)) {
            pitchRec.isApplied = true;
            m_appliedOrder.push_back(PATCH_PITCH);
        }
    }

    Logger::Instance().Log("PITCH_ADJ", "Pitch 360 unlock active (delta=" + Logger::Float(delta) + "); use mouse or right-stick to tilt camera 360 degrees");
    return true;
}

bool CameraPatcher::ResetDefaults() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess) {
        return false;
    }
    if (!IsProcessAliveInternal()) {
        return false;
    }

    // 1. Restore pitch clamp & sub-patches
    ApplySubPatches(m_pitchSubPatches, false);
    PatchRecord& pitchRec = m_patches[PATCH_PITCH];
    if (pitchRec.isApplied) {
        WriteMemorySafe(pitchRec.address, pitchRec.originalBytes);
        pitchRec.isApplied = false;
        m_rollbackHistory.push_back(pitchRec.name);
    }

    // 2. Restore zoom clamp & sub-patches
    ApplySubPatches(m_zoomSubPatches, false);
    PatchRecord& zoomRec = m_patches[PATCH_ZOOM];
    if (zoomRec.isApplied) {
        WriteMemorySafe(zoomRec.address, zoomRec.originalBytes);
        zoomRec.isApplied = false;
        m_rollbackHistory.push_back(zoomRec.name);
    }
    m_appliedOrder.clear();

    // 3. Reset altitude offset & sub-patches
    ApplySubPatches(m_altitudeSubPatches, false);
    if (m_cumulativeAltitudeDelta != 0.0f) {
        uintptr_t gameManager = 0;
        uintptr_t mapCam = 0;
        SIZE_T bytesRead = 0;
        if (ResolveMapCameraInternal(gameManager, mapCam)) {
            if (mapCam != 0) {
                float zero = 0.0f;
                SIZE_T bytesWritten = 0;
                WriteProcessMemory(
                    m_hProcess,
                    reinterpret_cast<LPVOID>(mapCam + OFFSET_MAP_CAMERA_PAN_Y),
                    &zero,
                    sizeof(zero),
                    &bytesWritten
                );
            } else if (gameManager != 0) {
                uintptr_t activeCam = 0;
                if (ReadProcessMemory(
                        m_hProcess,
                        reinterpret_cast<LPCVOID>(gameManager + OFFSET_GM_TO_ACTIVE_CAMERA),
                        &activeCam,
                        sizeof(activeCam),
                        &bytesRead) && bytesRead == sizeof(activeCam) && activeCam != 0) {
                    const uintptr_t eyeYAddr = activeCam + OFFSET_CAMERA_EYE_Y;
                    float currentY = 0.0f;
                    if (ReadProcessMemory(
                            m_hProcess,
                            reinterpret_cast<LPCVOID>(eyeYAddr),
                            &currentY,
                            sizeof(currentY),
                            &bytesRead) && bytesRead == sizeof(currentY)) {
                        float restoredY = currentY - m_cumulativeAltitudeDelta;
                        SIZE_T bytesWritten = 0;
                        WriteProcessMemory(
                            m_hProcess,
                            reinterpret_cast<LPVOID>(eyeYAddr),
                            &restoredY,
                            sizeof(restoredY),
                            &bytesWritten
                        );
                    }
                }
            }
        }
        m_cumulativeAltitudeDelta = 0.0f;
    }

    FlushInstructionCache(m_hProcess, nullptr, 0);
    Logger::Instance().Log("RESET", "Restored all camera settings and patches to game defaults");
    return true;
}

bool CameraPatcher::IsProcessAlive() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isAttached || !m_hProcess) {
        return false;
    }
    return IsProcessAliveInternal();
}

bool CameraPatcher::IsAttached() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_isAttached && (m_hProcess != nullptr);
}

bool CameraPatcher::IsPitchUnlocked() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_patches.size() > PATCH_PITCH) && m_patches[PATCH_PITCH].isApplied;
}

bool CameraPatcher::IsZoomUnlocked() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_patches.size() > PATCH_ZOOM) && m_patches[PATCH_ZOOM].isApplied;
}

float CameraPatcher::GetAltitudeDelta() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cumulativeAltitudeDelta;
}

float CameraPatcher::GetCumulativeAltitudeDelta() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cumulativeAltitudeDelta;
}

DWORD CameraPatcher::GetProcessId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pid;
}

HANDLE CameraPatcher::GetProcessHandle() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hProcess;
}

uintptr_t CameraPatcher::GetModuleBase() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_moduleBase;
}

size_t CameraPatcher::GetModuleSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_moduleSize;
}

uintptr_t CameraPatcher::GetPitchClampAddress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pitchClampAddr;
}

uintptr_t CameraPatcher::GetZoomClampAddress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_zoomClampAddr;
}

uintptr_t CameraPatcher::GetGameManagerGlobalAddress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gmGlobalPtr;
}

const std::vector<PatchRecord>& CameraPatcher::GetPatchRecords() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_patches;
}

const std::vector<std::string>& CameraPatcher::GetRollbackHistory() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rollbackHistory;
}

void CameraPatcher::SetCustomAddresses(
    uintptr_t pitchClampAddr,
    uintptr_t zoomClampAddr,
    uintptr_t gmGlobalPtr
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (pitchClampAddr != 0) {
        m_pitchClampAddr = pitchClampAddr;
        if (m_patches.size() > PATCH_PITCH) {
            m_patches[PATCH_PITCH].address = pitchClampAddr;
        }
    }
    if (zoomClampAddr != 0) {
        m_zoomClampAddr = zoomClampAddr;
        if (m_patches.size() > PATCH_ZOOM) {
            m_patches[PATCH_ZOOM].address = zoomClampAddr;
        }
    }
    if (gmGlobalPtr != 0) {
        m_gmGlobalPtr = gmGlobalPtr;
    } else if (pitchClampAddr == 0 && zoomClampAddr == 0) {
        m_gmGlobalPtr = 0;
    }
}

} // namespace ColdSteel3
