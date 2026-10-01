cmake_minimum_required(VERSION 3.25)

if(NOT PARTYBOARD_SWITCH)
    message(FATAL_ERROR "platforms/switch requires the libnx toolchain")
endif()

find_program(PARTYBOARD_NACPTOOL nacptool
    HINTS "${DEVKITPRO}/tools/bin"
    REQUIRED)
find_program(PARTYBOARD_ELF2NRO elf2nro
    HINTS "${DEVKITPRO}/tools/bin"
    REQUIRED)

add_executable(partyboard_switch_probe
    source/main.cpp
    source/switch_pad.cpp
    source/switch_egl.cpp

    # First real Aurora compatibility-layer code compiled for ARM64/libnx.
    ${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtx.c
    ${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtxstack.c
    ${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtxvec.c
    ${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtx44.c
    ${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/vec.c
    ${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/quat.c
)

set_target_properties(partyboard_switch_probe PROPERTIES
    OUTPUT_NAME "partyboard-switch-probe"
    SUFFIX ".elf"
)

target_compile_definitions(partyboard_switch_probe PRIVATE __SWITCH__=1)
target_compile_options(partyboard_switch_probe PRIVATE
    -Wall
    -Wextra
    -O2
    -ffunction-sections
    -fdata-sections
    -fno-rtti
    -fno-exceptions
)

target_include_directories(partyboard_switch_probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${LIBNX}/include"
    "${PORTLIBS}/include"
    "${CMAKE_SOURCE_DIR}/extern/aurora/include"
)

target_link_directories(partyboard_switch_probe PRIVATE
    "${LIBNX}/lib"
    "${PORTLIBS}/lib"
)

target_link_options(partyboard_switch_probe PRIVATE
    -Wl,--gc-sections
)

target_link_libraries(partyboard_switch_probe PRIVATE
    glad
    EGL
    glapi
    drm_nouveau
    nx
)

set(PARTYBOARD_SWITCH_NACP "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-probe.nacp")
set(PARTYBOARD_SWITCH_NRO  "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-probe.nro")

add_custom_command(
    OUTPUT "${PARTYBOARD_SWITCH_NACP}"
    COMMAND "${PARTYBOARD_NACPTOOL}" --create
            "PartyBoard Switch Probe"
            "MarioPartyRD"
            "0.1.0"
            "${PARTYBOARD_SWITCH_NACP}"
    VERBATIM
    COMMENT "Generating PartyBoard Switch NACP"
)

add_custom_command(
    OUTPUT "${PARTYBOARD_SWITCH_NRO}"
    COMMAND "${PARTYBOARD_ELF2NRO}"
            "$<TARGET_FILE:partyboard_switch_probe>"
            "${PARTYBOARD_SWITCH_NRO}"
            --nacp="${PARTYBOARD_SWITCH_NACP}"
    DEPENDS partyboard_switch_probe "${PARTYBOARD_SWITCH_NACP}"
    VERBATIM
    COMMENT "Generating PartyBoard Switch NRO"
)

add_custom_target(partyboard_switch_nro ALL
    DEPENDS "${PARTYBOARD_SWITCH_NRO}"
)

message(STATUS "PartyBoard Switch bootstrap enabled")
message(STATUS "  libnx:     ${LIBNX}")
message(STATUS "  portlibs:  ${PORTLIBS}")
message(STATUS "  output:    ${PARTYBOARD_SWITCH_NRO}")


option(PARTYBOARD_SWITCH_DAWN_PROBE
    "Cross-compile a minimal Dawn/OpenGLES adapter probe for libnx"
    OFF)

if(PARTYBOARD_SWITCH_DAWN_PROBE)
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

    if(NOT TARGET dawn::webgpu_dawn)
        message(FATAL_ERROR "Dawn probe configured but dawn::webgpu_dawn was not created")
    endif()

    add_executable(partyboard_switch_dawn_probe
        source/dawn_probe.cpp
        source/switch_egl.cpp
    )
    set_target_properties(partyboard_switch_dawn_probe PROPERTIES
        OUTPUT_NAME "partyboard-switch-dawn-probe"
        SUFFIX ".elf"
    )

    target_compile_definitions(partyboard_switch_dawn_probe PRIVATE __SWITCH__=1)
    target_compile_options(partyboard_switch_dawn_probe PRIVATE
        -Wall
        -Wextra
        -O2
        -ffunction-sections
        -fdata-sections
        -fno-rtti
        -fno-exceptions
    )
    target_include_directories(partyboard_switch_dawn_probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${LIBNX}/include"
        "${PORTLIBS}/include"
    )
    target_link_directories(partyboard_switch_dawn_probe PRIVATE
        "${LIBNX}/lib"
        "${PORTLIBS}/lib"
    )
    target_link_options(partyboard_switch_dawn_probe PRIVATE -Wl,--gc-sections)
    target_link_libraries(partyboard_switch_dawn_probe PRIVATE
        dawn::webgpu_dawn
        glad
        EGL
        glapi
        drm_nouveau
        nx
    )

    set(PARTYBOARD_SWITCH_DAWN_NACP
        "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-dawn-probe.nacp")
    set(PARTYBOARD_SWITCH_DAWN_NRO
        "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-dawn-probe.nro")

    add_custom_command(
        OUTPUT "${PARTYBOARD_SWITCH_DAWN_NACP}"
        COMMAND "${PARTYBOARD_NACPTOOL}" --create
                "PartyBoard Dawn Probe"
                "MarioPartyRD"
                "0.1.0"
                "${PARTYBOARD_SWITCH_DAWN_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard Dawn probe NACP"
    )
    add_custom_command(
        OUTPUT "${PARTYBOARD_SWITCH_DAWN_NRO}"
        COMMAND "${PARTYBOARD_ELF2NRO}"
                "$<TARGET_FILE:partyboard_switch_dawn_probe>"
                "${PARTYBOARD_SWITCH_DAWN_NRO}"
                --nacp="${PARTYBOARD_SWITCH_DAWN_NACP}"
        DEPENDS partyboard_switch_dawn_probe "${PARTYBOARD_SWITCH_DAWN_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard Dawn probe NRO"
    )
    add_custom_target(partyboard_switch_dawn_nro
        DEPENDS "${PARTYBOARD_SWITCH_DAWN_NRO}"
    )

    message(STATUS "PartyBoard experimental Dawn/libnx probe enabled")
endif()
