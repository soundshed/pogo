cmake_minimum_required(VERSION 3.22)

project(MidiControl VERSION 0.2.0 LANGUAGES C CXX)

# The host-side build: the shared library, its tests, and the management app.
# The firmware is built separately by firmware/build.py with the ARM toolchain
# (docs/07-building.md); it compiles the same shared/ sources.

option(MIDI_CONTROL_BUILD_APP   "Build the JUCE management app (needs JUCE)" ON)
option(MIDI_CONTROL_BUILD_TESTS "Build and register the host tests"           ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(shared)

if(MIDI_CONTROL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MIDI_CONTROL_BUILD_APP)
    add_subdirectory(app)
endif()
