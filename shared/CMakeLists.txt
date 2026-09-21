# The code compiled into both the firmware and the app. Plain C11, no
# allocation, no I/O, no libc beyond memcpy/memset/memcmp/strlen/strncpy - the
# firmware provides those itself and links with -nostdlib.

add_library(midi_control_shared STATIC
    src/midi_control_analog.c
    src/midi_control_bootimage.c
    src/midi_control_config.c
    src/midi_control_controls.c
    src/midi_control_crc.c
    src/midi_control_device.c
    src/midi_control_engine.c
    src/midi_control_font.c
    src/midi_control_gfx.c
    src/midi_control_protocol.c
    src/midi_control_ui.c
)

target_include_directories(midi_control_shared PUBLIC include)

if(MSVC)
    target_compile_options(midi_control_shared PRIVATE /W4 /wd4204 /wd4221)
    target_compile_definitions(midi_control_shared PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(midi_control_shared PRIVATE -Wall -Wextra)
endif()
