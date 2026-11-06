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
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/input/pc_controls.c"
                                      "${TSFP_PC_CONTROLS_DIR}/textures/texture_mode.c"
                                      "${TSFP_PC_CONTROLS_DIR}/input/pc_input.c"
                                      "${TSFP_PC_CONTROLS_DIR}/interface/pc_settings.c")
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
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/input/pc_prompts.c")
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
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/diagnostics/pc_debug.c")
    target_include_directories(xbox_input PRIVATE "${CMAKE_BINARY_DIR}/generated")
    file(STRINGS "${TSFP_PC_CONTROLS_DIR}/../VERSION" TSFP_PC_VERSION LIMIT_COUNT 1)
    target_compile_definitions(xbox_input PRIVATE TSFP_PC_VERSION="${TSFP_PC_VERSION}")
    target_link_libraries(xbox_input PUBLIC shell32)
endif()

if(UNIX AND TSFP_NATIVE_DIR)
    # Compile the same portable input model used by the Windows adapter.
    target_sources(xbox_input PRIVATE "${TSFP_NATIVE_DIR}/input_sdl.c"
                                      "${TSFP_NATIVE_DIR}/../input/pc_input.c"
                                      "${TSFP_NATIVE_DIR}/config_files.c"
                                      "${TSFP_NATIVE_DIR}/../input/pc_prompts.c"
                                      "${TSFP_NATIVE_DIR}/../diagnostics/pc_debug.c")
    if(APPLE)
        enable_language(OBJC)
        target_sources(xbox_input PRIVATE "${TSFP_NATIVE_DIR}/text_bitmap_macos.m" "${TSFP_NATIVE_DIR}/settings_macos.m")
        set_source_files_properties("${TSFP_NATIVE_DIR}/settings_macos.m" PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(xbox_input PUBLIC "-framework Cocoa" "-framework CoreText")
    else()
        find_package(Freetype REQUIRED)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
        target_sources(xbox_input PRIVATE "${TSFP_NATIVE_DIR}/text_bitmap_linux.c" "${TSFP_NATIVE_DIR}/settings_linux.c")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        target_link_libraries(xbox_input PUBLIC PkgConfig::GTK3)
        target_link_libraries(xbox_input PUBLIC Freetype::Freetype PkgConfig::FONTCONFIG)
    endif()
    target_include_directories(xbox_input PRIVATE "${TSFP_NATIVE_DIR}/..")
    target_include_directories(xbox_input PRIVATE "${TSFP_NATIVE_DIR}")
    target_compile_definitions(xbox_input PRIVATE TSFP_NATIVE_INPUT)
    target_include_directories(xbox_input PRIVATE "${CMAKE_BINARY_DIR}/generated")
    file(STRINGS "${TSFP_NATIVE_DIR}/../../VERSION" TSFP_PC_VERSION LIMIT_COUNT 1)
    target_compile_definitions(xbox_input PRIVATE TSFP_PC_VERSION="${TSFP_PC_VERSION}")
endif()

if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/network/pc_discord.c"
                                      "${TSFP_PC_CONTROLS_DIR}/network/pc_discord_model.c")
endif()

if(UNIX AND TSFP_NATIVE_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_NATIVE_DIR}/../network/pc_discord.c"
        "${TSFP_NATIVE_DIR}/../network/pc_discord_model.c" "${TSFP_NATIVE_DIR}/crash_posix.c")
    if(APPLE)
        target_link_libraries(xbox_input PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(xbox_input PUBLIC OpenSSL::Crypto ${CMAKE_DL_LIBS})
    endif()
endif()

