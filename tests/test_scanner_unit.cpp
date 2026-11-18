#include "../src/scanner.hpp"
#include "../src/common.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cassert>
#include <cstdlib>
#include <string>

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

// ---------------------------------------------------------------------------
// 1. Synthetic Unit Tests (Edge Cases & Wildcard Mechanics)
// ---------------------------------------------------------------------------

bool Test_Synthetic_EmptyBuffer() {
    const uint8_t dummy = 0x90;
    const char pat[] = "\x90";
    const char mask[] = "x";

    TEST_ASSERT(ProcessScanner::ScanPatternOnDisk(nullptr, 0, pat, mask) == INVALID_OFFSET,
                "Null buffer should return INVALID_OFFSET");
    TEST_ASSERT(ProcessScanner::ScanPatternOnDisk(&dummy, 0, pat, mask) == INVALID_OFFSET,
                "Zero size buffer should return INVALID_OFFSET");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(nullptr, 0, pat, mask);
    TEST_ASSERT(occ.empty(), "Null buffer should return empty vector");

    occ = ProcessScanner::ScanAllOccurrencesOnDisk(&dummy, 0, pat, mask);
    TEST_ASSERT(occ.empty(), "Zero size buffer should return empty vector");

    return true;
}

bool Test_Synthetic_BufferSmallerThanPattern() {
    const uint8_t buffer[] = { 0x11, 0x22 };
    const char pat[] = "\x11\x22\x33";
    const char mask[] = "xxx";

    TEST_ASSERT(ProcessScanner::ScanPatternOnDisk(buffer, sizeof(buffer), pat, mask) == INVALID_OFFSET,
                "Pattern larger than buffer must return INVALID_OFFSET");
    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(occ.empty(), "Pattern larger than buffer must return empty vector");
    return true;
}

bool Test_Synthetic_WildcardPattern() {
    // Search for 0xAA ?? 0xCC with mask "x?x"
    const uint8_t buffer[] = {
        0x00, 0xAA, 0x11, 0xCC, 0x00, 0xAA, 0x99, 0xCC, 0x55
    };
    const char pat[] = "\xAA\x00\xCC";
    const char mask[] = "x?x";

    size_t first = ProcessScanner::ScanPatternOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(first == 1, "First occurrence should be at index 1");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(occ.size() == 2, "Expected exactly 2 occurrences");
    TEST_ASSERT(occ[0] == 1, "First occurrence must be index 1");
    TEST_ASSERT(occ[1] == 5, "Second occurrence must be index 5");
    return true;
}

bool Test_Synthetic_NullBytePattern() {
    // Pattern containing embedded 0x00 bytes
    const uint8_t buffer[] = {
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC3
    };
    const char pat[] = "\x05\x00\x00\x00\x00\x48";
    const char mask[] = "xxxxxx";

    size_t first = ProcessScanner::ScanPatternOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(first == 2, "Pattern with null bytes should be located at index 2");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(occ.size() == 1, "Expected exactly 1 occurrence");
    TEST_ASSERT(occ[0] == 2, "Offset must be index 2");
    return true;
}

bool Test_Synthetic_AllWildcards() {
    const uint8_t buffer[] = { 0x10, 0x20, 0x30, 0x40 };
    const char pat[] = "\x00\x00";
    const char mask[] = "??";

    size_t first = ProcessScanner::ScanPatternOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(first == 0, "All wildcards first match should be 0");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer, sizeof(buffer), pat, mask);
    TEST_ASSERT(occ.size() == 3, "Buffer of size 4 with 2-byte wildcard should match at 0, 1, 2");
    TEST_ASSERT(occ[0] == 0 && occ[1] == 1 && occ[2] == 2, "Matched indices must be 0, 1, 2");
    return true;
}

// ---------------------------------------------------------------------------
// 2. Real Executable Binary Verification Tests
// ---------------------------------------------------------------------------

bool Test_Disk_LoadBinary(std::vector<uint8_t>& outBuffer) {
    const wchar_t* envPath = _wgetenv(L"CS3_EXE_PATH");
    if (!envPath || envPath[0] == L'\0') {
        std::cout << "       [SKIP] CS3_EXE_PATH not set; skipping on-disk ed8_3_PC.exe verification." << std::endl;
        return true;
    }
    auto fileOpt = ProcessScanner::ReadFileToBuffer(std::wstring(envPath));
    TEST_ASSERT(fileOpt.has_value(), "Failed to read target game executable from CS3_EXE_PATH!");
    outBuffer = std::move(*fileOpt);
    TEST_ASSERT(outBuffer.size() > 10 * 1024 * 1024, "Game executable size must be greater than 10MB");
    std::cout << "       Loaded " << outBuffer.size() << " bytes from ed8_3_PC.exe" << std::endl;
    return true;
}

bool Test_Disk_PitchClamp(const std::vector<uint8_t>& buffer) {
    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_PITCH_CLAMP);
    TEST_ASSERT(occ.size() == 1, "Pitch Clamp pattern MUST match exactly 1 time (uniqueness violated)");
    TEST_ASSERT(occ[0] == OFFSET_PITCH_CLAMP, "Pitch Clamp file offset does not match expected 0x1FC12A");

    size_t single = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), SIG_PITCH_CLAMP);
    TEST_ASSERT(single == OFFSET_PITCH_CLAMP, "ScanPatternOnDisk result must match ScanAllOccurrencesOnDisk");

    uintptr_t calculatedVA = FileOffsetToVA(occ[0]);
    TEST_ASSERT(calculatedVA == VA_PITCH_CLAMP, "Calculated VA does not match expected 0x1401FCD2A");

    std::cout << "       [MATCH] Pitch Clamp: offset=0x" << std::hex << occ[0]
              << ", VA=0x" << calculatedVA << std::dec << " (100% unique)" << std::endl;
    return true;
}

bool Test_Disk_ZoomClamp(const std::vector<uint8_t>& buffer) {
    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_ZOOM_CLAMP);
    TEST_ASSERT(occ.size() == 1, "Zoom Clamp pattern MUST match exactly 1 time (uniqueness violated)");
    TEST_ASSERT(occ[0] == OFFSET_ZOOM_CLAMP, "Zoom Clamp file offset does not match expected 0x2DD75C");

    size_t single = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), SIG_ZOOM_CLAMP);
    TEST_ASSERT(single == OFFSET_ZOOM_CLAMP, "ScanPatternOnDisk result must match ScanAllOccurrencesOnDisk");

    uintptr_t calculatedVA = FileOffsetToVA(occ[0]);
    TEST_ASSERT(calculatedVA == VA_ZOOM_CLAMP, "Calculated VA does not match expected 0x1402DE35C");

    std::cout << "       [MATCH] Zoom Clamp: offset=0x" << std::hex << occ[0]
              << ", VA=0x" << calculatedVA << std::dec << " (100% unique)" << std::endl;
    return true;
}

bool Test_Disk_AltitudeWrite(const std::vector<uint8_t>& buffer) {
    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_ALTITUDE_WRITE);
    TEST_ASSERT(occ.size() == 1, "Altitude Write pattern MUST match exactly 1 time (uniqueness violated)");
    TEST_ASSERT(occ[0] == OFFSET_ALTITUDE_WRITE, "Altitude Write file offset does not match expected 0x2DD7BB");

    size_t single = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), SIG_ALTITUDE_WRITE);
    TEST_ASSERT(single == OFFSET_ALTITUDE_WRITE, "ScanPatternOnDisk result must match ScanAllOccurrencesOnDisk");

    uintptr_t calculatedVA = FileOffsetToVA(occ[0]);
    TEST_ASSERT(calculatedVA == VA_ALTITUDE_WRITE, "Calculated VA does not match expected 0x1402DE3BB");

    std::cout << "       [MATCH] Altitude Write: offset=0x" << std::hex << occ[0]
              << ", VA=0x" << calculatedVA << std::dec << " (100% unique)" << std::endl;
    return true;
}

bool Test_Disk_GameManagerResolver(const std::vector<uint8_t>& buffer) {
    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_GM_RESOLVER);
    TEST_ASSERT(occ.size() == 1, "GameManager Resolver pattern MUST match exactly 1 time (uniqueness violated)");
    TEST_ASSERT(occ[0] == OFFSET_GM_RESOLVER, "GameManager Resolver file offset does not match expected 0x2A7100");

    size_t single = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), SIG_GM_RESOLVER);
    TEST_ASSERT(single == OFFSET_GM_RESOLVER, "ScanPatternOnDisk result must match ScanAllOccurrencesOnDisk");

    uintptr_t calculatedVA = FileOffsetToVA(occ[0]);
    TEST_ASSERT(calculatedVA == VA_GM_RESOLVER, "Calculated VA does not match expected 0x1402A7D00");

    std::cout << "       [MATCH] GM Resolver: offset=0x" << std::hex << occ[0]
              << ", VA=0x" << calculatedVA << std::dec << " (100% unique)" << std::endl;
    return true;
}

bool Test_Disk_SecondarySignatures(const std::vector<uint8_t>& buffer) {
    // Indoor pitch clamp
    auto occIndoor = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_PITCH_CLAMP_INDOOR);
    TEST_ASSERT(occIndoor.size() == 1, "Indoor Pitch Clamp must match exactly 1 time");
    TEST_ASSERT(occIndoor[0] == OFFSET_PITCH_CLAMP_INDOOR, "Indoor Pitch Clamp offset mismatch");
    TEST_ASSERT(FileOffsetToVA(occIndoor[0]) == VA_PITCH_CLAMP_INDOOR, "Indoor Pitch Clamp VA mismatch");

    // Zoom distance write
    auto occZoomWrite = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_ZOOM_WRITE);
    TEST_ASSERT(occZoomWrite.size() == 1, "Zoom Distance Write must match exactly 1 time");
    TEST_ASSERT(occZoomWrite[0] == OFFSET_ZOOM_WRITE, "Zoom Distance Write offset mismatch");
    TEST_ASSERT(FileOffsetToVA(occZoomWrite[0]) == VA_ZOOM_WRITE, "Zoom Distance Write VA mismatch");

    std::cout << "       [MATCH] Secondary Signatures (Indoor Pitch & Zoom Write) matched 100% uniquely" << std::endl;
    return true;
}

bool Test_Performance_Benchmark(const std::vector<uint8_t>& buffer) {
    const auto start = std::chrono::high_resolution_clock::now();

    // Perform full-binary scan for all 4 primary signatures
    auto occ1 = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_PITCH_CLAMP);
    auto occ2 = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_ZOOM_CLAMP);
    auto occ3 = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_ALTITUDE_WRITE);
    auto occ4 = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), SIG_GM_RESOLVER);

    const auto end = std::chrono::high_resolution_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

    TEST_ASSERT(occ1.size() == 1 && occ2.size() == 1 && occ3.size() == 1 && occ4.size() == 1,
                "All signatures must match during benchmark");

    std::cout << "       Scanned 4 signatures across " << (buffer.size() / (1024.0 * 1024.0))
              << " MB in " << std::fixed << std::setprecision(2) << elapsedMs << " ms" << std::endl;
    TEST_ASSERT(elapsedMs < 500.0, "Scan must complete in under 500ms");
    return true;
}

bool Test_Synthetic_NotFound(const std::vector<uint8_t>& buffer) {
    const char dummyPat[] = "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE\x01\x02\x03\x04\x05";
    const char dummyMask[] = "xxxxxxxxxxxxx";

    size_t first = ProcessScanner::ScanPatternOnDisk(buffer.data(), buffer.size(), dummyPat, dummyMask);
    TEST_ASSERT(first == INVALID_OFFSET, "Non-existent pattern must return INVALID_OFFSET");

    auto occ = ProcessScanner::ScanAllOccurrencesOnDisk(buffer.data(), buffer.size(), dummyPat, dummyMask);
    TEST_ASSERT(occ.empty(), "Non-existent pattern must return 0 occurrences");
    return true;
}

// ---------------------------------------------------------------------------
// Main Test Runner
// ---------------------------------------------------------------------------

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << " Cold Steel III - ProcessScanner Unit Test & AOB Verification" << std::endl;
    std::cout << "============================================================" << std::endl;

    // Run synthetic unit tests
    RUN_TEST(Test_Synthetic_EmptyBuffer);
    RUN_TEST(Test_Synthetic_BufferSmallerThanPattern);
    RUN_TEST(Test_Synthetic_WildcardPattern);
    RUN_TEST(Test_Synthetic_NullBytePattern);
    RUN_TEST(Test_Synthetic_AllWildcards);

    // Run real game binary tests
    std::vector<uint8_t> gameBinary;
    RUN_TEST([&]() { return Test_Disk_LoadBinary(gameBinary); });

    if (!gameBinary.empty()) {
        RUN_TEST([&]() { return Test_Disk_PitchClamp(gameBinary); });
        RUN_TEST([&]() { return Test_Disk_ZoomClamp(gameBinary); });
        RUN_TEST([&]() { return Test_Disk_AltitudeWrite(gameBinary); });
        RUN_TEST([&]() { return Test_Disk_GameManagerResolver(gameBinary); });
        RUN_TEST([&]() { return Test_Disk_SecondarySignatures(gameBinary); });
        RUN_TEST([&]() { return Test_Performance_Benchmark(gameBinary); });
        RUN_TEST([&]() { return Test_Synthetic_NotFound(gameBinary); });
    }

    std::cout << "============================================================" << std::endl;
    std::cout << " Results: Passed: " << g_passedTests << " | Failed: " << g_failedTests << std::endl;
    std::cout << "============================================================" << std::endl;

    return (g_failedTests == 0) ? 0 : 1;
}
