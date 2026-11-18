#!/usr/bin/env python3
"""
CLI Test Runner: Cold Steel III Camera Tweak 4-Tier E2E Test Suite
Executes all test tiers, displays comprehensive metrics and AOB resolution tables,
and returns exit code 0 on 100% test pass.
"""
import os
import sys
import time
import argparse
import pytest

# Ensure tests/ directory is in sys.path
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TESTS_DIR)
if TESTS_DIR not in sys.path:
    sys.path.insert(0, TESTS_DIR)

from test_helpers import (
    DEFAULT_EXE_PATH,
    AOB_SIGNATURES,
    parse_pe_headers,
    assert_unique_match,
)


def print_banner():
    target_display = DEFAULT_EXE_PATH if DEFAULT_EXE_PATH else "<CS3_EXE_PATH not set; disk-binary checks will skip>"
    banner = f"""
================================================================================
  Cold Steel III (v1.0.7 x64) Camera Tweak - 4-Tier E2E Verification Suite
  Target Binary: {target_display}
================================================================================
"""
    print(banner)


def verify_aob_summary_table():
    """Validates the 5 mandated signatures on disk and prints an ASCII status table."""
    print("[*] Validating Mandated AOB Signatures against Disk Binary...")
    if not DEFAULT_EXE_PATH or not os.path.isfile(DEFAULT_EXE_PATH):
        print("[*] CS3_EXE_PATH not set or target binary not found on disk; skipping pre-flight disk-binary AOB table.")
        print()
        return True

    with open(DEFAULT_EXE_PATH, "rb") as f:
        data = f.read()

    pe = parse_pe_headers(data)
    print(f"    - Binary Size : {len(data):,} bytes")
    print(f"    - ImageBase   : {hex(pe.image_base)}")
    print(f"    - .text Range : {hex(pe.sections['.text'].raw_offset)} - {hex(pe.sections['.text'].raw_offset + pe.sections['.text'].raw_size)}")
    print()

    header = f"{'Feature':<24} | {'Expected Offset':<16} | {'Resolved VA':<14} | {'Match Count':<12} | {'Status'}"
    divider = "-" * len(header)
    print(header)
    print(divider)

    all_ok = True
    for key, sig in AOB_SIGNATURES.items():
        try:
            offset = assert_unique_match(
                data,
                sig["pattern"],
                expected_offset=sig["expected_offset"],
                expected_va=sig["expected_va"],
                pe_info=pe
            )
            va = pe.file_offset_to_va(offset)
            status = "PASS (100% Unique)"
            print(f"{sig['name']:<24} | {hex(offset):<16} | {hex(va):<14} | {'1 match':<12} | {status}")
        except Exception as e:
            all_ok = False
            print(f"{sig['name']:<24} | {'ERROR':<16} | {'ERROR':<14} | {'N/A':<12} | FAIL: {e}")

    print(divider)
    print()
    return all_ok


class PytestMetricsPlugin:
    """Collects per-test results for summary reporting."""
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.records = []

    def pytest_runtest_logreport(self, report):
        if report.when == "call":
            self.records.append(report)
            if report.passed:
                self.passed += 1
            elif report.failed:
                self.failed += 1
            elif report.skipped:
                self.skipped += 1


def run_tests(args):
    print_banner()

    # Step 1: Pre-flight AOB verification summary
    aob_ok = verify_aob_summary_table()
    if not aob_ok:
        print("[!] Pre-flight AOB signature verification failed.")
        return 1

    # Step 2: Pytest execution across test tiers
    print("[*] Launching 4-Tier Test Framework...")
    test_files = [
        os.path.join(TESTS_DIR, "test_aob_signatures.py"),
        os.path.join(TESTS_DIR, "test_boundary_conditions.py"),
        os.path.join(TESTS_DIR, "test_detachment_rollback.py"),
        os.path.join(TESTS_DIR, "test_lifecycle_simulation.py"),
    ]

    pytest_args = ["-v", "--tb=short"]
    if args.tier:
        tier_map = {
            1: "test_aob_signatures.py",
            2: "test_boundary_conditions.py",
            3: "test_detachment_rollback.py",
            4: "test_lifecycle_simulation.py",
        }
        target = tier_map.get(args.tier)
        if not target:
            print(f"[!] Invalid tier {args.tier}. Valid options: 1, 2, 3, 4.")
            return 1
        pytest_args.append(os.path.join(TESTS_DIR, target))
    else:
        pytest_args.extend(test_files)

    plugin = PytestMetricsPlugin()
    t_start = time.perf_counter()
    exit_code = pytest.main(pytest_args, plugins=[plugin])
    elapsed_sec = time.perf_counter() - t_start

    # Step 3: Print structured test metrics
    total = plugin.passed + plugin.failed + plugin.skipped
    print()
    print("=" * 80)
    print("  TEST EXECUTION METRICS SUMMARY")
    print("=" * 80)
    print(f"  * Total Tests Run : {total}")
    print(f"  * Passed          : {plugin.passed}")
    print(f"  * Failed          : {plugin.failed}")
    print(f"  * Skipped         : {plugin.skipped}")
    print(f"  * Pass Rate       : {(plugin.passed / total * 100) if total else 0:.1f}%")
    print(f"  * Execution Time  : {elapsed_sec * 1000:.2f} ms ({elapsed_sec:.2f} seconds)")
    print("=" * 80)

    if exit_code == 0 and plugin.failed == 0 and plugin.passed > 0:
        print("\n>>> ALL TESTS PASSED SUCCESSFULLY (Exit Code 0) <<<\n")
        return 0
    else:
        print(f"\n>>> TEST SUITE FAILED (Exit Code {exit_code}) <<<\n")
        return exit_code or 1


def main():
    parser = argparse.ArgumentParser(description="Cold Steel III Camera Tweak E2E Test Runner")
    parser.add_argument("--tier", type=int, choices=[1, 2, 3, 4], help="Run only a specific test tier (1-4)")
    args = parser.parse_args()
    sys.exit(run_tests(args))


if __name__ == "__main__":
    main()
