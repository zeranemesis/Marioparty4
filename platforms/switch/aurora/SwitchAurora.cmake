# Aurora for libnx: extern/aurora built against the Switch Dawn
# (dawn/SwitchDawn.cmake), the Switch SDL3 (sdl3/SwitchSDL3.cmake) and
# devkitPro's portlibs, with patches/aurora-switch.patch applied to the
# submodule (after the PartyBoard patches, as CI does).

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/../dawn/SwitchDawn.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../sdl3/SwitchSDL3.cmake")

set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
if(PARTYBOARD_SWITCH_GAME)
    # The game's UI is RmlUi, and its discs go through Aurora's DVD layer,
    # which reads them with nod: nodlite on the Switch (nod::nod, added by
    # platforms/switch/CMakeLists.txt before this file).
    set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
    set(AURORA_NOD_PROVIDER "system" CACHE STRING "" FORCE)
    set(AURORA_NOD_LINKAGE "static" CACHE STRING "" FORCE)
    set(nod_FOUND TRUE)
else()
    set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
endif()

# Dawn: the "vendor" provider reuses an existing webgpu_dawn target and keeps
# the DAWN_ENABLE_* values of that build (OpenGL ES only).
set(AURORA_DAWN_PROVIDER "vendor" CACHE STRING "" FORCE)
set(AURORA_DAWN_LINKAGE "static" CACHE STRING "" FORCE)
# SDL3: the "system" provider takes SDL3::SDL3-static once SDL3 is "found".
set(AURORA_SDL3_PROVIDER "system" CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE "static" CACHE STRING "" FORCE)
set(SDL3_FOUND TRUE)

# portlibs' FreeType is built with HarfBuzz, bzip2 and libpng: give the target
# its whole link interface before FindFreetype makes a bare one.
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype STATIC IMPORTED GLOBAL)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION "${PORTLIBS}/lib/libfreetype.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PORTLIBS}/include/freetype2"
        INTERFACE_LINK_DIRECTORIES "${PORTLIBS}/lib"
        INTERFACE_LINK_LIBRARIES "-Wl,--start-group;harfbuzz;freetype;bz2;png16;z;-Wl,--end-group"
    )
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/extern/aurora" "${CMAKE_BINARY_DIR}/aurora" EXCLUDE_FROM_ALL)

# Tracy: see PatchTracyForSwitch.cmake.
FetchContent_GetProperties(tracy SOURCE_DIR _pb_tracy_source_dir)
if(_pb_tracy_source_dir)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DTRACY_SOURCE_DIR=${_pb_tracy_source_dir}"
                -P "${CMAKE_CURRENT_LIST_DIR}/PatchTracyForSwitch.cmake"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

# SQLite (Aurora builds the amalgamation as `sqlite3`): no unix VFS on libnx,
# see sqlite_vfs_switch.c.
if(TARGET sqlite3)
    get_target_property(_pb_sqlite_alias sqlite3 ALIASED_TARGET)
    if(NOT _pb_sqlite_alias)
        target_sources(sqlite3 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/sqlite_vfs_switch.c")
        target_compile_definitions(sqlite3 PRIVATE
            SQLITE_OS_OTHER=1
            SQLITE_THREADSAFE=1
            SQLITE_TEMP_STORE=3
            SQLITE_OMIT_WAL=1
            SQLITE_OMIT_LOAD_EXTENSION=1
            SQLITE_MAX_MMAP_SIZE=0
        )
    endif()
endif()

# patches/aurora-switch.patch uses switch-mesa's EGL headers from portlibs.
target_include_directories(aurora_core SYSTEM AFTER PRIVATE "${PORTLIBS}/include")

# Dear ImGui's default "open in shell" forks and execs; libnx has neither.
if(TARGET imgui)
    target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
endif()
