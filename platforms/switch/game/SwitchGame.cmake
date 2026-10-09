# The game for libnx, built like the root CMakeLists.txt builds it on other
# platforms, except that everything is static: libnx has no dynamic loader.
# Stage one: the main module (`dol`: the decompiled game plus the port).

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/../aurora/SwitchAurora.cmake")

include(FetchContent)

# Same pins as the root CMakeLists.txt.
FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)
include("${CMAKE_SOURCE_DIR}/cmake/rcheevos.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/mbedtls.cmake")

add_subdirectory("${CMAKE_SOURCE_DIR}/extern/musyx" "${CMAKE_BINARY_DIR}/musyx" EXCLUDE_FROM_ALL)
target_include_directories(musyx PRIVATE "${CMAKE_SOURCE_DIR}/include")

set(VERSION 1)
configure_file("${CMAKE_SOURCE_DIR}/version.h.in" "${CMAKE_BINARY_DIR}/partyboard_version.h")

include("${CMAKE_SOURCE_DIR}/files.cmake")
foreach(_pb_list GAME_FILES PORT_FILES REL_FILES)
    list(TRANSFORM ${_pb_list} PREPEND "${CMAKE_SOURCE_DIR}/")
endforeach()

add_library(partyboard_switch_dol STATIC ${GAME_FILES} ${PORT_FILES} "${CMAKE_SOURCE_DIR}/extern/libco/libco.c")
set_property(SOURCE ${GAME_FILES} APPEND PROPERTY COMPILE_OPTIONS
    -include "${CMAKE_SOURCE_DIR}/include/port/msl_sqrtf.h")
target_compile_definitions(partyboard_switch_dol PRIVATE
    TARGET_PC NON_MATCHING OPTIMIZED_TEXTURE_LOADING BYTESWAPPING TARGET_DOL VERSION=${VERSION}
    MUSY_VERSION_MAJOR=1 MUSY_VERSION_MINOR=5 MUSY_VERSION_PATCH=4)
target_include_directories(partyboard_switch_dol PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/extern"
    "${CMAKE_SOURCE_DIR}/extern/aurora/include" "${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin"
    "${CMAKE_BINARY_DIR}")
FetchContent_GetProperties(tracy SOURCE_DIR _pb_tracy_src)
if(_pb_tracy_src)
    target_include_directories(partyboard_switch_dol PRIVATE "${_pb_tracy_src}/profiler/src")
endif()
target_link_libraries(partyboard_switch_dol PUBLIC
    aurora::core aurora::card aurora::dvd aurora::gx aurora::mtx aurora::os aurora::si aurora::vi aurora::pad
    musyx absl::flat_hash_map nlohmann_json::nlohmann_json fmt::fmt rmlui_debugger rcheevos mbedcrypto)

# Mbed TLS: see mbedtls_switch_config.h.
target_compile_definitions(mbedcrypto PUBLIC
    "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/mbedtls_switch_config.h\"")
target_sources(mbedcrypto PRIVATE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_switch_entropy.c")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/mbedtls_switch_entropy.c" PROPERTIES
    INCLUDE_DIRECTORIES "${LIBNX}/include")

# The port's netplay and test input talk BSD sockets, which libnx provides
# (<arpa/inet.h>, <netinet/in.h>, ...) in its own include directory.
target_include_directories(partyboard_switch_dol SYSTEM AFTER PRIVATE "${LIBNX}/include")

# ---- Overlays (RELs), linked into the executable ----------------------------
# objdll.c finds them in a generated table instead of opening shared
# libraries (include/port/static_overlays.h). Every translation unit that sees
# omDllData needs the same definition.
target_compile_definitions(partyboard_switch_dol PUBLIC PARTYBOARD_STATIC_OVERLAYS)
target_sources(partyboard_switch_dol PRIVATE "${CMAKE_SOURCE_DIR}/src/port/static_overlays.c")

set(PARTYBOARD_SWITCH_OVERLAYS "")
foreach(_pb_rel_file ${REL_FILES})
    get_filename_component(_pb_rel_dir "${_pb_rel_file}" DIRECTORY)
    get_filename_component(_pb_rel_name "${_pb_rel_dir}" NAME)
    if(_pb_rel_name STREQUAL "REL")
        continue() # shared sources such as board_executor.c
    endif()
    list(APPEND PARTYBOARD_SWITCH_OVERLAYS "${_pb_rel_name}")
endforeach()
list(REMOVE_DUPLICATES PARTYBOARD_SWITCH_OVERLAYS)

foreach(_pb_ovl ${PARTYBOARD_SWITCH_OVERLAYS})
    set(_pb_ovl_sources ${REL_FILES})
    list(FILTER _pb_ovl_sources INCLUDE REGEX "/src/REL/${_pb_ovl}/.*\\.c$")
    if(_pb_ovl MATCHES "^w..Dll")
        list(APPEND _pb_ovl_sources "${CMAKE_SOURCE_DIR}/src/REL/board_executor.c")
    endif()
    add_library(pb_ovl_${_pb_ovl} OBJECT ${_pb_ovl_sources})
    target_compile_definitions(pb_ovl_${_pb_ovl} PRIVATE
        TARGET_PC OPTIMIZED_TEXTURE_LOADING BYTESWAPPING NON_MATCHING VERSION=${VERSION} PARTYBOARD_STATIC_OVERLAYS)
    target_compile_options(pb_ovl_${_pb_ovl} PRIVATE -include "${CMAKE_SOURCE_DIR}/include/port/msl_sqrtf.h")
    target_include_directories(pb_ovl_${_pb_ovl} PRIVATE
        "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/extern"
        "${CMAKE_SOURCE_DIR}/extern/aurora/include" "${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin")
    target_link_libraries(pb_ovl_${_pb_ovl} PRIVATE musyx aurora::mtx aurora::gx)
endforeach()

# Each overlay becomes one relocatable object whose only global definition is
# PartyBoardOverlay_<name>_ObjectSetup, with its data and bss in
# pb_ovl_<name>_data and pb_ovl_<name>_bss.
set(_pb_ld "${DEVKITA64}/bin/aarch64-none-elf-ld")
set(_pb_objcopy "${DEVKITA64}/bin/aarch64-none-elf-objcopy")
set(_pb_ovl_dir "${CMAKE_CURRENT_BINARY_DIR}/overlays")
file(MAKE_DIRECTORY "${_pb_ovl_dir}")
set(_pb_ovl_objects "")
set(_pb_ovl_decls "")
set(_pb_ovl_entries "")
foreach(_pb_ovl ${PARTYBOARD_SWITCH_OVERLAYS})
    set(_pb_merged "${_pb_ovl_dir}/${_pb_ovl}.merged.o")
    set(_pb_local "${_pb_ovl_dir}/${_pb_ovl}.local.o")
    set(_pb_final "${_pb_ovl_dir}/${_pb_ovl}.o")
    add_custom_command(
        OUTPUT "${_pb_final}"
        COMMAND "${_pb_ld}" -r -T "${CMAKE_CURRENT_LIST_DIR}/overlay.ld" -o "${_pb_merged}" $<TARGET_OBJECTS:pb_ovl_${_pb_ovl}>
        COMMAND "${_pb_objcopy}" --keep-global-symbol=ObjectSetup "${_pb_merged}" "${_pb_local}"
        COMMAND "${_pb_objcopy}"
                --redefine-sym ObjectSetup=PartyBoardOverlay_${_pb_ovl}_ObjectSetup
                --rename-section pb_ovl_data=pb_ovl_${_pb_ovl}_data
                --rename-section pb_ovl_bss=pb_ovl_${_pb_ovl}_bss
                "${_pb_local}" "${_pb_final}"
        DEPENDS pb_ovl_${_pb_ovl} $<TARGET_OBJECTS:pb_ovl_${_pb_ovl}> "${CMAKE_CURRENT_LIST_DIR}/overlay.ld"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Linking overlay ${_pb_ovl} for the executable")
    list(APPEND _pb_ovl_objects "${_pb_final}")
    string(APPEND _pb_ovl_decls
        "extern void PartyBoardOverlay_${_pb_ovl}_ObjectSetup(void);\n"
        "extern unsigned char __start_pb_ovl_${_pb_ovl}_data[] __attribute__((weak));\n"
        "extern unsigned char __stop_pb_ovl_${_pb_ovl}_data[] __attribute__((weak));\n"
        "extern unsigned char __start_pb_ovl_${_pb_ovl}_bss[] __attribute__((weak));\n"
        "extern unsigned char __stop_pb_ovl_${_pb_ovl}_bss[] __attribute__((weak));\n")
    string(APPEND _pb_ovl_entries
        "    {\"${_pb_ovl}.so\", PartyBoardOverlay_${_pb_ovl}_ObjectSetup,\n"
        "     __start_pb_ovl_${_pb_ovl}_data, __stop_pb_ovl_${_pb_ovl}_data,\n"
        "     __start_pb_ovl_${_pb_ovl}_bss, __stop_pb_ovl_${_pb_ovl}_bss, 0},\n")
endforeach()
set_source_files_properties(${_pb_ovl_objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

list(LENGTH PARTYBOARD_SWITCH_OVERLAYS _pb_ovl_count)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/static_overlay_table.c" CONTENT
"/* Generated by platforms/switch/game/SwitchGame.cmake. */
#include \"port/static_overlays.h\"

${_pb_ovl_decls}
PartyBoardStaticOverlay PartyBoard_StaticOverlays[] = {
${_pb_ovl_entries}};

const unsigned PartyBoard_StaticOverlayCount = ${_pb_ovl_count};
" @ONLY)

# ---- The executable ----------------------------------------------------------
add_executable(partyboard_switch_game
    "${CMAKE_SOURCE_DIR}/src/port/entry.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/static_overlay_table.c"
    ${_pb_ovl_objects})
set_target_properties(partyboard_switch_game PROPERTIES OUTPUT_NAME "partyboard" SUFFIX ".elf")
target_compile_definitions(partyboard_switch_game PRIVATE TARGET_PC NON_MATCHING VERSION=${VERSION})
target_include_directories(partyboard_switch_game PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_BINARY_DIR}")
target_link_options(partyboard_switch_game PRIVATE -Wl,--gc-sections)
target_link_libraries(partyboard_switch_game PRIVATE partyboard_switch_dol aurora::main)
target_sources(partyboard_switch_game PRIVATE "${CMAKE_CURRENT_LIST_DIR}/switch_app_init.c")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/switch_app_init.c" PROPERTIES
    INCLUDE_DIRECTORIES "${LIBNX}/include")

# ---- NRO -------------------------------------------------------------------
# romfs: the port's res/ directory, opened as "res/..." from romfs:/.
set(_pb_game_romfs "${CMAKE_CURRENT_BINARY_DIR}/game-romfs")
file(GLOB_RECURSE _pb_game_res CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/res/*")
set(_pb_game_romfs_stamp "${CMAKE_CURRENT_BINARY_DIR}/game-romfs.stamp")
add_custom_command(
    OUTPUT "${_pb_game_romfs_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_pb_game_romfs}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_pb_game_romfs}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/res" "${_pb_game_romfs}/res"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_pb_game_romfs_stamp}"
    DEPENDS ${_pb_game_res}
    VERBATIM
    COMMENT "Staging PartyBoard romfs")

set(_pb_game_nacp "${CMAKE_CURRENT_BINARY_DIR}/partyboard.nacp")
set(_pb_game_nro "${CMAKE_CURRENT_BINARY_DIR}/partyboard.nro")
add_custom_command(
    OUTPUT "${_pb_game_nacp}"
    COMMAND "${PARTYBOARD_NACPTOOL}" --create "PartyBoard" "MarioPartyRD" "${PARTY_BOARD_VERSION_STRING}" "${_pb_game_nacp}"
    VERBATIM
    COMMENT "Generating PartyBoard NACP")
add_custom_command(
    OUTPUT "${_pb_game_nro}"
    COMMAND "${PARTYBOARD_ELF2NRO}" "$<TARGET_FILE:partyboard_switch_game>" "${_pb_game_nro}"
            "--nacp=${_pb_game_nacp}"
            "--icon=${CMAKE_CURRENT_LIST_DIR}/icon.jpg"
            "--romfsdir=${_pb_game_romfs}"
    DEPENDS partyboard_switch_game "${_pb_game_nacp}" "${CMAKE_CURRENT_LIST_DIR}/icon.jpg" "${_pb_game_romfs_stamp}"
    VERBATIM
    COMMENT "Generating PartyBoard NRO")
add_custom_target(partyboard_switch_game_nro DEPENDS "${_pb_game_nro}")
