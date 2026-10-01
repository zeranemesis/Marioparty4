# SDL3 for libnx: Aurora's SDL3 release, plus the Switch backend in this
# directory, built with devkitPro's own CMake toolchain (whose
# CMAKE_SYSTEM_NAME, NintendoSwitch, is what the backend keys on).
#
# Defines the imported target SDL3::SDL3-static, like an SDL3 package would.

include_guard(GLOBAL)

include(ExternalProject)

# Keep in step with AURORA_SDL3_REF in extern/aurora/CMakeLists.txt.
set(PARTYBOARD_SWITCH_SDL3_REF "release-3.4.10" CACHE STRING
    "SDL3 tag built for the Switch (keep in step with AURORA_SDL3_REF)")

find_package(Git REQUIRED)

set(_pb_sdl3_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_pb_sdl3_prefix "${CMAKE_BINARY_DIR}/sdl3-switch")
set(_pb_sdl3_library "${_pb_sdl3_prefix}/lib/libSDL3.a")

ExternalProject_Add(partyboard_switch_sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG "${PARTYBOARD_SWITCH_SDL3_REF}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS FALSE
    UPDATE_DISCONNECTED TRUE
    # One git apply per patch: later ones edit files the first one creates.
    PATCH_COMMAND
        "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_pb_sdl3_dir}/0001-libnx-backend.patch"
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_pb_sdl3_dir}/0002-egl-context-attributes.patch"
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_pb_sdl3_dir}/0003-audio-keep-buffers-queued.patch"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${DEVKITPRO}/cmake/Switch.cmake"
        "-DCMAKE_INSTALL_PREFIX=${_pb_sdl3_prefix}"
        -DCMAKE_BUILD_TYPE=Release
        -DSDL_SHARED=OFF
        -DSDL_STATIC=ON
        -DSDL_TESTS=OFF
        -DSDL_EXAMPLES=OFF
        -DSDL_INSTALL_DOCS=OFF
    BUILD_BYPRODUCTS "${_pb_sdl3_library}"
)

# The include directory only exists after the install step; create it now so
# the imported target is valid at configure time.
file(MAKE_DIRECTORY "${_pb_sdl3_prefix}/include")

add_library(SDL3::SDL3-static STATIC IMPORTED GLOBAL)
set_target_properties(SDL3::SDL3-static PROPERTIES
    IMPORTED_LOCATION "${_pb_sdl3_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_pb_sdl3_prefix}/include"
    # The backend draws through switch-mesa (EGL + GLES) on libnx.
    INTERFACE_LINK_DIRECTORIES "${PORTLIBS}/lib;${LIBNX}/lib"
    INTERFACE_LINK_LIBRARIES "-Wl,--start-group;GLESv2;EGL;glapi;drm_nouveau;nx;m;-Wl,--end-group"
)
add_dependencies(SDL3::SDL3-static partyboard_switch_sdl3)
