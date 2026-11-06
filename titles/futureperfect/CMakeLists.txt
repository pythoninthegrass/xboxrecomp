cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Timesplitters - Future Perfect - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(futureperfect_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# ── Bring-up diagnostics ────────────────────────────────────────
#
# ebx/esi/edi are callee-saved on x86 and the recompiler keeps them in globals,
# so a function whose epilogue did not lift correctly corrupts its *caller*.
# There is no crash and no message where the failure happens: the caller
# carries on with a wrong loop cursor or a wrong `this` and faults somewhere
# unrelated, often several frames away.
#
# With this on, every call -- direct and indirect -- checks that ebx, esi and
# edi came back unchanged and that esp came back at least 4 higher, and names
# the offending function. Off by default because it costs a few compares per
# call; leave it on until a title boots.
#
# Directory scope, and set before add_subdirectory, deliberately: the check is
# emitted into generated game code but recomp_abi_violation_log() is compiled
# into xbox_kernel. Defining it on the game target alone builds the calls
# without the callee and fails to link.
option(RECOMP_ABI_CHECK "Verify callee-saved registers across every call" OFF)
if(RECOMP_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK)
    message(STATUS "ABI check enabled: callee-saved registers verified per call")
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    add_executable(tsfp_pc_debug_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_debug.c")
    target_link_libraries(tsfp_pc_debug_test PRIVATE xbox_input)
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    add_executable(tsfp_pc_prompts_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_prompts.c")
    target_link_libraries(tsfp_pc_prompts_test PRIVATE xbox_input)
endif()

# Core game sources
set(GAME_SOURCES
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

if(APPLE AND TSFP_NATIVE_DIR)
    list(APPEND GAME_SOURCES "${TSFP_NATIVE_DIR}/main.c" "${TSFP_NATIVE_DIR}/mmio.c")
elseif(WIN32)
    list(APPEND GAME_SOURCES src/main.c)
else()
    message(FATAL_ERROR "This title needs TSFP_NATIVE_DIR on macOS, or a Windows host")
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# A build-generated resource embeds the icon before executable identity sealing.
# SDL on Windows uses the executable's first group icon for its window class.
if(WIN32 AND TSFP_GAME_ICON_RESOURCE)
    enable_language(RC)
    target_sources(${PROJECT_NAME} PRIVATE "${TSFP_GAME_ICON_RESOURCE}")
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Windows crash capture is separate from the native Mac adapter.
if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    target_sources(xbox_input PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_crash.c")
    set_source_files_properties("${TSFP_PC_CONTROLS_DIR}/pc_crash.c" PROPERTIES COMPILE_OPTIONS "/Zi")
    add_executable(tsfp_crash_helper "${TSFP_PC_CONTROLS_DIR}/../tools/crash_helper.c")
    target_include_directories(tsfp_crash_helper PRIVATE "${TSFP_PC_CONTROLS_DIR}")
    target_link_libraries(tsfp_crash_helper PRIVATE dbghelp bcrypt)
    target_compile_options(tsfp_crash_helper PRIVATE /W4 /Zi)
    target_link_options(tsfp_crash_helper PRIVATE /DEBUG)
    add_dependencies(${PROJECT_NAME} tsfp_crash_helper)
    add_executable(tsfp_hang_policy_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_hang_policy.c")
    target_include_directories(tsfp_hang_policy_test PRIVATE "${TSFP_PC_CONTROLS_DIR}")
endif()


if(TSFP_LOCAL_PATH_TEST_SOURCE)
    add_executable(tsfp_save_paths_test "${TSFP_LOCAL_PATH_TEST_SOURCE}")
    target_link_libraries(tsfp_save_paths_test PRIVATE xboxrecomp)
endif()

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)


if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    target_sources(${PROJECT_NAME} PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_network.c")
    target_sources(${PROJECT_NAME} PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_online.c")
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 bcrypt)
    add_executable(tsfp_pc_network_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_network.c"
        "${TSFP_PC_CONTROLS_DIR}/pc_network.c")
    target_include_directories(tsfp_pc_network_test PRIVATE "${TSFP_PC_CONTROLS_DIR}" "${CMAKE_BINARY_DIR}/generated")
    target_link_libraries(tsfp_pc_network_test PRIVATE ws2_32 bcrypt)
endif()

# Windows SDK libraries needed at link time
if(WIN32)
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # A PDB in every configuration, so the crash handler's SymFromAddr can
        # name the generated function a host RIP is in. Linker-only: without
        # /Zi it holds public symbols -- every sub_XXXXXXXX -- and costs no
        # recompile of the large generated files.
        /DEBUG
    )
    # The generated code files can be very large. Increase object file section
    # limit, and compile them in parallel: MSBuild only parallelises across
    # projects, so without /MP the 36 generated files of a 36,000-function
    # title compile one after another on a single core.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP)

    # Drop MSVC's Debug runtime checks. /RTC1 traps a local read before its
    # first write, and lifted code does that deliberately: an fpo_leaf function
    # inherits its caller's frame, so it reads ebp before assigning it and
    # picks the value up from g_seh_ebp. That is correct by design here, but
    # /RTC1 cannot know it and aborts through a message box -- which never
    # reaches stderr, so the run looks like a silent hang at the first lifted
    # frame. Debug is the configuration bring-up uses, so this cannot stay on.
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

if(TSFP_PC_CONTROLS_DIR)
    if(WIN32)
        set_source_files_properties(src/main.c PROPERTIES COMPILE_DEFINITIONS TSFP_PC_CONTROLS)
    endif()
    set_source_files_properties(src/recomp_manual.c PROPERTIES COMPILE_DEFINITIONS TSFP_PC_CONTROLS)
    add_executable(tsfp_pc_controls_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_controls.c")
    target_link_libraries(tsfp_pc_controls_test PRIVATE xbox_input)
    add_executable(tsfp_pc_settings_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_settings.c")
    target_link_libraries(tsfp_pc_settings_test PRIVATE xbox_input)
endif()

if(TSFP_PC_CONTROLS_DIR)
    target_sources(${PROJECT_NAME} PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_display_menu.c")
    set_source_files_properties("${TSFP_PC_CONTROLS_DIR}/pc_display_menu.c" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen")
endif()

if(TSFP_PC_CONTROLS_DIR)
    add_executable(tsfp_pc_display_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_display.c" "${TSFP_PC_CONTROLS_DIR}/pc_display.c")
    target_include_directories(tsfp_pc_display_test PRIVATE "${TSFP_PC_CONTROLS_DIR}")
    target_link_libraries(tsfp_pc_display_test PRIVATE user32)
endif()

if(APPLE AND TSFP_NATIVE_DIR)
    # Keep debug symbols, but remove interpreter-style lifted-code overhead.
    # Protected MMIO is decoded by the scalar ARM64 fault handler.
    target_compile_options(${PROJECT_NAME} PRIVATE -O2 -fno-vectorize -fno-slp-vectorize)
    enable_testing()
    set(TSFP_TESTS "${TSFP_NATIVE_DIR}/../../tests")
    add_executable(tsfp_native_platform_test "${TSFP_TESTS}/native_platform.c")
    target_link_libraries(tsfp_native_platform_test PRIVATE platform)
    add_test(NAME native_platform COMMAND tsfp_native_platform_test)
    add_executable(tsfp_native_mmio_test "${TSFP_TESTS}/native_mmio.c" "${TSFP_NATIVE_DIR}/mmio.c")
    target_include_directories(tsfp_native_mmio_test PRIVATE "${TSFP_NATIVE_DIR}" "${XBOXRECOMP_DIR}/include")
    target_link_libraries(tsfp_native_mmio_test PRIVATE platform)
    add_test(NAME native_mmio COMMAND tsfp_native_mmio_test)
    add_executable(tsfp_native_paths_test "${TSFP_TESTS}/native_paths.c")
    target_link_libraries(tsfp_native_paths_test PRIVATE xboxrecomp)
    add_test(NAME native_paths COMMAND tsfp_native_paths_test "${CMAKE_BINARY_DIR}/path-tests")
endif()

if(APPLE AND TSFP_NATIVE_DIR)
    target_sources(${PROJECT_NAME} PRIVATE "${TSFP_NATIVE_DIR}/../pc_display_menu.c")
    set_source_files_properties("${TSFP_NATIVE_DIR}/../pc_display_menu.c" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen")
    target_include_directories(${PROJECT_NAME} PRIVATE "${TSFP_NATIVE_DIR}/..")
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSFP_NATIVE_DISPLAY TSFP_NATIVE_PROMPTS)
endif()

if(WIN32 AND TSFP_PC_CONTROLS_DIR)
    set_property(SOURCE "${TSFP_PC_CONTROLS_DIR}/pc_display_menu.c" APPEND PROPERTY
        COMPILE_DEFINITIONS TSFP_PC_DISCORD)
    add_executable(tsfp_pc_discord_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_discord.c"
        "${TSFP_PC_CONTROLS_DIR}/pc_discord_model.c")
    target_include_directories(tsfp_pc_discord_test PRIVATE "${TSFP_PC_CONTROLS_DIR}")
endif()

if(APPLE AND TSFP_NATIVE_DIR)
    set_property(SOURCE "${TSFP_NATIVE_DIR}/../pc_display_menu.c" APPEND PROPERTY
        COMPILE_DEFINITIONS TSFP_PC_DISCORD)
endif()
