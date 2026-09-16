# Working in this repo

POGO: replacement MIDI-controller firmware for the Line 6 POD Go, a JUCE app that
configures and installs it, an emulator, and shared C code between them. `README.md` is the front
page; `docs/` is the specification, one document per subject. **Read
[docs/00-start-here.md](docs/00-start-here.md) before touching the unit.**

## Where things are

| For | Go to |
|---|---|
| The safety rules and what is verified | [docs/00-start-here.md](docs/00-start-here.md) |
| The hardware: pins, memory, panel, display, USB | [docs/01-hardware.md](docs/01-hardware.md) |
| What firmware must do at startup and why | [docs/02-firmware-platform.md](docs/02-firmware-platform.md) |
| The SysEx configuration protocol | [docs/04-config-protocol.md](docs/04-config-protocol.md) |
| The configuration record and its byte layout | [docs/05-config-model.md](docs/05-config-model.md) |
| The eMMC layout, install, restore, recovery | [docs/06-persistence.md](docs/06-persistence.md) |
| Building, testing, loading | [docs/07-building.md](docs/07-building.md) |
| How the app and the emulator are put together | [docs/08-emulator-and-app.md](docs/08-emulator-and-app.md) |
| Failure modes that do not look like their cause | [docs/09-hazards.md](docs/09-hazards.md) |

Behaviour lives in `shared/` and runs in both the firmware and the app. `firmware/src/main.c` is
only the board side. `app/Source/EmulatorLink.cpp` is only the window side. If a change to what
the device *does* is not in `shared/`, it is in the wrong place.

## Running anything

There is **no `python` on `PATH`** on the development machine. Use:

```bash
PY="/c/Users/chris/AppData/Local/Microsoft/WindowsApps/python.exe"
$PY firmware/build.py
```

Never `python - <<'EOF'`. Put scripts in a file.

- Everything: `$PY build.py` — host tests, then firmware (also copied into `app/assets/firmware/`),
  then the app; `$PY build.py firmware` or `$PY build.py app` for one part. It loads the Visual
  Studio environment itself.
- Firmware: `$PY firmware/build.py` (arm-none-eabi-gcc 14, native or WSL).
- Host tests: `cmake -S . -B build-host -DMIDI_CONTROL_BUILD_APP=OFF && cmake --build build-host && ctest --test-dir build-host --output-on-failure`.
  On Windows run that from a Visual Studio developer shell, or with `-G "Visual Studio 18 2026"`.
- App: add `-DMIDI_CONTROL_JUCE_DIR=<JUCE checkout>` or let CMake fetch JUCE 8.0.14.
- Device: `$PY tools/midi_control_usb.py ...` — see [docs/07-building.md](docs/07-building.md).

## Rules

- **A change to shared/ needs a test in `tests/test_shared.c`**, and the tests must pass before the
  firmware is rebuilt. The tests are the only thing that runs the device logic without a unit.
- **A change to the configuration byte layout is a new `MIDI_CONTROL_CONFIG_VERSION`**, with a reader for
  the old one or an explicit "defaults are loaded" note in docs/05.
- **The eMMC and the QSPI NOR are the only things a power cycle does not restore.** Nothing writes
  the NOR. The eMMC is written by exactly two code paths: `storage.c` (the config slots) and the
  restore command in `main.c` (sectors 0..1364). Do not add a third without a verified backup path,
  a read-back, and a docs/06 update.
- **A firmware that has only run in the emulator has not run.** The first load of any new build goes
  over DFU (RAM only). Install to the eMMC only after the DFU-loaded build has been seen working.
- Anything that reboots or writes the unit needs the user at the desk; `--i-mean-it` on the Python
  tool, the confirmation dialogs in the app. **Ask before running one.**
- Turn the volume down before loading a build that touches audio. This one does not; keep the habit.
- Don't commit unless asked.
- Don't commit backups, captures, or manufacturer-provided binaries or content.

## Conventions

- Specification tone in `docs/`: present tense, declarative, cite the address or the measurement.
  Say whether a figure was measured, independently verified, or derived.
- Firmware C: no libc beyond the shim, no floating point, every wait bounded, log the name of a
  register before reading it. `-Wall -Wextra -Werror`.
- Shared C: C11, no allocation, no I/O, no `printf`; compiles under MSVC `/W4` and GCC.
- Tools print `<unknown>` rather than guess.
- No brand in the code. Identifiers, files, targets and data formats use the generic
  `midi_control` / `midi-control`. The name people see, POGO, is written once for C
  (`MIDI_CONTROL_PRODUCT_NAME` in `shared/include/midi_control/midi_control_version.h`) and
  once in `app/CMakeLists.txt`; everything else refers to those.
