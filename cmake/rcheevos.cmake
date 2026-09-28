# rcheevos, the official RetroAchievements client library (MIT).
#
# It ships no CMake build of its own, so the sources are listed here. Left out:
#  - rc_client_raintegration.c / rc_client_external.c: the Windows-only
#    RAIntegration DLL bridge, used by emulators that embed the developer
#    toolkit. The port is a player-facing client and does not load it.
#  - rc_libretro.c: libretro core memory maps, which have nothing to do with a
#    native port.
FetchContent_Declare(rcheevos
        URL https://github.com/RetroAchievements/rcheevos/archive/refs/tags/v12.5.0.tar.gz
        URL_HASH SHA256=e6df83de4e18f0a19206e711e1e589624dcfcf6bf529c14f6dd2bb2d1ced983f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# No CMakeLists.txt upstream, so this only downloads and extracts.
FetchContent_MakeAvailable(rcheevos)

set(RCHEEVOS_DIR ${rcheevos_SOURCE_DIR})
add_library(rcheevos STATIC
        ${RCHEEVOS_DIR}/src/rc_client.c
        ${RCHEEVOS_DIR}/src/rc_compat.c
        ${RCHEEVOS_DIR}/src/rc_util.c
        ${RCHEEVOS_DIR}/src/rc_version.c
        ${RCHEEVOS_DIR}/src/rapi/rc_api_common.c
        ${RCHEEVOS_DIR}/src/rapi/rc_api_editor.c
        ${RCHEEVOS_DIR}/src/rapi/rc_api_info.c
        ${RCHEEVOS_DIR}/src/rapi/rc_api_runtime.c
        ${RCHEEVOS_DIR}/src/rapi/rc_api_user.c
        ${RCHEEVOS_DIR}/src/rcheevos/alloc.c
        ${RCHEEVOS_DIR}/src/rcheevos/condition.c
        ${RCHEEVOS_DIR}/src/rcheevos/condset.c
        ${RCHEEVOS_DIR}/src/rcheevos/consoleinfo.c
        ${RCHEEVOS_DIR}/src/rcheevos/format.c
        ${RCHEEVOS_DIR}/src/rcheevos/lboard.c
        ${RCHEEVOS_DIR}/src/rcheevos/memref.c
        ${RCHEEVOS_DIR}/src/rcheevos/operand.c
        ${RCHEEVOS_DIR}/src/rcheevos/rc_validate.c
        ${RCHEEVOS_DIR}/src/rcheevos/richpresence.c
        ${RCHEEVOS_DIR}/src/rcheevos/runtime.c
        ${RCHEEVOS_DIR}/src/rcheevos/runtime_progress.c
        ${RCHEEVOS_DIR}/src/rcheevos/trigger.c
        ${RCHEEVOS_DIR}/src/rcheevos/value.c
        ${RCHEEVOS_DIR}/src/rhash/aes.c
        ${RCHEEVOS_DIR}/src/rhash/cdreader.c
        ${RCHEEVOS_DIR}/src/rhash/hash.c
        ${RCHEEVOS_DIR}/src/rhash/hash_disc.c
        ${RCHEEVOS_DIR}/src/rhash/hash_encrypted.c
        ${RCHEEVOS_DIR}/src/rhash/hash_rom.c
        ${RCHEEVOS_DIR}/src/rhash/hash_zip.c
        ${RCHEEVOS_DIR}/src/rhash/md5.c
)
target_include_directories(rcheevos PUBLIC ${RCHEEVOS_DIR}/include)
# Linked into the dol shared library.
set_target_properties(rcheevos PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "extern")
if (MSVC)
    target_compile_definitions(rcheevos PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(rcheevos PRIVATE /W0)
else ()
    target_compile_options(rcheevos PRIVATE -w)
endif ()
