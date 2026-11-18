#pragma once

#include "common.hpp"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <optional>

namespace ColdSteel3 {

/**
 * ProcessScanner
 * 
 * Production-grade Win32 process discovery and AOB memory/disk scanning engine.
 * Supports wildcard masks ('?' = any byte, 'x' = exact byte).
 */
class ProcessScanner {
public:
    /**
     * Locate process ID by executable name using Toolhelp32 snapshot.
     * Returns 0 if process is not found or snapshot fails.
     */
    static DWORD FindProcessId(const std::wstring& processName);

    /**
     * Get module base address in target process using Toolhelp32 snapshot.
     * If moduleName is empty, returns the main executable module's base.
     * Returns 0 if module is not found.
     */
    static uintptr_t GetModuleBase(DWORD pid, const std::wstring& moduleName = L"");

    /**
     * Get module virtual size in target process using Toolhelp32 snapshot.
     * If moduleName is empty, returns the main executable module's size.
     * Returns 0 if module is not found.
     */
    static size_t GetModuleSize(DWORD pid, const std::wstring& moduleName = L"");

    /**
     * Scan live remote process memory for an AOB pattern.
     * Reads memory in buffered chunks to maximize performance and avoid reading
     * unmapped/guard pages.
     * Returns target virtual address (VA) on match, or 0 if not found.
     */
    static uintptr_t ScanPattern(
        HANDLE hProcess,
        uintptr_t base,
        size_t size,
        const char* pattern,
        const char* mask
    );

    /**
     * Scan live remote process memory using a MemorySignature structure.
     */
    static uintptr_t ScanPattern(
        HANDLE hProcess,
        uintptr_t base,
        size_t size,
        const MemorySignature& sig
    );

    /**
     * Scan an in-memory or on-disk byte buffer for the first occurrence of an AOB pattern.
     * Returns file/buffer offset on match, or INVALID_OFFSET (static_cast<size_t>(-1)) if not found.
     */
    static size_t ScanPatternOnDisk(
        const uint8_t* buffer,
        size_t bufferSize,
        const char* pattern,
        const char* mask
    );

    /**
     * Scan an in-memory or on-disk byte buffer using a MemorySignature structure.
     */
    static size_t ScanPatternOnDisk(
        const uint8_t* buffer,
        size_t bufferSize,
        const MemorySignature& sig
    );

    /**
     * Scan an in-memory or on-disk byte buffer and return all matching offsets.
     * Essential for validating signature uniqueness (100% collision-free).
     */
    static std::vector<size_t> ScanAllOccurrencesOnDisk(
        const uint8_t* buffer,
        size_t bufferSize,
        const char* pattern,
        const char* mask
    );

    /**
     * Scan an in-memory or on-disk byte buffer for all occurrences using a MemorySignature structure.
     */
    static std::vector<size_t> ScanAllOccurrencesOnDisk(
        const uint8_t* buffer,
        size_t bufferSize,
        const MemorySignature& sig
    );

    /**
     * Helper to read an entire file from disk into a vector of bytes.
     * Returns std::nullopt if file cannot be opened or read.
     */
    static std::optional<std::vector<uint8_t>> ReadFileToBuffer(const std::wstring& filePath);
    static std::optional<std::vector<uint8_t>> ReadFileToBuffer(const std::string& filePath);
};

} // namespace ColdSteel3
