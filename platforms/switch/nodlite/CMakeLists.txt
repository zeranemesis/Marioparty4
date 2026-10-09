cmake_minimum_required(VERSION 3.20)

# nodlite: nod's C API (include/nod.h, nod v2.0.0-alpha.10) for GameCube
# discs, in C++ with no Rust, so Aurora's DVD layer and PartyBoard's disc
# validation build for libnx. See README.md.
#
# Standalone on a desktop host it also builds its tests:
#   cmake -S platforms/switch/nodlite -B build/nodlite && cmake --build build/nodlite
#   ctest --test-dir build/nodlite

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nodlite LANGUAGES C CXX)
    set(NODLITE_STANDALONE ON)
endif()

add_library(nodlite STATIC
    src/container.cpp
    src/lfg.cpp
    src/nodlite.cpp
)
add_library(nod::nod ALIAS nodlite)
target_include_directories(nodlite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                   PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(nodlite PUBLIC cxx_std_20)
target_compile_options(nodlite PRIVATE -Wall -Wextra)

if(PARTYBOARD_SWITCH)
    # devkitPro portlibs: switch-zstd, switch-zlib, switch-bzip2.
    target_include_directories(nodlite PRIVATE "${PORTLIBS}/include")
    target_link_directories(nodlite PUBLIC "${PORTLIBS}/lib")
    target_link_libraries(nodlite PUBLIC zstd bz2 z)
else()
    find_package(ZLIB REQUIRED)
    find_package(BZip2 REQUIRED)
    find_path(NODLITE_ZSTD_INCLUDE zstd.h REQUIRED)
    find_library(NODLITE_ZSTD_LIBRARY zstd REQUIRED)
    target_include_directories(nodlite PRIVATE "${NODLITE_ZSTD_INCLUDE}")
    target_link_libraries(nodlite PUBLIC ZLIB::ZLIB BZip2::BZip2 "${NODLITE_ZSTD_LIBRARY}")
endif()

if(NODLITE_STANDALONE)
    find_package(Threads REQUIRED)
    add_executable(nodlite_tests tests/nodlite_tests.cpp)
    target_include_directories(nodlite_tests PRIVATE src)
    target_compile_options(nodlite_tests PRIVATE -Wall -Wextra)
    target_link_libraries(nodlite_tests PRIVATE nodlite Threads::Threads)
    enable_testing()
    add_test(NAME nodlite_tests
             COMMAND nodlite_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
