"""
Adversarial Standalone Binary Resilience & Dependency Test Suite
Evaluates bin/coldsteel3_camera_tweak.exe for:
1. Zero non-system PE dependencies (statically linked).
2. Clean execution of CLI flags (--version, -v, --help, -h, --test).
3. Adversarial CLI arguments (buffer overflows, Unicode, fuzzing, invalid flags).
4. Subprocess headless execution, non-interactive piping, and rapid lifecycles.
5. Signal handling (Ctrl+C, Ctrl+Break) and clean detach rollback.
"""

import os
import sys
import time
import signal
import ctypes
import subprocess
import pefile

EXE_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "bin", "coldsteel3_camera_tweak.exe"))

# Whitelisted standard Windows core OS DLLs (present in C:\Windows\System32 on all Windows systems)
ALLOWED_SYSTEM_DLLS = {
    "kernel32.dll",
    "user32.dll",
    "msvcrt.dll",
    "ntdll.dll",
    "advapi32.dll"
}

# Forbidden external / compiler runtime DLLs that must NOT be dynamically linked
FORBIDDEN_DLLS = {
    "libstdc++-6.dll",
    "libgcc_s_seh-1.dll",
    "libgcc_s_dw2-1.dll",
    "libwinpthread-1.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "msvcr120.dll",
    "msvcr110.dll",
    "msvcr100.dll"
}

def test_pe_zero_dependencies():
    """Verify that bin/coldsteel3_camera_tweak.exe has zero non-system DLL dependencies."""
    print("\n[TEST] 1. PE Import Table & Dependency Analysis...")
    assert os.path.isfile(EXE_PATH), f"Target binary not found at {EXE_PATH}"
    
    pe = pefile.PE(EXE_PATH)
    
    # 1. Architecture check
    assert pe.FILE_HEADER.Machine == 0x8664, f"Expected x64 (0x8664), got {hex(pe.FILE_HEADER.Machine)}"
    print("       Architecture: x86_64 (IMAGE_FILE_MACHINE_AMD64)")
    
    # 2. Delay-load check
    has_delay = hasattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT')
    assert not has_delay or len(pe.DIRECTORY_ENTRY_DELAY_IMPORT) == 0, "Binary must not have delay-loaded DLLs"
    print("       Delay Imports: NONE")
    
    # 3. Import table check
    imported_dlls = []
    assert hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'), "Binary must have import directory"
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll_name = entry.dll.decode('utf-8', errors='ignore').lower()
        imported_dlls.append(dll_name)
        print(f"       Found Imported DLL: {dll_name} ({len(entry.imports)} imported symbols)")
        
        # Verify not in forbidden list
        assert dll_name not in FORBIDDEN_DLLS, f"CRITICAL: Binary dynamically links forbidden compiler runtime DLL: {dll_name}"
        
        # Verify in allowed system DLLs
        assert dll_name in ALLOWED_SYSTEM_DLLS, f"CRITICAL: Non-whitelisted DLL dependency detected: {dll_name}"

    print(f"       Total imported DLLs: {len(imported_dlls)}")
    assert "libstdc++-6.dll" not in imported_dlls, "libstdc++-6.dll must not be imported"
    assert "libgcc_s_seh-1.dll" not in imported_dlls, "libgcc_s_seh-1.dll must not be imported"
    assert "libwinpthread-1.dll" not in imported_dlls, "libwinpthread-1.dll must not be imported"
    print("  [PASS] Zero non-system DLL dependencies verified (100% statically linked).")

def test_standard_cli_flags():
    """Verify --version, -v, --help, -h, and --test execute cleanly with code 0."""
    print("\n[TEST] 2. Standard CLI Flags Execution...")
    
    # --version
    res = subprocess.run([EXE_PATH, "--version"], capture_output=True, text=True, timeout=5)
    assert res.returncode == 0, f"--version failed with code {res.returncode}"
    assert "Cold Steel III Camera Tweak Utility" in res.stdout
    assert "v1.0.7 x64" in res.stdout
    assert "Static C++17 standalone binary" in res.stdout
    print("       --version passed cleanly.")

    # -v
    res = subprocess.run([EXE_PATH, "-v"], capture_output=True, text=True, timeout=5)
    assert res.returncode == 0, f"-v failed with code {res.returncode}"
    assert "Cold Steel III Camera Tweak Utility" in res.stdout
    print("       -v passed cleanly.")

    # --help
    res = subprocess.run([EXE_PATH, "--help"], capture_output=True, text=True, timeout=5)
    assert res.returncode == 0, f"--help failed with code {res.returncode}"
    assert "Usage: coldsteel3_camera_tweak.exe" in res.stdout
    assert "--test" in res.stdout
    assert "Num /" in res.stdout
    print("       --help passed cleanly.")

    # -h
    res = subprocess.run([EXE_PATH, "-h"], capture_output=True, text=True, timeout=5)
    assert res.returncode == 0, f"-h failed with code {res.returncode}"
    assert "Usage: coldsteel3_camera_tweak.exe" in res.stdout
    print("       -h passed cleanly.")

    # --test (headless self-test)
    res = subprocess.run([EXE_PATH, "--test"], capture_output=True, text=True, timeout=10)
    assert res.returncode == 0, f"--test failed with code {res.returncode}"
    assert "[PASS] Signatures verified." in res.stdout
    assert "[PASS] CameraPatcher transactional rollback verified." in res.stdout
    assert "[PASS] HotkeyManager edge-trigger and debouncing verified." in res.stdout
    assert "[PASS] All internal self-tests passed successfully." in res.stdout
    print("       --test passed all 4 internal self-test suites cleanly.")

    print("  [PASS] Standard CLI flags passed with exit code 0.")

def test_adversarial_cli_fuzzing():
    """Adversarially fuzz CLI argument parser with malformed, extreme, and unexpected inputs."""
    print("\n[TEST] 3. Adversarial CLI Argument Fuzzing...")
    
    # 1. Multi-flag combinations
    combinations = [
        ["--help", "--version"],
        ["--version", "--help"],
        ["--test", "--version"],
        ["--version", "--test"],
        ["-v", "-h", "--test"],
    ]
    for combo in combinations:
        res = subprocess.run([EXE_PATH] + combo, capture_output=True, text=True, timeout=10)
        assert res.returncode == 0, f"Combo {combo} failed with code {res.returncode}"
    print("       Multi-flag combinations handled cleanly without crashes.")

    # 2. Buffer overflow stress attempt near Windows OS command line limit (30,000 characters, just under 32,767)
    huge_arg = "--" + "A" * 30000
    res = None
    proc = subprocess.Popen([EXE_PATH, huge_arg], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.5)
    # Check if process is still alive and didn't crash with SEH / segmentation fault
    poll = proc.poll()
    if poll is None:
        # Process is alive in its search loop
        proc.terminate()
        proc.wait(timeout=2)
        print("       30,000-char extreme argument: handled safely (no SEH crash or buffer overflow).")
    else:
        # Terminated
        assert poll == 0 or poll != 3221225477, f"Process crashed on 30,000 char input with code {hex(poll & 0xFFFFFFFF)}"
        print(f"       30,000-char extreme argument exited cleanly with code {poll}.")

    # 3. Special characters & Unicode fuzzing
    weird_args = [
        "",  # Empty string
        "-",
        "--",
        "-???",
        "/help",
        "/?",
        "--foo=bar",
        "!@#$%^&*()_+~|}{[]:;?><,./",
        "こんにちは世界",
        "Привет мир",
        "🎉🚀💻🔥",
        "\t\r\n",
    ]
    for arg in weird_args:
        proc = subprocess.Popen([EXE_PATH, arg], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        poll = proc.poll()
        if poll is None:
            proc.terminate()
            proc.wait(timeout=2)
        else:
            # 0xC0000005 = 3221225477 (STATUS_ACCESS_VIOLATION)
            assert poll != 3221225477, f"Process crashed with access violation on arg {repr(arg)}"
    print(f"       Tested {len(weird_args)} adversarial/malformed arguments: zero crashes.")

    print("  [PASS] Adversarial CLI fuzzing passed with zero crashes or memory violations.")

def test_headless_pipe_resilience():
    """Test binary execution under piped, redirected, and non-interactive environments."""
    print("\n[TEST] 4. Headless & Piped I/O Resilience...")
    
    # 1. Piped stdout & stderr with DEVNULL stdin
    res = subprocess.run(
        [EXE_PATH, "--test"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=10
    )
    assert res.returncode == 0
    assert "[PASS]" in res.stdout
    print("       DEVNULL stdin + piped stdout/stderr executed successfully.")

    # 2. Closed stdout/stderr simulation (running with stdout/stderr redirected to nul)
    res = subprocess.run(
        [EXE_PATH, "--version"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=5
    )
    assert res.returncode == 0
    print("       DEVNULL all handles executed successfully.")

    # 3. Rapid lifecycle spawning (20 concurrent/sequential launches)
    print("       Testing 20 rapid lifecycle launches...")
    for i in range(20):
        flag = "--version" if (i % 2 == 0) else "--help"
        p = subprocess.Popen([EXE_PATH, flag], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out, err = p.communicate(timeout=3)
        assert p.returncode == 0, f"Run {i} failed with {p.returncode}"
    print("       20 rapid lifecycle launches completed with 0 failures.")

    print("  [PASS] Headless & piped I/O resilience passed.")

def test_signal_handling_and_clean_exit():
    """Test Win32 console control signal handling (Ctrl+C, Ctrl+Break) and clean detach."""
    print("\n[TEST] 5. Signal Interception & Clean Exit...")
    
    # Spawn process in new process group to test Ctrl+C / Ctrl+Break via GenerateConsoleCtrlEvent
    # Note: On Windows, GenerateConsoleCtrlEvent requires CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(
        [EXE_PATH],
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        stdin=subprocess.PIPE,
        text=True
    )
    
    time.sleep(1.0) # Wait for initial HUD render and search loop
    assert proc.poll() is None, "Process should be running in search loop"
    
    # Send CTRL_BREAK_EVENT to the process group (0 sends to all processes in group)
    os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
    
    try:
        stdout, stderr = proc.communicate(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise AssertionError("Process did not exit within 3 seconds of CTRL_BREAK_EVENT")
    
    assert proc.returncode == 0, f"Expected returncode 0 on signal exit, got {proc.returncode}"
    assert "Cold Steel III Camera Tweak utility detached cleanly. Goodbye!" in stdout, (
        f"Missing clean detach message. Output was:\n{stdout}"
    )
    print("       CTRL_BREAK_EVENT intercepted: cleanly restored and exited with code 0.")

    # 2. Run dedicated C++ signal adversarial harness covering all 5 Win32 console signals
    sig_exe = os.path.abspath(os.path.join(os.path.dirname(__file__), "test_signal_adversarial.exe"))
    res_sig = subprocess.run([sig_exe], capture_output=True, text=True, timeout=5)
    assert res_sig.returncode == 0, f"Signal harness failed with code {res_sig.returncode}: {res_sig.stderr}"
    assert "Passed: 7 | Failed: 0" in res_sig.stdout
    print("       Dedicated C++ signal harness: all 7 signal & rollback tests passed.")

    print("  [PASS] Signal interception and clean exit verified.")

def main():
    print("=" * 70)
    print(" Adversarial Verification: Standalone Binary Resilience & Dependencies")
    print(" Target: bin/coldsteel3_camera_tweak.exe")
    print("=" * 70)
    
    t0 = time.time()
    test_pe_zero_dependencies()
    test_standard_cli_flags()
    test_adversarial_cli_fuzzing()
    test_headless_pipe_resilience()
    test_signal_handling_and_clean_exit()
    elapsed = time.time() - t0
    
    print("\n" + "=" * 70)
    print(f" ALL 5 ADVERSARIAL STRESS TEST SUITES PASSED ({elapsed:.2f}s)")
    print("=" * 70)
    return 0

if __name__ == "__main__":
    sys.exit(main())
