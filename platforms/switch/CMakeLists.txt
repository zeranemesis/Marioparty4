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
            "--nacp=${PARTYBOARD_SWITCH_NACP}"
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

# nodlite: nod's C API without Rust, for every Switch target that reads discs
# (the launcher today, Aurora's DVD layer and the disc validation next).
add_subdirectory(nodlite)

# GameCube-style launcher (game shelf + boot animation) that chain-loads the
# engine NRO. Independent of the probe above.
add_subdirectory(launcher)


option(PARTYBOARD_SWITCH_DAWN_PROBE
    "Cross-compile a minimal Dawn/OpenGLES adapter probe for libnx"
    OFF)

if(PARTYBOARD_SWITCH_DAWN_PROBE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/dawn/SwitchDawn.cmake")

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
                "--nacp=${PARTYBOARD_SWITCH_DAWN_NACP}"
        DEPENDS partyboard_switch_dawn_probe "${PARTYBOARD_SWITCH_DAWN_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard Dawn probe NRO"
    )
    add_custom_target(partyboard_switch_dawn_nro
        DEPENDS "${PARTYBOARD_SWITCH_DAWN_NRO}"
    )

    message(STATUS "PartyBoard experimental Dawn/libnx probe enabled")
endif()

option(PARTYBOARD_SWITCH_SDL3_PROBE
    "Build SDL3 for libnx (sdl3/) and an NRO that exercises it"
    OFF)

if(PARTYBOARD_SWITCH_SDL3_PROBE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/sdl3/SwitchSDL3.cmake")

    add_executable(partyboard_switch_sdl3_probe source/sdl3_probe.cpp)
    set_target_properties(partyboard_switch_sdl3_probe PROPERTIES
        OUTPUT_NAME "partyboard-switch-sdl3-probe"
        SUFFIX ".elf"
    )
    target_compile_definitions(partyboard_switch_sdl3_probe PRIVATE __SWITCH__=1)
    target_compile_options(partyboard_switch_sdl3_probe PRIVATE -Wall -Wextra -O2)
    target_link_options(partyboard_switch_sdl3_probe PRIVATE -Wl,--gc-sections)
    target_link_libraries(partyboard_switch_sdl3_probe PRIVATE SDL3::SDL3-static)

    set(PARTYBOARD_SWITCH_SDL3_NACP "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-sdl3-probe.nacp")
    set(PARTYBOARD_SWITCH_SDL3_NRO  "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-sdl3-probe.nro")
    add_custom_command(
        OUTPUT "${PARTYBOARD_SWITCH_SDL3_NACP}"
        COMMAND "${PARTYBOARD_NACPTOOL}" --create
                "PartyBoard SDL3 Probe"
                "MarioPartyRD"
                "0.1.0"
                "${PARTYBOARD_SWITCH_SDL3_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard SDL3 probe NACP"
    )
    add_custom_command(
        OUTPUT "${PARTYBOARD_SWITCH_SDL3_NRO}"
        COMMAND "${PARTYBOARD_ELF2NRO}"
                "$<TARGET_FILE:partyboard_switch_sdl3_probe>"
                "${PARTYBOARD_SWITCH_SDL3_NRO}"
                "--nacp=${PARTYBOARD_SWITCH_SDL3_NACP}"
        DEPENDS partyboard_switch_sdl3_probe "${PARTYBOARD_SWITCH_SDL3_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard SDL3 probe NRO"
    )
    add_custom_target(partyboard_switch_sdl3_nro
        DEPENDS "${PARTYBOARD_SWITCH_SDL3_NRO}"
    )

    message(STATUS "PartyBoard SDL3/libnx probe enabled")
endif()

option(PARTYBOARD_SWITCH_AURORA_PROBE
    "Build Aurora for libnx (aurora/) and an NRO running its GX clear example"
    OFF)

if(PARTYBOARD_SWITCH_AURORA_PROBE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/aurora/SwitchAurora.cmake")

    add_executable(partyboard_switch_aurora_probe source/aurora_probe.c)
    set_target_properties(partyboard_switch_aurora_probe PROPERTIES
        OUTPUT_NAME "partyboard-switch-aurora-probe"
        SUFFIX ".elf"
    )
    target_compile_options(partyboard_switch_aurora_probe PRIVATE -Wall -Wextra -O2)
    target_link_options(partyboard_switch_aurora_probe PRIVATE -Wl,--gc-sections)
    target_link_libraries(partyboard_switch_aurora_probe PRIVATE
        aurora::core aurora::gx aurora::main aurora::pad aurora::vi)

    set(PARTYBOARD_SWITCH_AURORA_NACP "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-aurora-probe.nacp")
    set(PARTYBOARD_SWITCH_AURORA_NRO  "${CMAKE_CURRENT_BINARY_DIR}/partyboard-switch-aurora-probe.nro")
    add_custom_command(
        OUTPUT "${PARTYBOARD_SWITCH_AURORA_NACP}"
        COMMAND "${PARTYBOARD_NACPTOOL}" --create
                "PartyBoard Aurora Probe"
                "MarioPartyRD"
                "0.1.0"
                "${PARTYBOARD_SWITCH_AURORA_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard Aurora probe NACP"
    )
    add_custom_command(
        OUTPUT "${PARTYBOARD_SWITCH_AURORA_NRO}"
        COMMAND "${PARTYBOARD_ELF2NRO}"
                "$<TARGET_FILE:partyboard_switch_aurora_probe>"
                "${PARTYBOARD_SWITCH_AURORA_NRO}"
                "--nacp=${PARTYBOARD_SWITCH_AURORA_NACP}"
        DEPENDS partyboard_switch_aurora_probe "${PARTYBOARD_SWITCH_AURORA_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard Aurora probe NRO"
    )
    add_custom_target(partyboard_switch_aurora_nro
        DEPENDS "${PARTYBOARD_SWITCH_AURORA_NRO}"
    )

    message(STATUS "PartyBoard Aurora/libnx probe enabled")
endif()

option(PARTYBOARD_SWITCH_GAME
    "Build the game itself for libnx (game/), on top of the Aurora probe's stack"
    OFF)

if(PARTYBOARD_SWITCH_GAME)
    include("${CMAKE_CURRENT_SOURCE_DIR}/game/SwitchGame.cmake")
endif()
