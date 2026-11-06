# Xbox Input: the host's pads and keyboard, as an Xbox controller

add_library(xbox_input STATIC
    xinput_device.c
    # Host device choice and per-control bindings, from the config file the
    # input UI writes (tools/input_ui). Used by both input paths: the XAPI
    # replacement in src/hle and the USB gamepad in src/usb.
    input_bindings.c
    # The pads themselves: SDL3 everywhere, XInput on Windows behind
    # RECOMP_PAD_API=xinput or the bindings file's "pad_api".
    recomp_pad.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3)
# Shared debug overlay reads the same native update target as the title clock.
target_link_libraries(xbox_input PRIVATE xbox_config)
if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_controls.c"
                                      "${TSFP_PC_CONTROLS_DIR}/texture_mode.c"
                                      "${TSFP_PC_CONTROLS_DIR}/pc_input.c"
                                      "${TSFP_PC_CONTROLS_DIR}/pc_settings.c")
    target_include_directories(xbox_input PUBLIC "${TSFP_PC_CONTROLS_DIR}")
    # Keep the define private: adding it to generated game sources would
    # recompile millions of unchanged lines for a host-input change.
    target_compile_definitions(xbox_input PRIVATE TSFP_PC_CONTROLS)
    target_link_libraries(xbox_input PUBLIC comctl32)
endif()
if(WIN32)
    target_link_libraries(xbox_input PUBLIC xinput)
endif()

if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_prompts.c")
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# What the runtime sees of the pads, live: `recomp_pad_probe [seconds] [xinput]`.
# Not built by default; `cmake --build <dir> --target recomp_pad_probe`.
add_executable(recomp_pad_probe EXCLUDE_FROM_ALL recomp_pad_probe.c)
target_link_libraries(recomp_pad_probe PRIVATE xbox_input)

# PC diagnostics use the same host-input library as the settings dialog.
if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_debug.c")
    target_include_directories(xbox_input PRIVATE "${CMAKE_BINARY_DIR}/generated")
    file(STRINGS "${TSFP_PC_CONTROLS_DIR}/../VERSION" TSFP_PC_VERSION LIMIT_COUNT 1)
    target_compile_definitions(xbox_input PRIVATE TSFP_PC_VERSION="${TSFP_PC_VERSION}")
    target_link_libraries(xbox_input PUBLIC shell32)
endif()

if(APPLE AND TSFP_NATIVE_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_NATIVE_DIR}/input_sdl.c")
    target_include_directories(xbox_input PRIVATE "${TSFP_NATIVE_DIR}")
    target_compile_definitions(xbox_input PRIVATE TSFP_NATIVE_INPUT)
endif()
