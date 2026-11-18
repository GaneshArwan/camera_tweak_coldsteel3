#include "../src/scanner.hpp"
#include "../src/common.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <windows.h>

using namespace ColdSteel3;

static int g_passedTests = 0;
static int g_failedTests = 0;

#define ADV_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] Line " << __LINE__ << ": " << msg << std::endl; \
            g_failedTests++; \
            return false; \
        } \
    } while (0)

#define ADV_RUN_TEST(fn) \
    do { \
        std::cout << "[RUN ] " << #fn << "..." << std::endl; \
        if (fn()) { \
            std::cout << "[PASS] " << #fn << std::endl; \
            g_passedTests++; \
        } else { \
            std::cout << "[FAIL] " << #fn << std::endl; \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// 1. Extreme and Pathological Edge Cases: Null, Empty, and Inverted Sizes
// ---------------------------------------------------------------------------

bool Test_Pathological_EmptyAndNullInputs() {
    uint8_t dummy = 0xAA;
    const char pat[] = "\xAA";
    const char mask[] = "x";

    // Null buffer
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(nullptr, 0, pat, mask) == INVALID_OFFSET,
               "Null buffer with size 0 must return INVALID_OFFSET");
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(nullptr, 100, pat, mask) == INVALID_OFFSET,
               "Null buffer with size 100 must return INVALID_OFFSET");

    // Zero-length buffer
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(&dummy, 0, pat, mask) == INVALID_OFFSET,
               "Non-null buffer with size 0 must return INVALID_OFFSET");

    // Null pattern or mask
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(&dummy, 1, nullptr, mask) == INVALID_OFFSET,
               "Null pattern must return INVALID_OFFSET");
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(&dummy, 1, pat, nullptr) == INVALID_OFFSET,
               "Null mask must return INVALID_OFFSET");

    // Empty mask (patLen == 0)
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(&dummy, 1, pat, "") == INVALID_OFFSET,
               "Empty mask must return INVALID_OFFSET");

    // ScanAllOccurrencesOnDisk null / empty checks
    auto occ1 = ProcessScanner::ScanAllOccurrencesOnDisk(nullptr, 0, pat, mask);
    ADV_ASSERT(occ1.empty(), "Null buffer must return empty occurrences");

    auto occ2 = ProcessScanner::ScanAllOccurrencesOnDisk(&dummy, 0, pat, mask);
    ADV_ASSERT(occ2.empty(), "Zero-size buffer must return empty occurrences");

    auto occ3 = ProcessScanner::ScanAllOccurrencesOnDisk(&dummy, 1, nullptr, mask);
    ADV_ASSERT(occ3.empty(), "Null pattern must return empty occurrences");

    auto occ4 = ProcessScanner::ScanAllOccurrencesOnDisk(&dummy, 1, pat, nullptr);
    ADV_ASSERT(occ4.empty(), "Null mask must return empty occurrences");

    auto occ5 = ProcessScanner::ScanAllOccurrencesOnDisk(&dummy, 1, pat, "");
    ADV_ASSERT(occ5.empty(), "Empty mask must return empty occurrences");

    return true;
}

bool Test_Pathological_PatternLongerThanBuffer() {
    const uint8_t bufSmall[3] = { 0x10, 0x20, 0x30 };
    const char patLong[] = "\x10\x20\x30\x40";
    const char maskLong[] = "xxxx";

    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(bufSmall, 3, patLong, maskLong) == INVALID_OFFSET,
               "Pattern longer than buffer must return INVALID_OFFSET");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(bufSmall, 3, patLong, maskLong);
    ADV_ASSERT(occ.empty(), "Pattern longer than buffer must return 0 occurrences");

    // Buffer size 1, pattern size 100
    std::string hugeMask(100, 'x');
    std::string hugePat(100, '\x10');
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(bufSmall, 1, hugePat.c_str(), hugeMask.c_str()) == INVALID_OFFSET,
               "100-byte pattern on 1-byte buffer must return INVALID_OFFSET");

    return true;
}

bool Test_Pathological_PatternEqualBuffer() {
    const uint8_t buffer[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const char patExact[] = "\xDE\xAD\xBE\xEF";
    const char maskExact[] = "xxxx";

    // Exact match
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(buffer, 4, patExact, maskExact) == 0,
               "Equal size buffer with exact match must return 0");
    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer, 4, patExact, maskExact);
    ADV_ASSERT(occ.size() == 1 && occ[0] == 0, "Exact match must yield {0}");

    // Mismatch at byte 0
    const char patFail0[] = "\x00\xAD\xBE\xEF";
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(buffer, 4, patFail0, maskExact) == INVALID_OFFSET,
               "Mismatch at byte 0 must return INVALID_OFFSET");

    // Mismatch at byte 3 (last byte)
    const char patFail3[] = "\xDE\xAD\xBE\x00";
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(buffer, 4, patFail3, maskExact) == INVALID_OFFSET,
               "Mismatch at last byte must return INVALID_OFFSET");

    // Buffer size 1, pattern size 1
    const uint8_t b1 = 0x77;
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(&b1, 1, "\x77", "x") == 0, "1-byte exact match must be 0");
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(&b1, 1, "\x88", "x") == INVALID_OFFSET, "1-byte mismatch must fail");

    return true;
}

// ---------------------------------------------------------------------------
// 2. Wildcard Permutations: All Wildcards, Index 0, and Last Byte Wildcards
// ---------------------------------------------------------------------------

bool Test_Pathological_AllWildcards() {
    // 1-byte wildcard
    const uint8_t buf5[5] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(buf5, 5, "\x00", "?") == 0,
               "1-byte wildcard first match must be 0");
    auto occ1 = ProcessScanner::ScanAllOccurrencesOnDisk(buf5, 5, "\x00", "?");
    ADV_ASSERT(occ1.size() == 5, "1-byte wildcard in 5-byte buffer must return 5 occurrences");
    for (size_t i = 0; i < 5; ++i) {
        ADV_ASSERT(occ1[i] == i, "Occurrence index must match i");
    }

    // 4-byte wildcard
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(buf5, 5, "\x00\x00\x00\x00", "????") == 0,
               "4-byte wildcard first match must be 0");
    auto occ4 = ProcessScanner::ScanAllOccurrencesOnDisk(buf5, 5, "\x00\x00\x00\x00", "????");
    ADV_ASSERT(occ4.size() == 2, "4-byte wildcard in 5-byte buffer must return 2 occurrences");
    ADV_ASSERT(occ4[0] == 0 && occ4[1] == 1, "Offsets must be {0, 1}");

    // All wildcards with buffer size smaller than pattern
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(buf5, 3, "\x00\x00\x00\x00", "????") == INVALID_OFFSET,
               "Wildcard pattern larger than buffer must return INVALID_OFFSET");
    auto occUnder = ProcessScanner::ScanAllOccurrencesOnDisk(buf5, 3, "\x00\x00\x00\x00", "????");
    ADV_ASSERT(occUnder.empty(), "Wildcard pattern larger than buffer must return empty");

    return true;
}

bool Test_Pathological_WildcardAtIndexZero() {
    // Pattern: ? 11 22 33 (mask: "?xxx")
    const char pat[] = "\x00\x11\x22\x33";
    const char mask[] = "?xxx";

    // Case A: Match at index 0 (wildcard covers 0x99)
    const uint8_t bufA[] = { 0x99, 0x11, 0x22, 0x33, 0xAA };
    size_t matchA = ProcessScanner::ScanPatternOnDisk(bufA, sizeof(bufA), pat, mask);
    ADV_ASSERT(matchA == 0, "Match at index 0 with wildcard at 0 failed");

    // Case B: Match at index 3
    const uint8_t bufB[] = { 0x00, 0x11, 0x00, 0xFE, 0x11, 0x22, 0x33 };
    size_t matchB = ProcessScanner::ScanPatternOnDisk(bufB, sizeof(bufB), pat, mask);
    ADV_ASSERT(matchB == 3, "Match at index 3 with wildcard at 0 failed");

    // Case C: False positive at index 0 (0x11 matches, but 0x22 is absent), true match at index 4
    const uint8_t bufC[] = { 0xFF, 0x11, 0x99, 0x00, 0xEE, 0x11, 0x22, 0x33 };
    size_t matchC = ProcessScanner::ScanPatternOnDisk(bufC, sizeof(bufC), pat, mask);
    ADV_ASSERT(matchC == 4, "False candidate backtracking failed");

    // Case D: Match at the very end of buffer
    const uint8_t bufD[] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    // Pattern length = 4. Buffer size = 6. Match starts at 2 (ends at 5).
    size_t matchD = ProcessScanner::ScanPatternOnDisk(bufD, sizeof(bufD), pat, mask);
    ADV_ASSERT(matchD == 2, "Match at end of buffer with wildcard at 0 failed");

    // Case E: Mismatch at last byte of buffer
    const uint8_t bufE[] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x00 };
    size_t matchE = ProcessScanner::ScanPatternOnDisk(bufE, sizeof(bufE), pat, mask);
    ADV_ASSERT(matchE == INVALID_OFFSET, "Mismatch at end of buffer must return INVALID_OFFSET");

    return true;
}

bool Test_Pathological_WildcardAtLastByte() {
    // Pattern: 11 22 33 ? (mask: "xxx?")
    const char pat[] = "\x11\x22\x33\x00";
    const char mask[] = "xxx?";

    // Case A: Match at index 0 (wildcard covers 0xAA)
    const uint8_t bufA[] = { 0x11, 0x22, 0x33, 0xAA, 0x55 };
    size_t matchA = ProcessScanner::ScanPatternOnDisk(bufA, sizeof(bufA), pat, mask);
    ADV_ASSERT(matchA == 0, "Match at index 0 with wildcard at last byte failed");

    // Case B: Match at the very last allowable position (bufferSize - patLen)
    const uint8_t bufB[] = { 0x00, 0x00, 0x11, 0x22, 0x33, 0xFF };
    // sizeof(bufB) = 6, patLen = 4, maxScanIndex = 2.
    size_t matchB = ProcessScanner::ScanPatternOnDisk(bufB, sizeof(bufB), pat, mask);
    ADV_ASSERT(matchB == 2, "Match at exact buffer end with wildcard at last byte failed");

    // Case C: False positive where 11 22 matches, but 33 is absent
    const uint8_t bufC[] = { 0x11, 0x22, 0x00, 0x00, 0x11, 0x22, 0x33, 0x77 };
    size_t matchC = ProcessScanner::ScanPatternOnDisk(bufC, sizeof(bufC), pat, mask);
    ADV_ASSERT(matchC == 4, "False positive resolution failed");

    return true;
}

bool Test_Pathological_MultipleWildcardConfigurations() {
    // 3 leading wildcards: ???x
    const char patLead[] = "\x00\x00\x00\x42";
    const char maskLead[] = "???x";
    const uint8_t bufLead[] = { 0x10, 0x20, 0x30, 0x42, 0x50 };
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(bufLead, sizeof(bufLead), patLead, maskLead) == 0,
               "Leading wildcards (???x) match at 0 failed");

    // 3 trailing wildcards: x???
    const char patTrail[] = "\x42\x00\x00\x00";
    const char maskTrail[] = "x???";
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(bufLead + 3, 4, patTrail, maskTrail) == 0,
               "Trailing wildcards [x???] match at 0 failed");

    // Alternating wildcards: ?x?x?
    const char patAlt[] = "\x00\x11\x00\x22\x00";
    const char maskAlt[] = "?x?x?";
    const uint8_t bufAlt[] = { 0xAA, 0x11, 0xBB, 0x22, 0xCC };
    ADV_ASSERT(ProcessScanner::ScanPatternOnDisk(bufAlt, sizeof(bufAlt), patAlt, maskAlt) == 0,
               "Alternating wildcards (?x?x?) failed");

    // Long pattern: 31 wildcards + 1 byte at the end
    std::string longMask(31, '?');
    longMask.push_back('x');
    std::string longPat(31, '\x00');
    longPat.push_back('\x7F');

    std::vector<uint8_t> longBuf(64, 0x00);
    longBuf[40] = 0x7F; // Pattern starts at index 40 - 31 = 9
    size_t longMatch = ProcessScanner::ScanPatternOnDisk(longBuf.data(), longBuf.size(), longPat.c_str(), longMask.c_str());
    ADV_ASSERT(longMatch == 9, "31 leading wildcards match at index 9 failed");

    return true;
}

// ---------------------------------------------------------------------------
// 3. Pathological Partial Matches: Worst-Case Repetition Stress
// ---------------------------------------------------------------------------

bool Test_Pathological_RepeatedPartialMatches() {
    // Pattern: AA AA AA BB (length 4)
    const char pat[] = "\xAA\xAA\xAA\xBB";
    const char mask[] = "xxxx";

    // Buffer filled with 100,000 bytes of AA AA AA AA ...
    // Every candidate matches 3 bytes, fails on byte 4.
    constexpr size_t PARTIAL_BUF_SIZE = 100000;
    std::vector<uint8_t> buffer(PARTIAL_BUF_SIZE, 0xAA);

    // Scan should fail completely and quickly without crashing or hanging
    auto t0 = std::chrono::high_resolution_clock::now();
    size_t result = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), pat, mask);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    ADV_ASSERT(result == INVALID_OFFSET, "Pathological AA...AA buffer must return INVALID_OFFSET");
    ADV_ASSERT(ms < 100.0, "100k pathological partial match scan must complete in < 100ms");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), pat, mask);
    ADV_ASSERT(occ.empty(), "Pathological AA...AA buffer must yield 0 occurrences");

    // Now plant a single true match at the VERY LAST possible offset
    // Offset = PARTIAL_BUF_SIZE - 4
    buffer[PARTIAL_BUF_SIZE - 1] = 0xBB;
    size_t foundMatch = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), pat, mask);
    ADV_ASSERT(foundMatch == PARTIAL_BUF_SIZE - 4,
               "True match at the end of pathological AA... buffer was not found correctly");

    auto occLast = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), pat, mask);
    ADV_ASSERT(occLast.size() == 1 && occLast[0] == PARTIAL_BUF_SIZE - 4,
               "Occurrences must contain exactly one match at the end");

    return true;
}

// ---------------------------------------------------------------------------
// 4. Memory Safety & Guard Page Protection (Proves Zero OOB Reads)
// ---------------------------------------------------------------------------

bool Test_Pathological_MemoryBoundaryGuard() {
    // Allocate 2 virtual pages (8192 bytes).
    // Page 1 (offset 0..4095) is PAGE_READWRITE.
    // Page 2 (offset 4096..8191) is PAGE_NOACCESS.
    // If ProcessScanner reads even 1 byte past Page 1, an access violation will crash the process.
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const DWORD pageSize = sysInfo.dwPageSize; // Typically 4096

    uint8_t* pPages = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, pageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    );
    ADV_ASSERT(pPages != nullptr, "VirtualAlloc failed");

    // Make second page NOACCESS
    DWORD oldProtect = 0;
    BOOL protOk = VirtualProtect(pPages + pageSize, pageSize, PAGE_NOACCESS, &oldProtect);
    ADV_ASSERT(protOk, "VirtualProtect PAGE_NOACCESS failed");

    // Fill Page 1 with known data
    std::memset(pPages, 0x55, pageSize);

    // Place 4-byte pattern at the very end of Page 1: [pageSize - 4, pageSize - 1]
    const char pat[] = "\x11\x22\x33\x44";
    const char mask[] = "xxxx";
    pPages[pageSize - 4] = 0x11;
    pPages[pageSize - 3] = 0x22;
    pPages[pageSize - 2] = 0x33;
    pPages[pageSize - 1] = 0x44;

    // Test A: Exact match at page boundary
    size_t matchBoundary = ProcessScanner::ScanPatternOnDisk(pPages, pageSize, pat, mask);
    ADV_ASSERT(matchBoundary == pageSize - 4, "Match at exact page boundary failed");

    // Test B: Mismatch at page boundary (last byte differs)
    pPages[pageSize - 1] = 0x99;
    size_t noMatch = ProcessScanner::ScanPatternOnDisk(pPages, pageSize, pat, mask);
    ADV_ASSERT(noMatch == INVALID_OFFSET, "Mismatch at page boundary must return INVALID_OFFSET");

    // Test C: Wildcard at last byte at page boundary
    const char maskWildLast[] = "xxx?";
    size_t matchWildLast = ProcessScanner::ScanPatternOnDisk(pPages, pageSize, pat, maskWildLast);
    ADV_ASSERT(matchWildLast == pageSize - 4, "Wildcard at last byte at page boundary failed");

    // Test D: Wildcard at first byte at page boundary
    const char maskWildFirst[] = "?xxx";
    const char patWildFirst[] = "\x00\x22\x33\x99";
    size_t matchWildFirst = ProcessScanner::ScanPatternOnDisk(pPages, pageSize, patWildFirst, maskWildFirst);
    ADV_ASSERT(matchWildFirst == pageSize - 4, "Wildcard at first byte at page boundary failed");

    // Cleanup memory
    VirtualFree(pPages, 0, MEM_RELEASE);
    return true;
}

// ---------------------------------------------------------------------------
// 5. High-Performance Stress Test: 50,000 Iterations over 5 MB Buffer
// ---------------------------------------------------------------------------

bool Test_Stress_HighPerformance_50k_Iterations_5MB() {
    constexpr size_t BUFFER_SIZE = 5 * 1024 * 1024; // 5 MB (5,242,880 bytes)
    constexpr int ITERATIONS = 50000;

    std::cout << "       Allocating 5 MB buffer and populating deterministic noise..." << std::endl;
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    // Fast deterministic LCG to create pseudo-random noise avoiding false positives
    uint32_t seed = 0x12345678;
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        seed = seed * 1664525u + 1013904223u;
        buffer[i] = static_cast<uint8_t>((seed >> 16) & 0x7F); // values 0..127
    }

    // Embed an 8-byte signature with bytes > 128 at the 2.5 MB midpoint
    constexpr size_t TARGET_OFFSET = 2500000;
    const char pat[] = "\xEE\xFF\xAA\xBB\xCC\xDD\x88\x99";
    const char mask[] = "xxxxxxxx";
    for (size_t j = 0; j < 8; ++j) {
        buffer[TARGET_OFFSET + j] = static_cast<uint8_t>(pat[j]);
    }

    // Verify initial single match
    size_t initCheck = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), pat, mask);
    ADV_ASSERT(initCheck == TARGET_OFFSET, "Initial match assertion failed");

    std::cout << "       Executing " << ITERATIONS << " pattern scans over 5 MB buffer..." << std::endl;

    const auto startTime = std::chrono::high_resolution_clock::now();

    size_t lastFound = 0;
    for (int it = 0; it < ITERATIONS; ++it) {
        lastFound = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), pat, mask);
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    const double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    const double elapsedMs = elapsedSec * 1000.0;

    ADV_ASSERT(lastFound == TARGET_OFFSET, "Stress test offset mismatch");

    // Calculate throughput metrics
    const double scannedMBPerIter = TARGET_OFFSET / (1024.0 * 1024.0); // scanned up to match
    const double totalScannedGB = (scannedMBPerIter * ITERATIONS) / 1024.0;
    const double throughputGBs = totalScannedGB / elapsedSec;
    const double iterPerSec = ITERATIONS / elapsedSec;

    std::cout << "       ----------------------------------------------------" << std::endl;
    std::cout << "       Iterations completed: " << ITERATIONS << std::endl;
    std::cout << "       Total wall-clock time: " << std::fixed << std::setprecision(2)
              << elapsedMs << " ms (" << elapsedSec << " s)" << std::endl;
    std::cout << "       Throughput: " << std::setprecision(2) << throughputGBs << " GB/s" << std::endl;
    std::cout << "       Rate: " << std::setprecision(0) << iterPerSec << " iterations/sec" << std::endl;
    std::cout << "       Average latency per scan: " << std::setprecision(4)
              << (elapsedMs / ITERATIONS) << " ms" << std::endl;
    std::cout << "       ----------------------------------------------------" << std::endl;

    // Strict performance assertion: 50k scans must finish comfortably under 30 seconds
    ADV_ASSERT(elapsedSec < 30.0, "50,000 iterations over 5MB took longer than 30s SLA");

    return true;
}

// ---------------------------------------------------------------------------
// 6. Process Attachment & Memory Read Fault Resilience
// ---------------------------------------------------------------------------

bool Test_ProcessScan_InvalidHandlesAndBases() {
    const char pat[] = "\x90\x90";
    const char mask[] = "xx";

    // Null and invalid handle tests
    ADV_ASSERT(ProcessScanner::ScanPattern(nullptr, 0x140000000ULL, 0x1000, pat, mask) == 0,
               "Null process handle must safely return 0");
    ADV_ASSERT(ProcessScanner::ScanPattern(INVALID_HANDLE_VALUE, 0x140000000ULL, 0x1000, pat, mask) == 0,
               "INVALID_HANDLE_VALUE must safely return 0");

    // Zero base or zero size
    HANDLE hCurrent = GetCurrentProcess();
    ADV_ASSERT(ProcessScanner::ScanPattern(hCurrent, 0, 0x1000, pat, mask) == 0,
               "Base address 0 must safely return 0");
    ADV_ASSERT(ProcessScanner::ScanPattern(hCurrent, 0x140000000ULL, 0, pat, mask) == 0,
               "Size 0 must safely return 0");

    // Null pattern or mask
    ADV_ASSERT(ProcessScanner::ScanPattern(hCurrent, 0x140000000ULL, 0x1000, nullptr, mask) == 0,
               "Null pattern must safely return 0");
    ADV_ASSERT(ProcessScanner::ScanPattern(hCurrent, 0x140000000ULL, 0x1000, pat, nullptr) == 0,
               "Null mask must safely return 0");

    // Read non-existent file
    auto nonExistent = ProcessScanner::ReadFileToBuffer("C:\\path\\does\\not\\exist_123456789.bin");
    ADV_ASSERT(!nonExistent.has_value(), "Reading non-existent file must return nullopt");

    return true;
}

// ---------------------------------------------------------------------------
// 7. Signature Truncation and Real Binary Adversarial Slices
// ---------------------------------------------------------------------------

bool Test_RealBinary_AdversarialSlices() {
    const wchar_t* envPath = _wgetenv(L"CS3_EXE_PATH");
    if (!envPath || envPath[0] == L'\0') {
        std::cout << "       [SKIP] CS3_EXE_PATH not set; skipping real disk binary slice testing." << std::endl;
        return true;
    }
    auto fileOpt = ProcessScanner::ReadFileToBuffer(std::wstring(envPath));
    ADV_ASSERT(fileOpt.has_value(), "Target binary could not be opened for slice testing");

    const std::vector<uint8_t>& fullBin = *fileOpt;

    // Pitch Clamp expected offset: 0x1FC12A, length: 16
    const size_t expectedOffset = OFFSET_PITCH_CLAMP;
    const size_t sigLen = SIG_PITCH_CLAMP.length;

    // Slice 1: Truncate buffer immediately before pattern start
    // Buffer size = expectedOffset
    size_t resBefore = ProcessScanner::ScanPatternOnDisk(fullBin.data(), expectedOffset, SIG_PITCH_CLAMP);
    ADV_ASSERT(resBefore == INVALID_OFFSET, "Scanning truncated buffer before pattern must return INVALID_OFFSET");

    // Slice 2: Truncate buffer in the middle of the pattern (pattern cut off by 1 byte)
    // Buffer size = expectedOffset + sigLen - 1
    size_t resMid = ProcessScanner::ScanPatternOnDisk(fullBin.data(), expectedOffset + sigLen - 1, SIG_PITCH_CLAMP);
    ADV_ASSERT(resMid == INVALID_OFFSET, "Scanning truncated buffer with 15/16 bytes must return INVALID_OFFSET");

    // Slice 3: Exact buffer cut off at pattern end
    // Buffer size = expectedOffset + sigLen
    size_t resExact = ProcessScanner::ScanPatternOnDisk(fullBin.data(), expectedOffset + sigLen, SIG_PITCH_CLAMP);
    ADV_ASSERT(resExact == expectedOffset, "Scanning buffer containing exact pattern at the very end failed");

    return true;
}

// ---------------------------------------------------------------------------
// Main Adversarial Test Runner
// ---------------------------------------------------------------------------

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << " ProcessScanner Adversarial & Stress Verification Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    ADV_RUN_TEST(Test_Pathological_EmptyAndNullInputs);
    ADV_RUN_TEST(Test_Pathological_PatternLongerThanBuffer);
    ADV_RUN_TEST(Test_Pathological_PatternEqualBuffer);
    ADV_RUN_TEST(Test_Pathological_AllWildcards);
    ADV_RUN_TEST(Test_Pathological_WildcardAtIndexZero);
    ADV_RUN_TEST(Test_Pathological_WildcardAtLastByte);
    ADV_RUN_TEST(Test_Pathological_MultipleWildcardConfigurations);
    ADV_RUN_TEST(Test_Pathological_RepeatedPartialMatches);
    ADV_RUN_TEST(Test_Pathological_MemoryBoundaryGuard);
    ADV_RUN_TEST(Test_ProcessScan_InvalidHandlesAndBases);
    ADV_RUN_TEST(Test_RealBinary_AdversarialSlices);
    ADV_RUN_TEST(Test_Stress_HighPerformance_50k_Iterations_5MB);

    std::cout << "============================================================" << std::endl;
    std::cout << " Adversarial Results: Passed: " << g_passedTests
              << " | Failed: " << g_failedTests << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
