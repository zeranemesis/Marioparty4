cmake_minimum_required(VERSION 3.25)

# The Switch's Aurora probe (../../source/aurora_probe.c) on a desktop host,
# on Dawn's OpenGL ES backend at the Compatibility feature level: the
# renderer configuration the Switch runs, checkable with Mesa and no console.
#
#   cmake -S platforms/switch/aurora/host -B build/aurora-host
#   cmake --build build/aurora-host
#   xvfb-run -s "-screen 0 1280x960x24" build/aurora-host/aurora_probe --frames 120
#
# extern/aurora must carry patches/aurora-*.patch first (Switch last), as on CI.
project(partyboard_aurora_probe_host LANGUAGES C CXX)

set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)

# A plain X11 window is all the probe needs (Xvfb on CI): keep SDL's
# optional X11 extensions and Wayland out of the dependencies.
foreach(_pb_sdl_opt SDL_X11_XCURSOR SDL_X11_XDBE SDL_X11_XINPUT SDL_X11_XFIXES SDL_X11_XRANDR
                    SDL_X11_XSCRNSAVER SDL_X11_XSHAPE SDL_X11_XSYNC SDL_X11_XTEST SDL_WAYLAND)
    set(${_pb_sdl_opt} OFF CACHE BOOL "" FORCE)
endforeach()

get_filename_component(_pb_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_subdirectory("${_pb_root}/extern/aurora" aurora EXCLUDE_FROM_ALL)

add_executable(aurora_probe "${_pb_root}/platforms/switch/source/aurora_probe.c")
target_link_libraries(aurora_probe PRIVATE
    aurora::core aurora::gx aurora::main aurora::mtx aurora::pad aurora::vi)
