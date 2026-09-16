#!/usr/bin/env python3
"""
Build the firmware and the desktop app, with the firmware put where the app
ships it, so the app's Firmware tab has it ready to load and install.

Steps, in this order:

    1. host tests   build-host; they must pass before the firmware is rebuilt
    2. firmware     firmware/build.py, which also copies midi-control.bin into
                    app/assets/firmware/
    3. app          build-app; everything in app/assets/ is copied beside the
                    executable, so the image the Firmware tab loads is the one
                    step 2 built

    python build.py                     # 1, 2 and 3
    python build.py firmware            # 1 and 2
    python build.py app                 # 3, with whatever app/assets/ holds
    python build.py --config Debug      # build type (default Release)
    python build.py --juce <checkout>   # JUCE for build-app's first configure;
                                        # otherwise external/JUCE, or fetched

On Windows, when no compiler is on PATH, the Visual Studio C++ environment is
loaded the way a developer prompt loads it (vswhere, then vcvars64.bat). Run
with the Python the machine actually has - on this machine that is not on PATH
(see CLAUDE.md).
"""

import argparse
import filecmp
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
HOST_BUILD = os.path.join(ROOT, "build-host")
APP_BUILD = os.path.join(ROOT, "build-app")
APP_TARGET = "MidiControlApp"
ASSET = os.path.join(ROOT, "app", "assets", "firmware", "midi-control.bin")
STEPS = ("firmware", "app")


def run(cmd, env=None):
    print("\n> " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=ROOT, env=env)
    if r.returncode != 0:
        raise SystemExit(f"failed ({r.returncode}): {' '.join(cmd)}")


def compiler_env():
    """The environment for CMake. On Windows with no compiler on PATH, the one a
    Visual Studio developer prompt would have."""
    env = dict(os.environ)
    if os.name != "nt" or shutil.which("cl"):
        return env

    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    install = ""
    if os.path.isfile(vswhere):
        install = subprocess.run(
            [vswhere, "-latest", "-prerelease", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True).stdout.strip().split("\n")[0].strip()
    vcvars = os.path.join(install, "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not install or not os.path.isfile(vcvars):
        print("compiler: no Visual Studio C++ tools found; leaving the choice to CMake")
        return env

    # /s takes the outer quotes off, so the path may hold any characters; /u
    # makes `set` write UTF-16, so every value survives the pipe.
    r = subprocess.run(f'cmd /d /u /s /c ""{vcvars}" >nul 2>&1 && set"', capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f"{vcvars} failed ({r.returncode})")
    for line in r.stdout.decode("utf-16-le", errors="replace").splitlines():
        key, sep, value = line.partition("=")
        if key and sep:
            env[key.upper()] = value
    print(f"compiler: {vcvars}")
    return env


def tool(name, env):
    found = shutil.which(name, path=env.get("PATH"))
    if not found:
        raise SystemExit(f"{name} is not on PATH")
    return found


def configure(build_dir, options, env):
    cmd = [tool("cmake", env), "-S", ROOT, "-B", build_dir] + options
    # An existing build directory keeps its generator. A new one gets Ninja when
    # there is one, as a developer prompt would.
    if not os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")) \
            and shutil.which("ninja", path=env.get("PATH")):
        cmd += ["-G", "Ninja"]
    run(cmd, env)


def host_tests(env, config):
    configure(HOST_BUILD, ["-DMIDI_CONTROL_BUILD_APP=OFF"], env)
    run([tool("cmake", env), "--build", HOST_BUILD, "--config", config], env)
    run([tool("ctest", env), "--test-dir", HOST_BUILD, "-C", config, "--output-on-failure"], env)


def firmware():
    run([sys.executable, os.path.join(ROOT, "firmware", "build.py")])


def app(env, config, juce):
    options = ["-DCMAKE_BUILD_TYPE=" + config]
    if juce:
        options.append("-DMIDI_CONTROL_JUCE_DIR=" + os.path.abspath(juce).replace("\\", "/"))
    elif not os.path.isfile(os.path.join(APP_BUILD, "CMakeCache.txt")) \
            and not os.path.isfile(os.path.join(ROOT, "external", "JUCE", "CMakeLists.txt")):
        print("\nJUCE: no --juce and no external/JUCE, so CMake fetches JUCE 8.0.14 from GitHub")
    configure(APP_BUILD, options, env)
    run([tool("cmake", env), "--build", APP_BUILD, "--target", APP_TARGET, "--config", config], env)

    products = os.path.join(APP_BUILD, "app", APP_TARGET + "_artefacts", config)
    shipped = os.path.join(products, "firmware", os.path.basename(ASSET))
    apps = sorted(n for n in os.listdir(products) if n.endswith((".exe", ".app"))) \
        if os.path.isdir(products) else []
    print()
    print(f"app       {os.path.join(products, apps[0]) if apps else products}")
    if not os.path.isfile(ASSET):
        print("firmware  none in app/assets/firmware/ - `build.py firmware` puts it there")
    elif os.path.isfile(shipped) and filecmp.cmp(ASSET, shipped, shallow=False):
        print(f"firmware  {shipped}")
    else:
        raise SystemExit(f"firmware  {shipped} is missing or is not the image in {ASSET}")


def main():
    ap = argparse.ArgumentParser(description="Build the firmware and the desktop app.")
    ap.add_argument("steps", nargs="*", metavar="step",
                    help="firmware and/or app; both when none is given")
    ap.add_argument("--config", default="Release",
                    help="build type: Release (default), Debug or RelWithDebInfo")
    ap.add_argument("--juce", metavar="DIR",
                    help="a JUCE checkout, for build-app's first configure")
    args = ap.parse_args()
    unknown = [s for s in args.steps if s not in STEPS]
    if unknown:
        ap.error(f"unknown step {unknown[0]!r}: choose from {', '.join(STEPS)}")
    steps = args.steps or list(STEPS)

    env = compiler_env()
    if "firmware" in steps:
        host_tests(env, args.config)
        firmware()
    if "app" in steps:
        app(env, args.config, args.juce)
    return 0


if __name__ == "__main__":
    sys.exit(main())
