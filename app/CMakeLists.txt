# The management app: a JUCE standalone. Windows first; the MIDI side is
# portable, the firmware-installer side (WinUSB) is Windows-only for now.
#
# JUCE is found in this order:
#   1. -DMIDI_CONTROL_JUCE_DIR=<path to a JUCE checkout>
#   2. external/JUCE (a checkout placed there by hand or as a submodule)
#   3. fetched from GitHub at configure time (tag 8.0.14, shallow)

if(MIDI_CONTROL_JUCE_DIR)
    add_subdirectory(${MIDI_CONTROL_JUCE_DIR} ${CMAKE_BINARY_DIR}/juce-build)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../external/JUCE/CMakeLists.txt)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/JUCE ${CMAKE_BINARY_DIR}/juce-build)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.14
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The name people see: the window title and the executable. The C code has its
# own copy for the firmware's USB strings, MIDI_CONTROL_PRODUCT_NAME in
# shared/include/midi_control/midi_control_version.h; keep the two equal.
set(MIDI_CONTROL_PRODUCT_NAME "POGO")

# juceaide scales the logo PNG into the executable's icon at 16, 32, 48 and
# 256 px when CMake configures. The window and taskbar take it from there.
set(app_icon_big "${CMAKE_CURRENT_SOURCE_DIR}/icons/midi-control-logo-v1.png")
set(app_icon_small "${app_icon_big}")

juce_add_gui_app(MidiControlApp
    PRODUCT_NAME "${MIDI_CONTROL_PRODUCT_NAME}"
    COMPANY_NAME "${MIDI_CONTROL_PRODUCT_NAME}"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${app_icon_big}"
    ICON_SMALL "${app_icon_small}")

# JUCE embeds icon.ico through a generated resource script whose text is the
# same whatever the drawing, so on its own a changed drawing never reaches the
# executable. A changed image re-runs the configure, which remakes icon.ico,
# and a remade icon.ico rebuilds the resources.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${app_icon_big}" "${app_icon_small}")
get_target_property(app_generated MidiControlApp JUCE_GENERATED_SOURCES_DIRECTORY)
set_source_files_properties("${app_generated}/MidiControlApp_resources.rc"
    PROPERTIES OBJECT_DEPENDS "${app_generated}/icon.ico")

target_sources(MidiControlApp PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/ConfigJson.cpp
    Source/ConfigSession.cpp
    Source/EmulatorLink.cpp
    Source/MidiDeviceLink.cpp
    Source/ui/ControlsPanel.cpp
    Source/ui/EmulatorStrip.cpp
    Source/ui/FirmwarePanel.cpp
    Source/ui/HardwareView.cpp
    Source/ui/Inspector.cpp
    Source/ui/MidiMonitor.cpp
    Source/ui/ScreenPreview.cpp
    Source/ui/Theme.cpp
)

if(WIN32)
    target_sources(MidiControlApp PRIVATE
        Source/usb/WinUsbDevice.cpp
        Source/usb/MidiControlUsb.cpp)
    target_link_libraries(MidiControlApp PRIVATE setupapi winusb)
endif()

target_include_directories(MidiControlApp PRIVATE Source)

target_compile_definitions(MidiControlApp PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MidiControlApp,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MidiControlApp,JUCE_VERSION>")

target_link_libraries(MidiControlApp
    PRIVATE
        midi_control_shared
        juce::juce_gui_extra
        juce::juce_audio_devices
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Everything in assets/ ships beside the executable, copied again whenever it
# changes, without relinking. firmware/build.py puts its image there as
# assets/firmware/midi-control.bin, which is where the Firmware tab looks first.
# The directory is build output: it is absent until the firmware is built.
get_target_property(app_products MidiControlApp RUNTIME_OUTPUT_DIRECTORY)
file(GLOB_RECURSE app_assets CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
set(app_asset_copies)
foreach(asset IN LISTS app_assets)
    get_filename_component(asset_dir "${asset}" DIRECTORY)
    add_custom_command(OUTPUT "${app_products}/${asset}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${app_products}/${asset_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/assets/${asset}" "${app_products}/${asset}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/${asset}"
        VERBATIM)
    list(APPEND app_asset_copies "${app_products}/${asset}")
endforeach()

# Ship the project licence and third-party notices with every desktop build.
foreach(legal_file IN ITEMS LICENSE THIRD_PARTY_NOTICES.md)
    add_custom_command(OUTPUT "${app_products}/${legal_file}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/${legal_file}" "${app_products}/${legal_file}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${legal_file}"
        VERBATIM)
    list(APPEND app_asset_copies "${app_products}/${legal_file}")
endforeach()
add_custom_target(MidiControlAppAssets DEPENDS ${app_asset_copies})
add_dependencies(MidiControlApp MidiControlAppAssets)
