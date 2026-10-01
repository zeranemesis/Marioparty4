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
