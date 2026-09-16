# 07 — Building, testing, loading

---

## 1. Toolchains

| Part | Needs |
|---|---|
| `shared/` and `tests/` | CMake ≥ 3.22 and any C11 compiler (MSVC, GCC, Clang) |
| `firmware/` | `arm-none-eabi-gcc` (14.2 tested) on PATH, in Arm's default Windows install directory, or inside WSL — `build.py` finds whichever exists |
| `app/` | CMake, a C++17 compiler, JUCE 8.0.x. On Windows the Windows SDK provides `setupapi` and `winusb` |
| Loading | Python 3 for `tools/midi_control_usb.py` (Windows, ctypes, no packages), or the app |

On the development machine there is **no `python` on PATH**; use
`/c/Users/chris/AppData/Local/Microsoft/WindowsApps/python.exe` (`$PY` below). Never
`python - <<'EOF'`.

## 2. Commands

`build.py` builds in the order the rules need: the host tests, which must pass before the firmware
is rebuilt; the firmware, whose `firmware/build.py` also copies `midi-control.bin` into
`app/assets/firmware/`; then the app. On Windows it loads the Visual Studio C++ environment itself
(`vswhere`, then `vcvars64.bat`) when no compiler is on PATH. `build.bat` runs it from cmd or
PowerShell with the first Python 3 it finds (`py -3`, `python`, then the WindowsApps `python.exe`
alias) and passes the arguments on: `build`, `build firmware`, `build app`.

```bash
$PY build.py                             # host tests, firmware, app
$PY build.py firmware                    # host tests, firmware
$PY build.py app                         # app, with whatever app/assets/ holds
$PY build.py --juce C:/path/to/JUCE      # build-app's first configure; otherwise external/JUCE or a fetch
```

By hand:

```bash
# host library and tests
cmake -S . -B build-host -DMIDI_CONTROL_BUILD_APP=OFF
cmake --build build-host
ctest --test-dir build-host --output-on-failure

# firmware
$PY firmware/build.py            # -> firmware/build/midi-control.{bin,raw,elf,map}, midi-control-emmc.img,
                                 #    and midi-control.bin copied into app/assets/firmware/
$PY firmware/build.py --toolchain

# app
cmake -S . -B build-app -DMIDI_CONTROL_JUCE_DIR=C:/path/to/JUCE     # or omit: JUCE 8.0.14 is fetched
cmake --build build-app --target MidiControlApp --config Release
```

On Windows, run CMake by hand from a Visual Studio developer prompt (Ninja) or pass a Visual Studio
generator. The app build copies everything in `app/assets/` beside the executable, and again
whenever a file there changes, so `build-app/app/MidiControlApp_artefacts/Release/` holds `POGO.exe`
and `firmware/midi-control.bin`: the image the Firmware tab loads unless another is chosen.
`app/assets/firmware/` is build output and is not committed.

`tests/test_shared --build-emmc <raw-or-bin> <out>` writes the C image builder's output for
comparison with `tools/midi_control_boot_image.py build --emmc`; they are byte-identical on the real image.

## 3. Loading onto the unit

The factory system must be fully booted — its normal screen, then **20 seconds more** — before
`reboot-dfu`. Taken early, the loaded image inherits a half-initialised board and stalls in the EMC
stage (docs/09).

```bash
$PY tools/midi_control_usb.py info                         # passive
$PY tools/midi_control_usb.py reboot-dfu --i-mean-it       # factory system or POGO -> DFU (1FC9:000C)
$PY tools/midi_control_usb.py dfu-load firmware/build/midi-control.bin --i-mean-it
$PY tools/midi_control_usb.py log --replay                 # the boot log, over the vendor endpoint
$PY tools/midi_control_usb.py result --run 70              # storage status packet
$PY tools/midi_control_usb.py result --run 95              # ADC snapshot
$PY tools/midi_control_usb.py log --cc 74 --ccval 1        # LCD registers, on one handle
```

Persistent install and restore: docs/06 §3–4.

The MIDI side is any MIDI tool; the app's MIDI monitor is the easiest. `winmm` allows one process
per port — close other MIDI software first (docs/09).

## 4. Tools

| | |
|---|---|
| `tools/midi_control_usb.py` | the device over WinUSB: `info`, `probe`, `reboot-dfu`, `dfu-load`, `log`, `cc`, `result`, `dump-emmc`, `write-emmc`, `dump-nor` (needs a build with the SPIFI reader; this firmware has none). Writes need `--i-mean-it` |
| `tools/midi_control_boot_image.py` | `info <image>` parses and checks a bootloader image (with or without sector 0); `build <raw> <out> --emmc` wraps a raw image |
| `app` Firmware tab | the same, without Python |

## 5. Adding a feature, end to end

A new button action, say:

1. `shared/include/midi_control/midi_control_config.h`: add the enum value (at the end), document it.
2. `shared/src/midi_control_config.c`: its name; `sanitise` if it has a range.
3. `shared/src/midi_control_engine.c`: what it does on press / release, and when it counts as active.
4. `tests/test_shared.c`: a check that sends the right bytes. Run the tests.
5. `app/Source/ConfigJson.cpp`: its JSON identifier (appended). The editor picks the name up from
   `midi_control_button_action_name()` automatically.
6. `docs/05-config-model.md`: the table row.
7. Rebuild the firmware; it takes the new engine with no firmware change.
8. Try it in the emulator, then over DFU on the unit.
