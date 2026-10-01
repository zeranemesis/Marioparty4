# Dawn for libnx: the revision Aurora pins, WebGPU core plus the OpenGL ES
# backend only, patched at fetch time (cmake/PatchDawn*ForSwitch.cmake).
# Shared by the Dawn probe and the Aurora probe; defines dawn::webgpu_dawn.

include_guard(GLOBAL)

include(FetchContent)

# Keep this in lock-step with the Dawn revision pinned by extern/aurora.
set(PARTYBOARD_SWITCH_DAWN_REF
    "13abc3bc8ea2d3c2050f9e77a12d012108ceee24"
    CACHE STRING "Dawn revision for the Switch probe")

# Build the smallest useful Dawn: WebGPU core + OpenGL ES only.
set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_NODE_BINDINGS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES ON CACHE BOOL "" FORCE)
set(DAWN_ENABLE_SPIRV_VALIDATION OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_USE_X11 OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WAYLAND OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WINDOWS_UI OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_WRITER ON CACHE BOOL "" FORCE)
# Dawn's GL backend runs Tint's null writer to size compute workgroups;
# it otherwise follows DAWN_ENABLE_NULL, which is off here.
set(TINT_BUILD_NULL_WRITER ON CACHE BOOL "" FORCE)
# CMake's Generic platform names libdl for CMAKE_DL_LIBS; libnx has no
# dynamic loader (Dawn's DynamicLib is patched out for the Switch).
set(CMAKE_DL_LIBS "")

FetchContent_Declare(
    partyboard_switch_dawn
    URL "https://github.com/google/dawn/archive/${PARTYBOARD_SWITCH_DAWN_REF}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DDAWN_SOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_SOURCE_DIR}/cmake/PatchDawnForSwitch.cmake"
)
FetchContent_MakeAvailable(partyboard_switch_dawn)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DDAWN_SOURCE_DIR=${partyboard_switch_dawn_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/PatchDawnDependenciesForSwitch.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

if(NOT TARGET dawn::webgpu_dawn)
    message(FATAL_ERROR "Dawn for the Switch was configured but dawn::webgpu_dawn was not created")
endif()
