#!/usr/bin/env python3
"""
Build the MIDI controller firmware image for the POD Go.

Produces, in firmware/build/:

    midi-control.elf        for the map and debugger symbols
    midi-control.raw        the flat image, link address 0x10000000
    midi-control.bin        the raw image with the 16-byte NXP DFU header -
                            load it with `midi_control_usb.py dfu-load` (RAM only)
    midi-control-emmc.img   the same image in the NOR bootloader's format
                            with the sector-0 descriptor, for
                            `midi_control_usb.py write-emmc <img> 0` (persistent)

and copies midi-control.bin into app/assets/firmware/, which the desktop app
ships beside its executable for its Firmware tab (app/CMakeLists.txt).

The sources are firmware/src plus everything in shared/src, compiled with the
same flags; shared/ is the code the management app's emulator also runs.

Needs arm-none-eabi-gcc: on PATH, in Arm's default install directory, or
inside WSL. Run with the Python the machine actually has - on this machine
that is not on PATH (see CLAUDE.md).

    python build.py               # build
    python build.py --toolchain   # just report which toolchain was found
"""

import argparse
import glob
import importlib.util
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "src")
SHARED = os.path.join(ROOT, "shared")
BUILD = os.path.join(HERE, "build")
NAME = "midi-control"

# Soft float on purpose: the LPC4350's FPU is disabled at reset and would need
# CPACR enabling before any FP instruction. Nothing here needs floating point.
CFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft",
    "-Os", "-g3",
    "-ffunction-sections", "-fdata-sections",
    "-fno-common", "-fno-builtin",
    "-Wall", "-Wextra", "-Werror",
    "-std=gnu11",
]

LDFLAGS = [
    "-nostartfiles", "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--no-warn-rwx-segments",
]

SOURCES = [
    "src/startup.c", "src/board.c", "src/emc.c", "src/emmc.c", "src/lcd.c",
    "src/log.c", "src/libc_shim.c", "src/result.c", "src/storage.c",
    "src/main.c", "src/usb_descriptors.c",
    # TinyUSB 0.21.0 (git submodule). Only the files this device needs, not
    # their build system; src/chip.h stands in for the LPCOpen dependency the
    # LPC18/43 port expects.
    "tinyusb/src/tusb.c",
    "tinyusb/src/common/tusb_fifo.c",
    "tinyusb/src/device/usbd.c",
    "tinyusb/src/class/midi/midi_device.c",
    "tinyusb/src/class/vendor/vendor_device.c",
    "tinyusb/src/portable/chipidea/ci_hs/dcd_ci_hs.c",
]

SHARED_SOURCES = sorted(
    os.path.relpath(p, HERE) for p in glob.glob(os.path.join(SHARED, "src", "*.c"))
)

INCLUDES = ["src", "tinyusb/src", os.path.relpath(os.path.join(SHARED, "include"), HERE)]


class Toolchain:
    """Runs the cross toolchain, natively or through WSL."""

    def __init__(self, kind, distro=None, bindir=None):
        self.kind = kind
        self.distro = distro
        self.bindir = bindir

    def path(self, p):
        if self.kind != "wsl":
            return p
        p = os.path.abspath(p).replace("\\", "/")
        if len(p) > 1 and p[1] == ":":
            p = "/mnt/" + p[0].lower() + p[2:]
        return p

    def run(self, tool, args, cwd=None):
        if self.kind == "native":
            exe = os.path.join(self.bindir, tool) if self.bindir else tool
            cmd = [exe] + args
        else:
            cmd = ["wsl.exe", "-d", self.distro, "--", tool] + args
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(" ".join(cmd) + "\n")
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit(f"{tool} failed ({r.returncode})")
        return r.stdout

    def version(self):
        return self.run("arm-none-eabi-gcc", ["--version"]).splitlines()[0]


def find_toolchain():
    if shutil.which("arm-none-eabi-gcc"):
        return Toolchain("native")

    patterns = [
        r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*\bin",
        r"C:\Program Files\Arm GNU Toolchain arm-none-eabi\*\bin",
    ]
    found = []
    for pat in patterns:
        for d in glob.glob(pat):
            if os.path.isfile(os.path.join(d, "arm-none-eabi-gcc.exe")):
                found.append(d)
    if found:
        found.sort()
        return Toolchain("native", bindir=found[-1])

    if shutil.which("wsl.exe") or shutil.which("wsl"):
        try:
            out = subprocess.run(["wsl.exe", "-l", "-q"], capture_output=True,
                                 text=True, timeout=30).stdout
            distros = [d.strip() for d in out.replace("\x00", "").splitlines() if d.strip()]
        except Exception:
            distros = []
        for d in distros:
            if d.lower().startswith("docker"):
                continue
            probe = subprocess.run(["wsl.exe", "-d", d, "--", "command", "-v",
                                    "arm-none-eabi-gcc"], capture_output=True, text=True)
            if probe.returncode == 0 and probe.stdout.strip():
                return Toolchain("wsl", d)

    raise SystemExit(
        "No arm-none-eabi-gcc found.\n"
        "  Windows: winget install Arm.GnuArmEmbeddedToolchain\n"
        "  WSL:     sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi\n"
    )


def build(tc):
    objdir = os.path.join(BUILD, "obj")
    os.makedirs(objdir, exist_ok=True)

    objs = []
    for src in SOURCES + SHARED_SOURCES:
        obj = os.path.join(objdir, src.replace("/", "_").replace("\\", "_")
                           .replace("..", "up").rsplit(".", 1)[0] + ".o")
        args = CFLAGS + ["-c", tc.path(os.path.join(HERE, src)), "-o", tc.path(obj)]
        for inc in INCLUDES:
            args += ["-I", tc.path(os.path.join(HERE, inc))]
        tc.run("arm-none-eabi-gcc", args)
        objs.append(obj)

    stem = os.path.join(BUILD, NAME)
    elf = stem + ".elf"
    tc.run("arm-none-eabi-gcc", CFLAGS + LDFLAGS + [
        "-T", tc.path(os.path.join(HERE, "midi_control.ld")),
        "-Wl,-Map=" + tc.path(stem + ".map"),
    ] + [tc.path(o) for o in objs] + ["-o", tc.path(elf)])

    raw = stem + ".raw"
    tc.run("arm-none-eabi-objcopy", ["-O", "binary", tc.path(elf), tc.path(raw)])
    size = tc.run("arm-none-eabi-size", [tc.path(elf)])

    body = open(raw, "rb").read()

    # A section placed in AHB SRAM without (NOLOAD) makes objcopy pad from
    # 0x10000000 to 0x20000000 and emit a 256 MB file. Catch that here.
    if len(body) > 128 * 1024:
        raise SystemExit(
            f"raw image is {len(body)} bytes, larger than the 128 KB of local SRAM.\n"
            "Almost certainly a section in AHB SRAM is missing (NOLOAD) or an AT>."
        )

    body += b"\x00" * ((-len(body)) % 512)
    blocks = len(body) // 512
    if blocks == 0 or blocks > 0xFFFF:
        raise SystemExit("image size out of range for the DFU header")

    # 16-byte NXP boot-image header: 1a 00, length in 512-byte blocks, zeros.
    img = struct.pack("<BBH", 0x1A, 0x00, blocks) + b"\x00" * 12 + body
    out = stem + ".bin"
    with open(out, "wb") as f:
        f.write(img)

    # The persistent image, through the same tool a person would use.
    spec = importlib.util.spec_from_file_location(
        "midi_control_boot_image", os.path.join(ROOT, "tools", "midi_control_boot_image.py"))
    pbi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pbi)
    emmc = stem + "-emmc.img"
    pbi.cmd_build(argparse.Namespace(raw=raw, out=emmc, dest="0x10000000",
                                     entry=None, handoff=None, emmc=True))
    sectors = os.path.getsize(emmc) // 512
    if sectors > 1300:
        raise SystemExit(f"eMMC image is {sectors} sectors and would overlap the "
                         "configuration slots at sector 1300")

    return out, img, size, emmc, sectors


def copy_to_app(image):
    """The desktop app ships app/assets/ beside its executable, and its Firmware
    tab looks there first: keep its copy the image just built."""
    dest = os.path.join(ROOT, "app", "assets", "firmware", os.path.basename(image))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(image, dest)
    return dest


def main():
    ap = argparse.ArgumentParser(description="Build the MIDI controller firmware.")
    ap.add_argument("--toolchain", action="store_true",
                    help="report which toolchain was found, and build nothing")
    args = ap.parse_args()

    tc = find_toolchain()
    where = (f"native [{tc.bindir}]" if tc.bindir else "native (PATH)") \
        if tc.kind == "native" else f"WSL [{tc.distro}]"
    print(f"toolchain: {where}\n           {tc.version()}")
    if args.toolchain:
        return 0

    out, img, size, emmc, sectors = build(tc)
    asset = copy_to_app(out)
    print()
    print(size.rstrip())
    print(f"{os.path.basename(out)}   {len(img)} bytes "
          f"({len(img) - 16} body + 16 header, {(len(img) - 16) // 512} blocks)")
    print(f"   {out}")
    print(f"{os.path.basename(emmc)}   {sectors} sectors (descriptor + image)")
    print(f"   {emmc}")
    print(f"{os.path.basename(asset)}   copied for the desktop app")
    print(f"   {asset}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
