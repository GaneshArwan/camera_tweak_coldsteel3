#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <string_view>
#include <optional>
#include <windows.h>

namespace ColdSteel3 {

// Target process and PE constants
inline constexpr const wchar_t* DEFAULT_PROCESS_NAME = L"ed8_3_PC.exe";
inline constexpr const wchar_t* DEFAULT_MODULE_NAME = L"ed8_3_PC.exe";
inline constexpr uintptr_t DEFAULT_IMAGE_BASE = 0x140000000ULL;

// In PE .text section for ed8_3_PC.exe v1.0.7:
// Virtual Address = ImageBase (0x140000000) + RVA
// .text Raw Pointer = 0x400, .text RVA = 0x1000
// FileOffsetToRVA = fileOffset - 0x400 + 0x1000 = fileOffset + 0xC00
inline constexpr size_t TEXT_SECTION_RAW_OFFSET = 0x400;
inline constexpr uintptr_t TEXT_SECTION_RVA = 0x1000;
inline constexpr size_t TEXT_FILE_TO_RVA_DELTA = TEXT_SECTION_RVA - TEXT_SECTION_RAW_OFFSET; // 0xC00

inline constexpr size_t INVALID_OFFSET = static_cast<size_t>(-1);

// Error codes for scanning and memory operations
enum class ScanErrorCode {
    Success = 0,
    ProcessNotFound,
    ModuleNotFound,
    ProcessOpenFailed,
    PatternNotFound,
    PatternCollision,
    BufferEmpty,
    BufferTooSmall,
    InvalidPatternOrMask,
    MemoryReadFailed
};

// Unified structure for representing Array-Of-Bytes (AOB) signatures
struct MemorySignature {
    const char* name;
    const char* pattern;
    const char* mask;
    size_t length;
    size_t expectedFileOffset;
    uintptr_t expectedVA;
};

// Core Signatures verified on ed8_3_PC.exe (v1.0.7 x64)

// 1. Pitch Clamp (Field Camera minss + maxss)
// VA: 0x1401FCD2A, File Offset: 0x1FC12A, Length: 16 bytes
// Disassembly:
//   minss xmm7, dword ptr [rbx + 0x124] (F3 0F 5D BB 24 01 00 00)
//   maxss xmm7, dword ptr [rbx + 0x120] (F3 0F 5F BB 20 01 00 00)
inline constexpr char SIG_PITCH_CLAMP_PATTERN[] =
    "\xF3\x0F\x5D\xBB\x24\x01\x00\x00\xF3\x0F\x5F\xBB\x20\x01\x00\x00";
inline constexpr char SIG_PITCH_CLAMP_MASK[] = "xxxxxxxxxxxxxxxx";
inline constexpr size_t OFFSET_PITCH_CLAMP = 0x1FC12A;
inline constexpr uintptr_t VA_PITCH_CLAMP = 0x1401FCD2A;

inline const MemorySignature SIG_PITCH_CLAMP = {
    "Pitch Clamp (Field)",
    SIG_PITCH_CLAMP_PATTERN,
    SIG_PITCH_CLAMP_MASK,
    16,
    OFFSET_PITCH_CLAMP,
    VA_PITCH_CLAMP
};

// 2. Zoom Clamp (Max zoom distance minss)
// VA: 0x1402DE35C, File Offset: 0x2DD75C, Length: 9 bytes
// Disassembly:
//   minss xmm10, dword ptr [r14 + 0x3a7c4] (F3 45 0F 5D 96 C4 A7 03 00)
inline constexpr char SIG_ZOOM_CLAMP_PATTERN[] =
    "\xF3\x45\x0F\x5D\x96\xC4\xA7\x03\x00";
inline constexpr char SIG_ZOOM_CLAMP_MASK[] = "xxxxxxxxx";
inline constexpr size_t OFFSET_ZOOM_CLAMP = 0x2DD75C;
inline constexpr uintptr_t VA_ZOOM_CLAMP = 0x1402DE35C;

inline const MemorySignature SIG_ZOOM_CLAMP = {
    "Zoom Clamp (Max Distance)",
    SIG_ZOOM_CLAMP_PATTERN,
    SIG_ZOOM_CLAMP_MASK,
    9,
    OFFSET_ZOOM_CLAMP,
    VA_ZOOM_CLAMP
};

// 3. Altitude Write (Camera Eye Y position movss)
// VA: 0x1402DE3BB, File Offset: 0x2DD7BB, Length: 8 bytes
// Disassembly:
//   movss dword ptr [rsi + 0x368], xmm0 (F3 0F 11 86 68 03 00 00)
inline constexpr char SIG_ALTITUDE_WRITE_PATTERN[] =
    "\xF3\x0F\x11\x86\x68\x03\x00\x00";
inline constexpr char SIG_ALTITUDE_WRITE_MASK[] = "xxxxxxxx";
inline constexpr size_t OFFSET_ALTITUDE_WRITE = 0x2DD7BB;
inline constexpr uintptr_t VA_ALTITUDE_WRITE = 0x1402DE3BB;

inline const MemorySignature SIG_ALTITUDE_WRITE = {
    "Altitude Write (Eye Y)",
    SIG_ALTITUDE_WRITE_PATTERN,
    SIG_ALTITUDE_WRITE_MASK,
    8,
    OFFSET_ALTITUDE_WRITE,
    VA_ALTITUDE_WRITE
};

// 4. GameManager Resolver
// VA: 0x1402A7D00, File Offset: 0x2A7100, Length: 15 bytes
// Disassembly:
//   mov rax, qword ptr [rip + 0x9ab589] ; [0x140C53290] (48 8B 05 ?? ?? ?? ??)
//   mov rax, qword ptr [rax + 0x1ce8]                   (48 8B 80 E8 1C 00 00)
//   ret                                                 (C3)
inline constexpr char SIG_GM_RESOLVER_PATTERN[] =
    "\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x80\xE8\x1C\x00\x00\xC3";
inline constexpr char SIG_GM_RESOLVER_MASK[] = "xxx????xxxxxxxx";
inline constexpr size_t OFFSET_GM_RESOLVER = 0x2A7100;
inline constexpr uintptr_t VA_GM_RESOLVER = 0x1402A7D00;

inline const MemorySignature SIG_GM_RESOLVER = {
    "GameManager Resolver",
    SIG_GM_RESOLVER_PATTERN,
    SIG_GM_RESOLVER_MASK,
    15,
    OFFSET_GM_RESOLVER,
    VA_GM_RESOLVER
};

// 5. Indoor / Special Pitch Clamp (Branch A, secondary)
// VA: 0x1401FCC46, File Offset: 0x1FC046, Length: 20 bytes
// Disassembly:
//   movss xmm0, dword ptr [rip + 0x60585e] ; (F3 0F 10 05 ?? ?? ?? ??)
//   minss xmm7, dword ptr [rip + 0x602be6] ; (F3 0F 5D 3D ?? ?? ?? ??)
//   maxss xmm0, xmm7                       ; (F3 0F 5F C7)
inline constexpr char SIG_PITCH_CLAMP_INDOOR_PATTERN[] =
    "\xF3\x0F\x10\x05\x00\x00\x00\x00\xF3\x0F\x5D\x3D\x00\x00\x00\x00\xF3\x0F\x5F\xC7";
inline constexpr char SIG_PITCH_CLAMP_INDOOR_MASK[] = "xxxx????xxxx????xxxx";
inline constexpr size_t OFFSET_PITCH_CLAMP_INDOOR = 0x1FC046;
inline constexpr uintptr_t VA_PITCH_CLAMP_INDOOR = 0x1401FCC46;

inline const MemorySignature SIG_PITCH_CLAMP_INDOOR = {
    "Pitch Clamp (Indoor Branch A)",
    SIG_PITCH_CLAMP_INDOOR_PATTERN,
    SIG_PITCH_CLAMP_INDOOR_MASK,
    20,
    OFFSET_PITCH_CLAMP_INDOOR,
    VA_PITCH_CLAMP_INDOOR
};

// 6. Current Zoom Distance Write
// VA: 0x1402DE3F7, File Offset: 0x2DD7F7, Length: 9 bytes
// Disassembly:
//   movss dword ptr [rsi + 0x65c], xmm10 (F3 44 0F 11 96 5C 06 00 00)
inline constexpr char SIG_ZOOM_WRITE_PATTERN[] =
    "\xF3\x44\x0F\x11\x96\x5C\x06\x00\x00";
inline constexpr char SIG_ZOOM_WRITE_MASK[] = "xxxxxxxxx";
inline constexpr size_t OFFSET_ZOOM_WRITE = 0x2DD7F7;
inline constexpr uintptr_t VA_ZOOM_WRITE = 0x1402DE3F7;

inline const MemorySignature SIG_ZOOM_WRITE = {
    "Zoom Distance Write",
    SIG_ZOOM_WRITE_PATTERN,
    SIG_ZOOM_WRITE_MASK,
    9,
    OFFSET_ZOOM_WRITE,
    VA_ZOOM_WRITE
};

// Scan result structure
struct ScanResult {
    bool found = false;
    uintptr_t address = 0;
    size_t fileOffset = INVALID_OFFSET;
    size_t matchCount = 0;
    ScanErrorCode errorCode = ScanErrorCode::Success;
    std::string message;
};

// Helper functions for address translation
inline constexpr uintptr_t FileOffsetToVA(size_t fileOffset, uintptr_t imageBase = DEFAULT_IMAGE_BASE) noexcept {
    return imageBase + fileOffset + TEXT_FILE_TO_RVA_DELTA;
}

inline constexpr size_t VAToFileOffset(uintptr_t va, uintptr_t imageBase = DEFAULT_IMAGE_BASE) noexcept {
    return (va >= imageBase + TEXT_FILE_TO_RVA_DELTA)
        ? static_cast<size_t>(va - imageBase - TEXT_FILE_TO_RVA_DELTA)
        : INVALID_OFFSET;
}

} // namespace ColdSteel3
