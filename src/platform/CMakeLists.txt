cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Original project mapping providers shared by compatibility and kernel code.
if(NOT TSFP_SHARED_SOURCE OR NOT EXISTS "${TSFP_SHARED_SOURCE}/runtime/shared_views.c")
    message(FATAL_ERROR "Project guest mapping providers missing; use project build entry points")
endif()
add_library(tsfp_guest_maps STATIC
    "${TSFP_SHARED_SOURCE}/runtime/shared_views.c"
    "${TSFP_SHARED_SOURCE}/runtime/owned_arena.c"
    "${TSFP_SHARED_SOURCE}/runtime/mmio_views.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_arena_service.c"
    "${TSFP_SHARED_SOURCE}/native/shared_host.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_map_layout.c"
    "${TSFP_SHARED_SOURCE}/native/guest_map_host.c")
target_include_directories(tsfp_guest_maps PUBLIC "${TSFP_SHARED_SOURCE}/runtime")
if(MSVC)
    target_compile_definitions(tsfp_guest_maps PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(tsfp_guest_maps PRIVATE /W4 /O2 /Zi)
else()
    target_compile_options(tsfp_guest_maps PRIVATE -Wall -Wextra -O2)
endif()

if(WIN32)
    add_library(platform INTERFACE)
    target_link_libraries(platform INTERFACE tsfp_guest_maps)
    target_include_directories(platform INTERFACE ${XBOXRECOMP_SRC_DIR})
else()
    add_library(platform STATIC win32_compat.c)
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads tsfp_guest_maps)
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
