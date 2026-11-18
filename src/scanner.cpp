#include "scanner.hpp"
#include <tlhelp32.h>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace ColdSteel3 {

DWORD ProcessScanner::FindProcessId(const std::wstring& processName) {
    if (processName.empty()) {
        return 0;
    }

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    DWORD targetPid = 0;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0) {
                targetPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return targetPid;
}

uintptr_t ProcessScanner::GetModuleBase(DWORD pid, const std::wstring& moduleName) {
    if (pid == 0) {
        return 0;
    }

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    uintptr_t baseAddress = 0;
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            if (moduleName.empty() || _wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                baseAddress = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }

    CloseHandle(hSnapshot);
    return baseAddress;
}

size_t ProcessScanner::GetModuleSize(DWORD pid, const std::wstring& moduleName) {
    if (pid == 0) {
        return 0;
    }

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    size_t moduleSize = 0;
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            if (moduleName.empty() || _wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                moduleSize = static_cast<size_t>(me.modBaseSize);
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }

    CloseHandle(hSnapshot);
    return moduleSize;
}

size_t ProcessScanner::ScanPatternOnDisk(
    const uint8_t* buffer,
    size_t bufferSize,
    const char* pattern,
    const char* mask
) {
    if (!buffer || !pattern || !mask) {
        return INVALID_OFFSET;
    }

    const size_t patLen = std::strlen(mask);
    if (patLen == 0 || bufferSize < patLen) {
        return INVALID_OFFSET;
    }

    // Locate first non-wildcard byte for memchr optimization
    size_t firstNonWild = 0;
    while (firstNonWild < patLen && mask[firstNonWild] == '?') {
        ++firstNonWild;
    }

    // If pattern is entirely wildcards, offset 0 is an immediate match
    if (firstNonWild == patLen) {
        return 0;
    }

    const uint8_t firstByte = static_cast<uint8_t>(pattern[firstNonWild]);
    const size_t maxScanIndex = bufferSize - patLen;
    size_t i = 0;

    while (i <= maxScanIndex) {
        const void* found = std::memchr(buffer + i + firstNonWild, firstByte, maxScanIndex - i + 1);
        if (!found) {
            break;
        }

        const size_t candidate = static_cast<const uint8_t*>(found) - buffer - firstNonWild;
        if (candidate > maxScanIndex) {
            break;
        }

        bool match = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] != '?' && buffer[candidate + j] != static_cast<uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }

        if (match) {
            return candidate;
        }

        i = candidate + 1;
    }

    return INVALID_OFFSET;
}

size_t ProcessScanner::ScanPatternOnDisk(
    const uint8_t* buffer,
    size_t bufferSize,
    const MemorySignature& sig
) {
    return ScanPatternOnDisk(buffer, bufferSize, sig.pattern, sig.mask);
}

std::vector<size_t> ProcessScanner::ScanAllOccurrencesOnDisk(
    const uint8_t* buffer,
    size_t bufferSize,
    const char* pattern,
    const char* mask
) {
    std::vector<size_t> occurrences;
    if (!buffer || !pattern || !mask) {
        return occurrences;
    }

    const size_t patLen = std::strlen(mask);
    if (patLen == 0 || bufferSize < patLen) {
        return occurrences;
    }

    // Locate first non-wildcard byte
    size_t firstNonWild = 0;
    while (firstNonWild < patLen && mask[firstNonWild] == '?') {
        ++firstNonWild;
    }

    // If all wildcards, every valid starting position matches
    if (firstNonWild == patLen) {
        occurrences.reserve(bufferSize - patLen + 1);
        for (size_t k = 0; k <= bufferSize - patLen; ++k) {
            occurrences.push_back(k);
        }
        return occurrences;
    }

    const uint8_t firstByte = static_cast<uint8_t>(pattern[firstNonWild]);
    const size_t maxScanIndex = bufferSize - patLen;
    size_t i = 0;

    while (i <= maxScanIndex) {
        const void* found = std::memchr(buffer + i + firstNonWild, firstByte, maxScanIndex - i + 1);
        if (!found) {
            break;
        }

        const size_t candidate = static_cast<const uint8_t*>(found) - buffer - firstNonWild;
        if (candidate > maxScanIndex) {
            break;
        }

        bool match = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] != '?' && buffer[candidate + j] != static_cast<uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }

        if (match) {
            occurrences.push_back(candidate);
        }

        i = candidate + 1;
    }

    return occurrences;
}

std::vector<size_t> ProcessScanner::ScanAllOccurrencesOnDisk(
    const uint8_t* buffer,
    size_t bufferSize,
    const MemorySignature& sig
) {
    return ScanAllOccurrencesOnDisk(buffer, bufferSize, sig.pattern, sig.mask);
}

uintptr_t ProcessScanner::ScanPattern(
    HANDLE hProcess,
    uintptr_t base,
    size_t size,
    const char* pattern,
    const char* mask
) {
    if (!hProcess || base == 0 || size == 0 || !pattern || !mask) {
        return 0;
    }

    const size_t patLen = std::strlen(mask);
    if (patLen == 0 || size < patLen) {
        return 0;
    }

    constexpr size_t CHUNK_SIZE = 64 * 1024; // 64 KB chunk size
    const size_t overlap = (patLen > 1) ? (patLen - 1) : 0;
    std::vector<uint8_t> chunkBuffer(CHUNK_SIZE);

    uintptr_t currentAddr = base;
    const uintptr_t endAddr = base + size;

    while (currentAddr < endAddr) {
        const size_t bytesToRead = (currentAddr + CHUNK_SIZE <= endAddr)
            ? CHUNK_SIZE
            : static_cast<size_t>(endAddr - currentAddr);

        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(
                hProcess,
                reinterpret_cast<LPCVOID>(currentAddr),
                chunkBuffer.data(),
                bytesToRead,
                &bytesRead) && bytesRead >= patLen) {
            
            const size_t matchOffset = ScanPatternOnDisk(chunkBuffer.data(), bytesRead, pattern, mask);
            if (matchOffset != INVALID_OFFSET) {
                return currentAddr + matchOffset;
            }

            if (bytesRead < bytesToRead) {
                currentAddr += bytesRead;
            } else {
                currentAddr += (bytesRead - overlap);
            }
        } else {
            // Advance by memory page (4KB) if page is unreadable/protected
            currentAddr = (currentAddr + 0x1000) & ~0xFFFULL;
        }
    }

    return 0;
}

uintptr_t ProcessScanner::ScanPattern(
    HANDLE hProcess,
    uintptr_t base,
    size_t size,
    const MemorySignature& sig
) {
    return ScanPattern(hProcess, base, size, sig.pattern, sig.mask);
}

std::optional<std::vector<uint8_t>> ProcessScanner::ReadFileToBuffer(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
        CloseHandle(hFile);
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    DWORD totalRead = 0;

    while (totalRead < buffer.size()) {
        const DWORD toRead = static_cast<DWORD>(
            std::min<size_t>(buffer.size() - totalRead, 64 * 1024 * 1024)
        );

        if (!ReadFile(hFile, buffer.data() + totalRead, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        totalRead += bytesRead;
    }

    CloseHandle(hFile);

    if (totalRead != buffer.size()) {
        return std::nullopt;
    }

    return buffer;
}

std::optional<std::vector<uint8_t>> ProcessScanner::ReadFileToBuffer(const std::string& filePath) {
    if (filePath.empty()) {
        return std::nullopt;
    }

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return std::nullopt;
    }

    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, wpath.data(), wlen);
    wpath.resize(wlen - 1); // remove null terminator

    return ReadFileToBuffer(wpath);
}

} // namespace ColdSteel3
