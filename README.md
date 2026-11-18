# Cold Steel III Camera Tweak Utility (`v1.0.7 x64`)

A standalone C++17 x64 Win32 memory-scanner and camera patcher utility for *The Legend of Heroes: Trails of Cold Steel III* (`ed8_3_PC.exe` v1.0.7). It attaches to the running game process, locates camera clamp instructions via Array-of-Bytes (AOB) pattern scanning, unlocks pitch and zoom limits, and adjusts camera altitude in real time with transactional LIFO rollback safety.

## Features

- **AOB Memory & Disk Scanner**: Wildcard byte-pattern scanner (`src/scanner.cpp`) targeting `ed8_3_PC.exe` v1.0.7 `.text` code signatures.
- **360° Pitch & Zoom Distance Unlocks**: Removes vertical pitch clamps (including overhead and under-character angles with terrain-collision bypass) and uncapped field zoom limits (`src/camera_patcher.cpp`).
- **Real-Time Camera Adjustments**: Adjust camera height (altitude), zoom multiplier, and pitch with `Shift` (3.0x boost) and `Ctrl` (0.1x precision) modifiers.
- **Transactional LIFO Rollback & Signal Safety**: Records original bytes for every patch and restores them in reverse LIFO order on clean exit, target process termination (`WaitForSingleObject` liveness monitor), or Win32 console signals (`CTRL_C_EVENT`, `CTRL_CLOSE_EVENT`, `CTRL_BREAK_EVENT`, logoff, shutdown).
- **Zero Non-System Runtime Dependencies**: Compiles to a statically linked Windows x64 executable (`KERNEL32`, `USER32`, `msvcrt`).

## Default Hotkeys

| Action | Primary Key | Secondary Key | Notes |
|---|---|---|---|
| Toggle 360° Pitch Unlock | `F1` | `F5` | Unlocks overhead and under-shoes vertical pitch |
| Toggle Zoom Unlock | `F2` | `F6` | Uncaps maximum camera zoom distance |
| Camera Height (Altitude) Up | `PgUp` | `F7` | Raises camera altitude |
| Camera Height (Altitude) Down | `PgDn` | `F8` | Lowers camera altitude |
| Dynamic Zoom In | `[` | `=` | Decreases zoom multiplier |
| Dynamic Zoom Out | `]` | `-` | Increases zoom multiplier |
| Dynamic Pitch Up / Down | `F10` / `F11` | — | Adjusts pitch angle (avoids `F4` which closes the game) |
| Reset Camera Defaults | `F9` | `Home` | Restores default pitch, zoom, and height |
| Speed Modifiers (Hold) | `Shift` (3.0x) | `Ctrl` (0.1x) | Modifies continuous zoom, pitch, and altitude steps |
| Clean Detach & Exit | `F12` | `End` | Restores all patched bytes in LIFO order and exits |

## Building from Source

Requires **MinGW-w64** (`g++` with C++17 support on Windows x64):

```powershell
mkdir bin -Force
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -static src/scanner.cpp src/camera_patcher.cpp src/hotkey_manager.cpp src/main.cpp -o bin/coldsteel3_camera_tweak.exe -luser32
```

## Usage

1. Launch *The Legend of Heroes: Trails of Cold Steel III* (`ed8_3_PC.exe` v1.0.7).
2. Run `bin\coldsteel3_camera_tweak.exe` (or pass the path to `ed8_3_PC.exe` as the first argument).
3. Run as Administrator if the game is running with elevated privileges.

```powershell
.\bin\coldsteel3_camera_tweak.exe "<GAME_DIR>\bin\x64\ed8_3_PC.exe"
```

## Verification & Testing

### Built-in Self-Test

Runs signature validation, mock `CameraPatcher` transactional rollback, and `HotkeyManager` debounce tests without needing the game running:

```powershell
.\bin\coldsteel3_camera_tweak.exe --test
```

Optionally pass the path to `ed8_3_PC.exe` to also verify on-disk AOB signatures:

```powershell
.\bin\coldsteel3_camera_tweak.exe --test "<GAME_DIR>\bin\x64\ed8_3_PC.exe"
```

### Python E2E & Adversarial Test Suite

Requires Python 3 with `pytest`, `pefile`, and `capstone`:

```powershell
python tests/run_e2e_tests.py
```

By default, mock memory, boundary, rollback, and lifecycle simulation tests run offline. To also run on-disk binary AOB uniqueness and disassembly tests against your local game executable, set `CS3_EXE_PATH`:

```powershell
$env:CS3_EXE_PATH = "<GAME_DIR>\bin\x64\ed8_3_PC.exe"
python tests/run_e2e_tests.py
python tests/adversarial_aob_verifier.py
```

### C++ Unit & Adversarial Suites

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -static tests/test_patcher_unit.cpp src/camera_patcher.cpp src/scanner.cpp -o bin/test_patcher_unit.exe -luser32
g++ -std=c++17 -O2 -Wall -Wextra -static tests/test_scanner_adversarial.cpp src/scanner.cpp -o bin/test_scanner_adversarial.exe
g++ -std=c++17 -O2 -Wall -Wextra -static tests/test_scanner_unit.cpp src/scanner.cpp -o bin/test_scanner_unit.exe
.\bin\test_patcher_unit.exe
.\bin\test_scanner_adversarial.exe
.\bin\test_scanner_unit.exe
```

## Attribution & Disclaimer

- **Upstream Attribution**: The `FieldCamera` vertical pitch, zoom, and altitude sub-patch logic in `src/camera_patcher.cpp` is ported from **`ayuanX`**'s original v1.05 *Trails of Cold Steel III* camera offsets/sub-patches to v1.07.
- **Trademark Disclaimer**: *The Legend of Heroes: Trails of Cold Steel III* (`ed8_3_PC.exe`) is a trademark of **Nihon Falcom Corporation** (PC port by NIS America / PH3 GmbH). This project is an independent, non-commercial community utility and is not affiliated with or endorsed by Nihon Falcom Corporation or NIS America.

## License

Licensed under the [MIT License](LICENSE).
