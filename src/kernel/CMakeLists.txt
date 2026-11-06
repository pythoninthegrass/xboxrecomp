cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_io.c
    kernel_crypto.c
    kernel_bridge.c
    xbox_memory_layout.c
    xbox_watchpoint.c
    icall_feedback.c
    exit_trace.c
    recomp_trace.c
    recomp_fps.c
    recomp_sample.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_vsh.c
)

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

target_sources(xbox_kernel PRIVATE
    "${TSFP_SHARED_SOURCE}/runtime/guest_x86_context.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_stack_trace.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_text_raw.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_string_transform.c"
    "${TSFP_SHARED_SOURCE}/runtime/host_string_case.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_text_codec.c"
    "${TSFP_SHARED_SOURCE}/runtime/host_text_codec.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_memory_ops.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_integer_division.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_integer_fault.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_integer_product.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_numbers.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_printf.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_critical.c"
    "${TSFP_SHARED_SOURCE}/runtime/host_critical.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_rtl_services.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_kernel_rtl.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_strings.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_time.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_storage.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_kernel_storage.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_paths.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_symlinks.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_kernel_symlinks.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_targets.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_event.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_scheduler.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_queue_wire.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_startio.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_queue.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_transfer.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_build.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_complete.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_call.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_target.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_scope.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_devices.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_share.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_object_names.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_name_service.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_factory.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_body.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_callback.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_callback_call.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_dispatch.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_event.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_event_scope.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_device_event_batch.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_stack.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_request.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_owner.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_registry.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_io_pool.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_kernel_boot.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_objects.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_object_scope.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_object_services.c")

# The complete project stack supplies this root before add_subdirectory.
if(NOT TSFP_SHARED_SOURCE OR NOT EXISTS "${TSFP_SHARED_SOURCE}/runtime/guest_nonlocal_jump.c")
    message(FATAL_ERROR "Project continuation provider missing; use the project build entry point")
endif()
target_sources(xbox_kernel PRIVATE "${TSFP_SHARED_SOURCE}/runtime/guest_nonlocal_jump.c")

# Project architectural cells and optional ABI report hooks share the public ABI.
target_sources(xbox_kernel PRIVATE
    "${TSFP_SHARED_SOURCE}/runtime/guest_state.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_abi_diagnostics.c")

# Independent image inspection and materialization, shared by both hosts.
target_sources(xbox_kernel PRIVATE "${TSFP_SHARED_SOURCE}/runtime/guest_image.c")
target_sources(xbox_kernel PRIVATE "${TSFP_SHARED_SOURCE}/runtime/guest_allocation.c")
target_sources(xbox_kernel PRIVATE "${TSFP_SHARED_SOURCE}/runtime/guest_thread_memory.c"
    "${TSFP_SHARED_SOURCE}/runtime/guest_virtual_memory.c"
    "${TSFP_SHARED_SOURCE}/runtime/host_virtual_memory.c")
target_include_directories(xbox_kernel PRIVATE "${TSFP_SHARED_SOURCE}/runtime")

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)
target_link_libraries(xbox_kernel PUBLIC xbox_config)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    # winmm for timeBeginPeriod (the vblank clock's fallback timer), dbghelp
    # and psapi for the sampling profiler's symbol and module lookups.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt winmm dbghelp psapi)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    # /Zi in every configuration: the title links with /DEBUG, so the PDB then
    # names this library's static functions too, and a sampled profile
    # (RECOMP_SAMPLE) attributes time to nv2a_ack_thread rather than to whatever
    # public symbol happens to precede it. No effect on the generated code.
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- $<$<NOT:$<CONFIG:Debug>>:/O2> /GS- /Gy /Zi)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)

# Original native Mm ownership and thin OS page operations, shared by both hosts.
target_sources(xbox_kernel PRIVATE
    "${TSFP_SHARED_SOURCE}/runtime/host_blocks.c"
    "${TSFP_SHARED_SOURCE}/native/host_pages.c")

# Original guest Mm policy and host memory snapshots.
target_sources(xbox_kernel PRIVATE
    "${TSFP_SHARED_SOURCE}/runtime/guest_memory_services.c"
    "${TSFP_SHARED_SOURCE}/runtime/memory_statistics.c"
    "${TSFP_SHARED_SOURCE}/native/host_memory_stats.c")

# Native NT host operations and original reservation ownership.
target_sources(xbox_kernel PRIVATE
    "${TSFP_SHARED_SOURCE}/runtime/native_reservations.c"
    "${TSFP_SHARED_SOURCE}/native/native_vm_host.c")
