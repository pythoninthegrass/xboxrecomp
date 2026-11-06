# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL3 + libepoxy (d3d8_gl.c)

option(TSFP_NATIVE_RHI "Use the common Xbox renderer on native Mac (experimental)" OFF)
if(WIN32 OR (APPLE AND TSFP_NATIVE_RHI))
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
        d3d8_overlay.c
        d3d8_movie.c
        d3d8_screencopy.c
        d3d8_display.c
        d3d8_compile.c
        rhi.c
        rhi_shader_cache.c
    )
    if(TSFP_PC_CONTROLS_DIR)
        target_sources(xbox_d3d8 PRIVATE "${TSFP_PC_CONTROLS_DIR}/texture_pack.c")
        target_include_directories(xbox_d3d8 PRIVATE "${TSFP_PC_CONTROLS_DIR}")
        target_compile_definitions(xbox_d3d8 PUBLIC TSFP_TEXTURE_PACK)
        add_executable(tsfp_texture_toggle_test
            "${TSFP_PC_CONTROLS_DIR}/../tests/texture_toggle.c"
            "${TSFP_PC_CONTROLS_DIR}/texture_pack.c"
            "${TSFP_PC_CONTROLS_DIR}/texture_mode.c")
        target_include_directories(tsfp_texture_toggle_test PRIVATE
            "${TSFP_PC_CONTROLS_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
        target_compile_definitions(tsfp_texture_toggle_test PRIVATE TSFP_TEXTURE_PACK)
        add_executable(tsfp_texture_format_test "${TSFP_PC_CONTROLS_DIR}/../tests/texture_pack_format.c")
        target_include_directories(tsfp_texture_format_test PRIVATE "${TSFP_PC_CONTROLS_DIR}")
    endif()
    if(WIN32)
    target_sources(xbox_d3d8 PRIVATE rhi_d3d11.c)
    # gdi32: the overlay renders its text with GDI rather than an embedded font.
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler gdi32)

    # The Vulkan backend (RECOMP_D3D8_BACKEND=vulkan), built beside D3D11
    # when the vendored headers are checked out (git submodule update
    # --init). Nothing links a Vulkan loader or DXC: volk finds the loader
    # and rhi_vulkan_dxc.cpp finds dxcompiler at run time, so an executable
    # built with this still runs on a machine with neither.
    set(XBR_THIRD_PARTY ${CMAKE_CURRENT_LIST_DIR}/../../third_party)
    if(EXISTS ${XBR_THIRD_PARTY}/Vulkan-Headers/include/vulkan/vulkan.h AND
       EXISTS ${XBR_THIRD_PARTY}/volk/volk.c AND
       EXISTS ${XBR_THIRD_PARTY}/VulkanMemoryAllocator/include/vk_mem_alloc.h AND
       EXISTS ${XBR_THIRD_PARTY}/dxc/inc/dxcapi.h)
        set(XBR_VULKAN_DEFAULT ON)
    else()
        set(XBR_VULKAN_DEFAULT OFF)
    endif()
    option(XBOXRECOMP_VULKAN "Build the Vulkan renderer backend" ${XBR_VULKAN_DEFAULT})
    if(XBOXRECOMP_VULKAN)
        target_sources(xbox_d3d8 PRIVATE
            rhi_vulkan.c
            rhi_vulkan_dxc.cpp
            rhi_vulkan_vma.cpp
            ${XBR_THIRD_PARTY}/volk/volk.c)
        target_include_directories(xbox_d3d8 PRIVATE
            ${XBR_THIRD_PARTY}/Vulkan-Headers/include
            ${XBR_THIRD_PARTY}/volk
            ${XBR_THIRD_PARTY}/VulkanMemoryAllocator/include
            ${XBR_THIRD_PARTY}/dxc/inc)
        target_compile_definitions(xbox_d3d8 PRIVATE
            RHI_HAVE_VULKAN VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
        if(EXISTS ${XBR_THIRD_PARTY}/dxc/bin/x64/dxcompiler.dll)
            file(TO_NATIVE_PATH ${XBR_THIRD_PARTY}/dxc/bin/x64/dxcompiler.dll XBR_DXC_DLL)
            string(REPLACE "\\" "\\\\" XBR_DXC_DLL "${XBR_DXC_DLL}")
            set_source_files_properties(rhi_vulkan_dxc.cpp PROPERTIES
                COMPILE_DEFINITIONS "RHI_DXC_LIBRARY=\"${XBR_DXC_DLL}\"")
        endif()
        # Vendored code keeps its own warnings.
        if(MSVC)
            set_source_files_properties(${XBR_THIRD_PARTY}/volk/volk.c rhi_vulkan_vma.cpp
                                        PROPERTIES COMPILE_OPTIONS "/W0")
        endif()
    endif()

    else()
        target_sources(xbox_d3d8 PRIVATE
            "${TSFP_NATIVE_DIR}/renderer_rhi_sdl.c" "${TSFP_NATIVE_DIR}/renderer_host_sdl.c"
            "${TSFP_NATIVE_DIR}/../texture_mode.c"
            "${TSFP_NATIVE_DIR}/../texture_pack.c")
        target_include_directories(xbox_d3d8 PRIVATE "${TSFP_NATIVE_DIR}" "${TSFP_NATIVE_DIR}/..")
        target_compile_definitions(xbox_d3d8 PUBLIC TSFP_RHI_RENDERER TSFP_PC_DISPLAY TSFP_TEXTURE_PACK)
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL3::SDL3)
        include("${TSFP_NATIVE_DIR}/vulkan_backend.cmake")
        tsfp_add_vulkan_backend(xbox_d3d8 "${CMAKE_CURRENT_SOURCE_DIR}/../.." "${TSFP_VULKAN_SDK}")
    endif()

    if(WIN32)
    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
    endif()
else()
    if(APPLE AND TSFP_NATIVE_DIR)
        add_library(xbox_d3d8 STATIC "${TSFP_NATIVE_DIR}/renderer_gl.c"
            "${TSFP_NATIVE_DIR}/../texture_mode.c"
            "${TSFP_NATIVE_DIR}/renderer_host_sdl.c" rhi.c)
    else()
        add_library(xbox_d3d8 STATIC d3d8_gl.c)
    endif()
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL3::SDL3)
    if(APPLE)
        find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
        target_link_libraries(xbox_d3d8 PUBLIC ${OPENGL_FRAMEWORK})
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(EPOXY REQUIRED epoxy)
        target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
        target_link_libraries(xbox_d3d8 PUBLIC ${EPOXY_LIBRARIES})
    endif()
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_d3d8 PUBLIC xbox_config)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2> /Zi)  # /Zi: see src/kernel/CMakeLists.txt
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

if(TSFP_PC_CONTROLS_DIR)
    target_sources(xbox_d3d8 PRIVATE "${TSFP_PC_CONTROLS_DIR}/pc_display.c")
    target_compile_definitions(xbox_d3d8 PRIVATE TSFP_PC_DISPLAY)
    if(WIN32)
        add_executable(tsfp_pc_clear_test "${TSFP_PC_CONTROLS_DIR}/../tests/pc_clear.c")
        target_include_directories(tsfp_pc_clear_test PRIVATE "${TSFP_PC_CONTROLS_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../hle")
        target_link_libraries(tsfp_pc_clear_test PRIVATE xbox_d3d8)
    endif()
endif()

if(APPLE AND TSFP_NATIVE_DIR AND NOT TSFP_NATIVE_RHI)
    target_sources(xbox_d3d8 PRIVATE d3d8_resources.c)
    set_source_files_properties(d3d8_resources.c PROPERTIES COMPILE_DEFINITIONS TSFP_FORMAT_ONLY)
    target_include_directories(xbox_d3d8 PRIVATE "${TSFP_NATIVE_DIR}")
endif()

if(APPLE AND TSFP_NATIVE_DIR)
    target_sources(xbox_d3d8 PRIVATE "${TSFP_NATIVE_DIR}/../pc_display.c")
    target_include_directories(xbox_d3d8 PRIVATE "${TSFP_NATIVE_DIR}/..")
endif()

if(APPLE AND TSFP_NATIVE_DIR)
    include("${TSFP_NATIVE_DIR}/vulkan_probe.cmake")
endif()
