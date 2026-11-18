#pragma once

#include "common.hpp"
#include "scanner.hpp"
#include <windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <functional>

namespace ColdSteel3 {

// Camera and Game Structure Offsets (v1.0.7 x64)
inline constexpr uintptr_t DEFAULT_GM_GLOBAL_PTR_VA = 0x140C53290ULL;
inline constexpr size_t OFFSET_GM_POINTER_TO_INSTANCE = 0x1CE8;
inline constexpr size_t OFFSET_GM_TO_ACTIVE_CAMERA = 0x39F48;
inline constexpr size_t OFFSET_GM_TO_MAP_MANAGER = 0x5A778;
inline constexpr size_t OFFSET_MAP_MANAGER_TO_MAP_CAMERA = 0x38;
inline constexpr size_t OFFSET_CAMERA_EYE_X = 0x364;
inline constexpr size_t OFFSET_CAMERA_EYE_Y = 0x368;
inline constexpr size_t OFFSET_CAMERA_EYE_Z = 0x36C;
inline constexpr size_t OFFSET_CAMERA_ZOOM = 0x65C;
inline constexpr size_t OFFSET_CONFIG_MIN_ZOOM = 0x3A7C0;
inline constexpr size_t OFFSET_CONFIG_MAX_ZOOM_NORMAL = 0x3A7C4;
inline constexpr size_t OFFSET_CONFIG_MAX_ZOOM_FIELD = 0x3A7C8;

// MapCamera Structure Offsets (size 0x1E0, ctor 0x1401FBCC0)
inline constexpr size_t OFFSET_MAP_CAMERA_EYE_Y = 0x1C;
inline constexpr size_t OFFSET_MAP_CAMERA_TARGET_Y = 0x2C;
inline constexpr size_t OFFSET_MAP_CAMERA_PITCH = 0x38;
inline constexpr size_t OFFSET_MAP_CAMERA_YAW = 0x3C;
inline constexpr size_t OFFSET_MAP_CAMERA_DISTANCE = 0x48;
inline constexpr size_t OFFSET_MAP_CAMERA_PREV_EYE_Y = 0x60;
inline constexpr size_t OFFSET_MAP_CAMERA_PREV_TARGET_Y = 0x70;
inline constexpr size_t OFFSET_MAP_CAMERA_PAN_HOLD = 0x97;
inline constexpr size_t OFFSET_MAP_CAMERA_PAN_X = 0x9C;
inline constexpr size_t OFFSET_MAP_CAMERA_PAN_Y = 0xA0;
inline constexpr size_t OFFSET_MAP_CAMERA_PAN_Z = 0xA4;
inline constexpr size_t OFFSET_MAP_CAMERA_START_TARGET_Y = 0xB0;
inline constexpr size_t OFFSET_MAP_CAMERA_END_TARGET_Y = 0xC0;
inline constexpr size_t OFFSET_MAP_CAMERA_START_PITCH = 0xCC;
inline constexpr size_t OFFSET_MAP_CAMERA_END_PITCH = 0xDC;
inline constexpr size_t OFFSET_MAP_CAMERA_START_DISTANCE = 0xEC;
inline constexpr size_t OFFSET_MAP_CAMERA_END_DISTANCE = 0xF0;
inline constexpr size_t OFFSET_MAP_CAMERA_MIN_PITCH = 0x120;
inline constexpr size_t OFFSET_MAP_CAMERA_MAX_PITCH = 0x124;
inline constexpr size_t OFFSET_MAP_CAMERA_DEFAULT_PITCH = 0x128;

// Patch sizes and lengths
inline constexpr size_t PITCH_PATCH_LENGTH = 16;
inline constexpr size_t ZOOM_PATCH_LENGTH = 9;
inline constexpr size_t PITCH_INDOOR_PATCH_LENGTH = 20;
inline constexpr size_t CAMERA_RAYCAST_PATCH_LENGTH = 6;
inline constexpr size_t CAMERA_ASIN_PATCH_LENGTH = 13;
inline constexpr size_t PITCH_SHRINK_PATCH_LENGTH = 2;
inline constexpr size_t MAP_ZOOM_CLAMP_PATCH_LENGTH = 10;
inline constexpr size_t SET_ZOOM_CLAMP_PATCH_LENGTH = 8;

// Relative Virtual Addresses (RVAs) from Default Image Base (0x140000000)
inline constexpr uintptr_t RVA_PITCH_CLAMP = 0x1FCD2A;
inline constexpr uintptr_t RVA_ZOOM_CLAMP = 0x2DE35C;
inline constexpr uintptr_t RVA_ALTITUDE_WRITE = 0x2DE3BB;
inline constexpr uintptr_t RVA_GM_RESOLVER = 0x2A7D00;
inline constexpr uintptr_t RVA_GM_GLOBAL_PTR = 0xC53290;

// Verified ayuanX FieldCamera v1.0.7 RVAs (ported from v1.05 CETRAINER & verified on v1.07 disk binary)
inline constexpr uintptr_t RVA_AYUAN_PITCH_MAX_CONST   = 0x7FA320; // .rdata float: 0.87266463f -> 1.50000000f (~86 deg overhead)
inline constexpr uintptr_t RVA_AYUAN_CAVE_PITCH_MIN    = 0x7FA3B4; // .rdata cave float: -75.0f deg (under character's shoes)
inline constexpr uintptr_t RVA_AYUAN_CAVE_ZOOM_VAL     = 0x7FA3B8; // .rdata cave float: dynamic zoom multiplier (default 0.6981317f)
inline constexpr uintptr_t RVA_AYUAN_CAVE_ALTITUDE_VAL = 0x7FA3BC; // .rdata cave float: dynamic camera height (default 1.400000f)

inline constexpr uintptr_t RVA_AYUAN_COLLISION_BYPASS  = 0x21287F; // .text 1B: 85 -> 31 (xor rcx,rcx to skip ground collision raycast)
inline constexpr uintptr_t RVA_AYUAN_PITCH_FOLLOW      = 0x213FD6; // .text 1B: C8 -> C9 (comiss xmm1,xmm1 to bypass follow pitch clamp)
inline constexpr uintptr_t RVA_AYUAN_ZOOM_REDIRECT     = 0x213FE5; // .text 4B: 87 24 08 00 -> 05 CE 63 5E (redirect [rdi+0x824] to 0x1407FA3B8)
inline constexpr uintptr_t RVA_AYUAN_PITCH_MIN_REDIR   = 0x214635; // .text 4B: A7 C8 07 00 -> 25 7A 5D 5E (redirect [rdi+0x7c8] to 0x1407FA3B4)
inline constexpr uintptr_t RVA_AYUAN_PITCH_LIMIT1      = 0x21F6D7; // .text 2B: B5 AC -> C1 AC (load -0.75f instead of -0.30f)
inline constexpr uintptr_t RVA_AYUAN_PITCH_LIMIT2      = 0x21F6F2; // .text 1B: AA -> BE (load -75.0f from 0x1407FA3B4 instead of -5.0f)
inline constexpr uintptr_t RVA_AYUAN_ALTITUDE_REDIRECT = 0x2D108A; // .text 4B: 8F C0 07 00 -> 0D 2D 93 52 (redirect [rdi+0x7c0] to 0x1407FA3BC)

inline constexpr float DEFAULT_AYUAN_ZOOM_VAL     = 0.6981317f;
inline constexpr float DEFAULT_AYUAN_ALTITUDE_VAL = 1.400000f;

/**
 * PatchRecord
 * Tracks individual memory patches for atomic inspection, modification, and LIFO rollback.
 */
struct PatchRecord {
    uintptr_t address = 0;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> patchBytes;
    std::string name;
    bool isApplied = false;
};

/**
 * CameraPatcher
 *
 * Thread-safe, RAII-governed memory patching and dynamic camera modification engine
 * for The Legend of Heroes: Trails of Cold Steel III (v1.0.7 x64).
 *
 * Implements:
 * - Pitch clamp uncap (16 NOPs at VA 0x1401FCD2A)
 * - Zoom clamp uncap (9 NOPs at VA 0x1402DE35C)
 * - Dynamic vertical camera altitude translation via pointer chain:
 *   ActiveCameraController = [[[0x140C53290] + 0x1CE8] + 0x39F48], Eye Y at +0x368
 * - Atomic reverse LIFO rollback on detachment or signal
 * - Dead PID crash guard preventing invalid memory writes into terminated processes
 */
class CameraPatcher {
public:
    CameraPatcher();
    ~CameraPatcher();

    // Non-copyable and non-movable to ensure strict RAII lifetime and handle safety
    CameraPatcher(const CameraPatcher&) = delete;
    CameraPatcher& operator=(const CameraPatcher&) = delete;
    CameraPatcher(CameraPatcher&&) = delete;
    CameraPatcher& operator=(CameraPatcher&&) = delete;

    /**
     * Attach to running ed8_3_PC.exe process by PID.
     * Opens handle with PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION.
     * Validates process is alive, 64-bit, and resolves target addresses via ProcessScanner.
     */
    bool Attach(DWORD pid);

    /**
     * Attach using existing process handle and module base address.
     * Useful for unit test harnesses and mock process memory injection.
     */
    bool Attach(HANDLE hExistingProcess, uintptr_t moduleBase);

    /**
     * Detach from target process:
     * - Restores all active patches in strict reverse LIFO order.
     * - Flushes instruction cache.
     * - Closes process handle if owned.
     * - Guards against dead PID to prevent writing to terminated processes.
     */
    void Detach();

    /**
     * Toggle pitch clamp:
     * - unlock == true: writes 16 NOPs (0x90) to pitch clamp address.
     * - unlock == false: restores original 16 bytes (F3 0F 5D BB 24 01 00 00 F3 0F 5F BB 20 01 00 00).
     * Idempotent: repeated calls with same argument are safe no-ops.
     */
    bool SetPitchUnlocked(bool unlock);

    /**
     * Toggle zoom distance max clamp:
     * - unlock == true: writes 9 NOPs (0x90) to zoom clamp address.
     * - unlock == false: restores original 9 bytes (F3 45 0F 5D 96 C4 A7 03 00).
     * Idempotent: repeated calls with same argument are safe no-ops.
     */
    bool SetZoomUnlocked(bool unlock);

    /**
     * Dynamically adjust camera vertical altitude (height):
     * Resolves MapCamera = [[[GameManagerGlobal] + 0x1CE8] + 0x5A778] + 0x38
     * (with fallback to ActiveCameraController in mock test environments).
     * Updates cumulative altitude delta tracker.
     */
    bool AdjustAltitude(float delta);

    /**
     * Dynamically adjust camera zoom distance:
     * Modifies MapCamera distance (+0x48, +0xEC, +0xF0) in real time.
     */
    bool AdjustZoom(float delta);

    /**
     * Dynamically adjust camera vertical pitch angle (in degrees):
     * Modifies MapCamera pitch (+0x38, +0xCC, +0xDC) in real time.
     */
    bool AdjustPitch(float delta);

    /**
     * Reset all modifications to baseline defaults:
     * - Restores pitch clamp
     * - Restores zoom clamp
     * - Resets altitude delta (reverses cumulative altitude offset)
     */
    bool ResetDefaults();

    // Query Methods
    bool IsProcessAlive() const;
    bool IsAttached() const;
    bool IsPitchUnlocked() const;
    bool IsZoomUnlocked() const;
    float GetAltitudeDelta() const;
    float GetCumulativeAltitudeDelta() const;

    // Inspection & Diagnostic Accessors
    DWORD GetProcessId() const;
    HANDLE GetProcessHandle() const;
    uintptr_t GetModuleBase() const;
    size_t GetModuleSize() const;
    uintptr_t GetPitchClampAddress() const;
    uintptr_t GetZoomClampAddress() const;
    uintptr_t GetGameManagerGlobalAddress() const;
    const std::vector<PatchRecord>& GetPatchRecords() const;
    const std::vector<std::string>& GetRollbackHistory() const;

    /**
     * Configure explicit patch and global addresses.
     * Primarily used by mock test harnesses to inject custom in-memory buffers.
     */
    void SetCustomAddresses(
        uintptr_t pitchClampAddr,
        uintptr_t zoomClampAddr,
        uintptr_t gmGlobalPtr = 0
    );

private:
    enum PatchIndex : size_t {
        PATCH_PITCH = 0,
        PATCH_ZOOM = 1,
        PATCH_COUNT = 2
    };

    // Internal Helpers
    bool IsProcessAliveInternal() const;
    bool ResolveAddresses();
    void InitializePatchRecords();
    void InitializeSubPatches();
    void ApplySubPatches(std::vector<PatchRecord>& subPatches, bool apply);
    bool ResolveMapCameraInternal(uintptr_t& outGameManager, uintptr_t& outMapCamera) const;
    bool WriteMemorySafe(uintptr_t address, const std::vector<uint8_t>& bytes);
    void DetachInternal();

    mutable std::mutex m_mutex;
    HANDLE m_hProcess = nullptr;
    DWORD m_pid = 0;
    uintptr_t m_moduleBase = 0;
    size_t m_moduleSize = 0;
    bool m_ownsHandle = false;
    bool m_isAttached = false;

    // Resolved Memory Addresses
    uintptr_t m_pitchClampAddr = 0;
    uintptr_t m_zoomClampAddr = 0;
    uintptr_t m_altitudeWriteAddr = 0;
    uintptr_t m_gmGlobalPtr = 0;

    // Patch State Management
    std::vector<PatchRecord> m_patches;
    std::vector<PatchRecord> m_pitchSubPatches;
    std::vector<PatchRecord> m_zoomSubPatches;
    std::vector<PatchRecord> m_altitudeSubPatches;
    std::vector<size_t> m_appliedOrder; // Tracks patch application sequence for reverse LIFO rollback
    std::vector<std::string> m_rollbackHistory; // Records names of restored patches in execution order

    // Dynamic Altitude Tracking
    float m_cumulativeAltitudeDelta = 0.0f;
};

} // namespace ColdSteel3
