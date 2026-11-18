#!/usr/bin/env python3
"""
Adversarial AOB Collision & Binary Integrity Verification Harness
Role: challenger_m1_2 (Adversarial Binary Collision Specialist)
Target: ed8_3_PC.exe (The Legend of Heroes: Trails of Cold Steel III v1.0.7 x64)

Exhaustively verifies whole-file AOB uniqueness, section collision absence,
PE header mapping, and exact offset assertions.
"""

import os
import sys
import hashlib
import re
import pefile

BINARY_PATH = os.environ.get("CS3_EXE_PATH", "")

# Expected signatures and specifications
SIGNATURES = [
    {
        "name": "Pitch Clamp (Field)",
        "pattern": b"\xF3\x0F\x5D\xBB\x24\x01\x00\x00\xF3\x0F\x5F\xBB\x20\x01\x00\x00",
        "mask": "xxxxxxxxxxxxxxxx",
        "expected_offset": 0x1FC12A,
        "expected_va": 0x1401FCD2A,
        "length": 16,
        "regex": re.compile(re.escape(b"\xF3\x0F\x5D\xBB\x24\x01\x00\x00\xF3\x0F\x5F\xBB\x20\x01\x00\x00")),
    },
    {
        "name": "Zoom Clamp (Max Distance)",
        "pattern": b"\xF3\x45\x0F\x5D\x96\xC4\xA7\x03\x00",
        "mask": "xxxxxxxxx",
        "expected_offset": 0x2DD75C,
        "expected_va": 0x1402DE35C,
        "length": 9,
        "regex": re.compile(re.escape(b"\xF3\x45\x0F\x5D\x96\xC4\xA7\x03\x00")),
    },
    {
        "name": "Altitude Write (Eye Y)",
        "pattern": b"\xF3\x0F\x11\x86\x68\x03\x00\x00",
        "mask": "xxxxxxxx",
        "expected_offset": 0x2DD7BB,
        "expected_va": 0x1402DE3BB,
        "length": 8,
        "regex": re.compile(re.escape(b"\xF3\x0F\x11\x86\x68\x03\x00\x00")),
    },
    {
        "name": "GameManager Resolver",
        "pattern": b"\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x80\xE8\x1C\x00\x00\xC3",
        "mask": "xxx????xxxxxxxx",
        "expected_offset": 0x2A7100,
        "expected_va": 0x1402A7D00,
        "length": 15,
        "regex": re.compile(b"\x48\x8B\x05....\x48\x8B\x80\xE8\x1C\x00\x00\xC3", re.DOTALL),
    },
    # Secondary signatures
    {
        "name": "Indoor Pitch Clamp (Branch A)",
        "pattern": b"\xF3\x0F\x10\x05\x00\x00\x00\x00\xF3\x0F\x5D\x3D\x00\x00\x00\x00\xF3\x0F\x5F\xC7",
        "mask": "xxxx????xxxx????xxxx",
        "expected_offset": 0x1FC046,
        "expected_va": 0x1401FCC46,
        "length": 20,
        "regex": re.compile(b"\xF3\x0F\x10\x05....\xF3\x0F\x5D\x3D....\xF3\x0F\x5F\xC7", re.DOTALL),
    },
    {
        "name": "Zoom Distance Write",
        "pattern": b"\xF3\x44\x0F\x11\x96\x5C\x06\x00\x00",
        "mask": "xxxxxxxxx",
        "expected_offset": 0x2DD7F7,
        "expected_va": 0x1402DE3F7,
        "length": 9,
        "regex": re.compile(re.escape(b"\xF3\x44\x0F\x11\x96\x5C\x06\x00\x00")),
    },
]

def main():
    print("=" * 70)
    print("ADVERSARIAL AOB COLLISION & SECTION INTEGRITY AUDIT")
    print(f"Target Binary: {BINARY_PATH or '<CS3_EXE_PATH not set>'}")
    print("=" * 70)

    if not BINARY_PATH or not os.path.isfile(BINARY_PATH):
        print("[SKIP] CS3_EXE_PATH is not set or ed8_3_PC.exe was not found on disk.")
        print("       Set CS3_EXE_PATH to run whole-binary AOB collision verification.")
        sys.exit(0)

    file_size = os.path.getsize(BINARY_PATH)
    print(f"File Size: {file_size:,} bytes ({file_size / (1024 * 1024):.4f} MB)")

    with open(BINARY_PATH, "rb") as f:
        file_bytes = f.read()

    sha256 = hashlib.sha256(file_bytes).hexdigest()
    print(f"SHA-256: {sha256}")

    # PE Section mapping
    print("\n--- PE Section Table ---")
    pe = pefile.PE(BINARY_PATH, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    print(f"ImageBase: 0x{image_base:016X}")

    sections = []
    text_section = None
    for s in pe.sections:
        name = s.Name.decode("utf-8", errors="ignore").strip("\x00")
        raw_start = s.PointerToRawData
        raw_size = s.SizeOfRawData
        raw_end = raw_start + raw_size
        va_start = image_base + s.VirtualAddress
        va_size = s.Misc_VirtualSize
        va_end = va_start + va_size
        is_code = bool(s.Characteristics & 0x00000020) # IMAGE_SCN_CNT_CODE
        is_exec = bool(s.Characteristics & 0x20000000) # IMAGE_SCN_MEM_EXECUTE
        
        sec_info = {
            "name": name,
            "raw_start": raw_start,
            "raw_end": raw_end,
            "raw_size": raw_size,
            "va_start": va_start,
            "va_end": va_end,
            "va_size": va_size,
            "rva": s.VirtualAddress,
            "is_code": is_code,
            "is_exec": is_exec,
        }
        sections.append(sec_info)
        if name == ".text":
            text_section = sec_info
        print(f"Section {name:<8}: Raw [0x{raw_start:08X} - 0x{raw_end:08X}] ({raw_size:,} B) | VA [0x{va_start:012X} - 0x{va_end:012X}] | Exec={is_exec} Code={is_code}")

    if not text_section:
        print("[FAIL] Could not locate .text section in PE headers!")
        sys.exit(1)

    print(f"\n.text Raw Pointer: 0x{text_section['raw_start']:X}, RVA: 0x{text_section['rva']:X}")
    text_delta = text_section['rva'] - text_section['raw_start']
    print(f"Calculated text delta: 0x{text_delta:X} (expected 0xC00)")

    failures = 0

    print("\n--- Exhaustive Whole-Binary AOB Scans ---")
    for sig in SIGNATURES:
        name = sig["name"]
        rgx = sig["regex"]
        exp_off = sig["expected_offset"]
        exp_va = sig["expected_va"]
        length = sig["length"]

        # 1. Whole binary scan
        matches = [m.start() for m in rgx.finditer(file_bytes)]
        match_count = len(matches)

        print(f"\n[SIGNATURE] {name}")
        print(f"  Pattern Len : {length} bytes")
        print(f"  Mask        : {sig['mask']}")
        print(f"  Expected Off: 0x{exp_off:08X} (VA: 0x{exp_va:012X})")
        print(f"  Matches Found: {match_count}")

        if match_count == 1:
            found_off = matches[0]
            print(f"  Found Offset: 0x{found_off:08X}")
            if found_off == exp_off:
                print(f"  [PASS] Exactly 1 occurrence at EXACT target offset 0x{found_off:08X}")
            else:
                print(f"  [FAIL] Occurrence found at 0x{found_off:08X}, expected 0x{exp_off:08X}")
                failures += 1
        elif match_count == 0:
            print(f"  [FAIL] ZERO occurrences found in entire binary!")
            failures += 1
        else:
            print(f"  [FAIL] AMBIGUOUS / COLLISION! Found {match_count} occurrences: {[hex(m) for m in matches]}")
            failures += 1

        # 2. Section mapping and section collision analysis
        print("  Section Analysis for Matches:")
        for off in matches:
            matched_sec = None
            for s in sections:
                if s["raw_start"] <= off < s["raw_end"]:
                    matched_sec = s
                    break
            if matched_sec:
                offset_in_sec = off - matched_sec["raw_start"]
                calc_va = image_base + matched_sec["rva"] + offset_in_sec
                print(f"    Match at 0x{off:08X} is inside section '{matched_sec['name']}' (Exec={matched_sec['is_exec']}, Code={matched_sec['is_code']})")
                print(f"    Computed VA: 0x{calc_va:012X} vs Expected VA: 0x{exp_va:012X}")
                if matched_sec["name"] != ".text":
                    print(f"    [FAIL] CRITICAL COLLISION: Pattern matched outside .text in non-code section '{matched_sec['name']}'!")
                    failures += 1
                if calc_va != exp_va:
                    print(f"    [FAIL] VA mismatch: computed 0x{calc_va:012X} != expected 0x{exp_va:012X}")
                    failures += 1
            else:
                print(f"    [FAIL] Match at 0x{off:08X} is in PE headers or unmapped space!")
                failures += 1

        # 3. Non-code section scan explicitly
        for s in sections:
            if s["name"] != ".text":
                sec_data = file_bytes[s["raw_start"]:s["raw_end"]]
                non_code_matches = list(rgx.finditer(sec_data))
                if non_code_matches:
                    print(f"    [FAIL] Non-code section '{s['name']}' contains {len(non_code_matches)} matches!")
                    failures += 1
                else:
                    # Verified 0 matches in this non-code section
                    pass

        # 4. Context bytes display
        if matches:
            off = matches[0]
            pre = file_bytes[max(0, off - 8):off]
            matched_data = file_bytes[off:off + length]
            post = file_bytes[off + length:off + length + 8]
            print(f"  Bytes Before (-8) : {pre.hex().upper()}")
            print(f"  Matched Bytes     : {matched_data.hex().upper()}")
            print(f"  Bytes After (+8)  : {post.hex().upper()}")

    print("\n--- Adversarial Sub-Pattern & Stress Testing ---")
    # For each core signature, test resilience to truncation and sub-patterns
    # To demonstrate why full-length patterns were chosen and confirm safety margins
    for sig in SIGNATURES[:4]:
        name = sig["name"]
        pat = sig["pattern"]
        mask = sig["mask"]
        rgx = sig["regex"]
        print(f"\nStress Testing Signature: {name}")

        # Test sub-lengths: does cutting 1 byte or 2 bytes cause collisions?
        if "???" not in mask:
            # Exact pattern: check progressive prefixes
            for trim in [1, 2, 3, 4]:
                sub_len = len(pat) - trim
                if sub_len >= 4:
                    sub_pat = pat[:sub_len]
                    sub_matches = list(re.compile(re.escape(sub_pat)).finditer(file_bytes))
                    print(f"  Prefix length {sub_len} bytes (-{trim}): {len(sub_matches)} match(es)")

        # Verify boundary safety: does match + length overflow section?
        off = sig["expected_offset"]
        if text_section["raw_start"] <= off and (off + sig["length"]) <= text_section["raw_end"]:
            print(f"  [PASS] Section boundary safety: [0x{off:08X} .. 0x{off + sig['length']:08X}] is well within .text [0x{text_section['raw_start']:08X} .. 0x{text_section['raw_end']:08X}]")
        else:
            print(f"  [FAIL] Section boundary violation!")
            failures += 1

    print("\n" + "=" * 70)
    if failures == 0:
        print("FINAL VERDICT: ALL TESTS PASSED - ZERO COLLISIONS DETECTED")
        print("Verdict: APPROVE")
    else:
        print(f"FINAL VERDICT: {failures} FAILURES DETECTED")
        print("Verdict: REQUEST_CHANGES")
    print("=" * 70)

    sys.exit(0 if failures == 0 else 1)

if __name__ == "__main__":
    main()
