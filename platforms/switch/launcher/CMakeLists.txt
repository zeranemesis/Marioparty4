cmake_minimum_required(VERSION 3.20)

# PartyBoard GameCube launcher.
#
# Two ways in:
#   - from platforms/switch (libnx toolchain): builds partyboard-launcher.nro;
#   - standalone on a desktop host:
#       cmake -S platforms/switch/launcher -B build/launcher-host
#     builds the headless preview (screenshots via Mesa EGL) and the unit tests.

if(NOT PARTYBOARD_SWITCH)
    project(partyboard_launcher_host LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PARTYBOARD_LAUNCHER_CORE
    src/app.cpp
    src/boot.cpp
    src/catalog.cpp
    src/disc.cpp
    src/font.cpp
    src/i18n.cpp
    src/image.cpp
    src/json.cpp
    src/library.cpp
    src/mods.cpp
    src/render.cpp
    src/settings.cpp
    src/sound.cpp
)

set(PARTYBOARD_LAUNCHER_WARNINGS -Wall -Wextra)

if(PARTYBOARD_SWITCH)
    add_executable(partyboard_switch_launcher
        ${PARTYBOARD_LAUNCHER_CORE}
        switch/switch_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../source/switch_egl.cpp
    )
    set_target_properties(partyboard_switch_launcher PROPERTIES
        OUTPUT_NAME "partyboard-launcher"
        SUFFIX ".elf"
    )
    target_compile_definitions(partyboard_switch_launcher PRIVATE __SWITCH__=1)
    target_compile_options(partyboard_switch_launcher PRIVATE
        ${PARTYBOARD_LAUNCHER_WARNINGS}
        -O2
        -ffunction-sections
        -fdata-sections
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti -fno-exceptions>
    )
    target_include_directories(partyboard_switch_launcher PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${LIBNX}/include"
        "${PORTLIBS}/include"
        "${PORTLIBS}/include/freetype2"
    )
    target_link_directories(partyboard_switch_launcher PRIVATE
        "${LIBNX}/lib"
        "${PORTLIBS}/lib"
    )
    target_link_options(partyboard_switch_launcher PRIVATE -Wl,--gc-sections)
    # FreeType and HarfBuzz depend on each other in portlibs: link as a group.
    target_link_libraries(partyboard_switch_launcher PRIVATE
        -Wl,--start-group
        freetype harfbuzz bz2 png16 z
        glad EGL glapi drm_nouveau nx
        -Wl,--end-group
    )

    # romfs: the launcher's assets/ (catalogue, CubeShelf covers and logo) plus
    # PartyBoard's own artwork and fonts from res/, shared with the PC
    # pre-launch screen.
    set(PARTYBOARD_RES "${CMAKE_CURRENT_SOURCE_DIR}/../../../res")
    set(PARTYBOARD_LAUNCHER_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/romfs")
    set(PARTYBOARD_LAUNCHER_ROMFS_FILES
        "${PARTYBOARD_RES}/logo.png"
        "${PARTYBOARD_RES}/icon.png"
        "${PARTYBOARD_RES}/prelaunch-bg.png"
        "${PARTYBOARD_RES}/N64Party.otf"
        "${PARTYBOARD_RES}/FOT-NewRodin Pro DB.otf"
    )
    file(GLOB_RECURSE PARTYBOARD_LAUNCHER_ASSET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
    set(PARTYBOARD_LAUNCHER_ROMFS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/romfs.stamp")
    add_custom_command(
        OUTPUT "${PARTYBOARD_LAUNCHER_ROMFS_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PARTYBOARD_LAUNCHER_ROMFS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PARTYBOARD_LAUNCHER_ROMFS}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${PARTYBOARD_LAUNCHER_ROMFS}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PARTYBOARD_RES}/logo.png" "${PARTYBOARD_LAUNCHER_ROMFS}/logo.png"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PARTYBOARD_RES}/icon.png" "${PARTYBOARD_LAUNCHER_ROMFS}/icon.png"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PARTYBOARD_RES}/prelaunch-bg.png" "${PARTYBOARD_LAUNCHER_ROMFS}/prelaunch-bg.png"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PARTYBOARD_RES}/N64Party.otf" "${PARTYBOARD_LAUNCHER_ROMFS}/n64party.otf"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PARTYBOARD_RES}/FOT-NewRodin Pro DB.otf" "${PARTYBOARD_LAUNCHER_ROMFS}/rodin-db.otf"
        COMMAND "${CMAKE_COMMAND}" -E touch "${PARTYBOARD_LAUNCHER_ROMFS_STAMP}"
        DEPENDS ${PARTYBOARD_LAUNCHER_ROMFS_FILES} ${PARTYBOARD_LAUNCHER_ASSET_FILES}
        VERBATIM
        COMMENT "Staging PartyBoard launcher romfs"
    )

    set(PARTYBOARD_LAUNCHER_NACP "${CMAKE_CURRENT_BINARY_DIR}/partyboard-launcher.nacp")
    set(PARTYBOARD_LAUNCHER_NRO  "${CMAKE_CURRENT_BINARY_DIR}/partyboard-launcher.nro")
    set(PARTYBOARD_LAUNCHER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")

    add_custom_command(
        OUTPUT "${PARTYBOARD_LAUNCHER_NACP}"
        COMMAND "${PARTYBOARD_NACPTOOL}" --create
                "PartyBoard GameCube"
                "MarioPartyRD"
                "0.1.0"
                "${PARTYBOARD_LAUNCHER_NACP}"
        VERBATIM
        COMMENT "Generating PartyBoard launcher NACP"
    )
    add_custom_command(
        OUTPUT "${PARTYBOARD_LAUNCHER_NRO}"
        COMMAND "${PARTYBOARD_ELF2NRO}"
                "$<TARGET_FILE:partyboard_switch_launcher>"
                "${PARTYBOARD_LAUNCHER_NRO}"
                "--nacp=${PARTYBOARD_LAUNCHER_NACP}"
                "--icon=${PARTYBOARD_LAUNCHER_ICON}"
                "--romfsdir=${PARTYBOARD_LAUNCHER_ROMFS}"
        DEPENDS partyboard_switch_launcher "${PARTYBOARD_LAUNCHER_NACP}" "${PARTYBOARD_LAUNCHER_ICON}"
                "${PARTYBOARD_LAUNCHER_ROMFS_STAMP}"
        VERBATIM
        COMMENT "Generating PartyBoard launcher NRO"
    )
    add_custom_target(partyboard_switch_launcher_nro ALL
        DEPENDS "${PARTYBOARD_LAUNCHER_NRO}"
    )
    message(STATUS "  launcher:  ${PARTYBOARD_LAUNCHER_NRO}")
    return()
endif()

# ---- Desktop host: preview + tests ---------------------------------------

find_package(Freetype REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_library(PARTYBOARD_LAUNCHER_EGL EGL REQUIRED)
find_library(PARTYBOARD_LAUNCHER_GLES GLESv2 REQUIRED)

add_library(partyboard_launcher_core STATIC ${PARTYBOARD_LAUNCHER_CORE})
get_filename_component(PARTYBOARD_RES "${CMAKE_CURRENT_SOURCE_DIR}/../../../res" ABSOLUTE)
target_compile_definitions(partyboard_launcher_core PUBLIC
    PARTYBOARD_LAUNCHER_RES_DIR="${PARTYBOARD_RES}"
    PARTYBOARD_LAUNCHER_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_include_directories(partyboard_launcher_core PUBLIC src)
target_compile_options(partyboard_launcher_core PRIVATE ${PARTYBOARD_LAUNCHER_WARNINGS})
target_link_libraries(partyboard_launcher_core PUBLIC
    Freetype::Freetype PNG::PNG ZLIB::ZLIB
    ${PARTYBOARD_LAUNCHER_GLES} ${PARTYBOARD_LAUNCHER_EGL}
)

add_library(partyboard_launcher_demo STATIC host/demo_library.cpp)
target_include_directories(partyboard_launcher_demo PUBLIC host)
target_link_libraries(partyboard_launcher_demo PUBLIC partyboard_launcher_core)

add_executable(partyboard_launcher_preview host/preview_main.cpp)
target_compile_options(partyboard_launcher_preview PRIVATE ${PARTYBOARD_LAUNCHER_WARNINGS})
target_link_libraries(partyboard_launcher_preview PRIVATE partyboard_launcher_demo)

add_executable(partyboard_launcher_tests tests/launcher_tests.cpp)
target_compile_options(partyboard_launcher_tests PRIVATE ${PARTYBOARD_LAUNCHER_WARNINGS})
target_link_libraries(partyboard_launcher_tests PRIVATE partyboard_launcher_demo)

enable_testing()
add_test(NAME partyboard_launcher_tests COMMAND partyboard_launcher_tests)
