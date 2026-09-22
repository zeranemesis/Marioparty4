cmake_minimum_required(VERSION 3.25)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

# obtain revision info from git
find_package(Git)
if (GIT_FOUND)
    # make sure version information gets re-run when the current Git HEAD changes
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
            OUTPUT_VARIABLE partyboard_git_head_filename
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${partyboard_git_head_filename}")

    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --symbolic-full-name HEAD
            OUTPUT_VARIABLE partyboard_git_head_symbolic
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${partyboard_git_head_symbolic}
            OUTPUT_VARIABLE partyboard_git_head_symbolic_filename
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${partyboard_git_head_symbolic_filename}")

    # defines PARTY_BOARD_WC_REVISION
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            OUTPUT_VARIABLE PARTY_BOARD_WC_REVISION
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    # defines PARTY_BOARD_WC_DESCRIBE
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} describe --tags --long --dirty --match "v*"
            OUTPUT_VARIABLE PARTY_BOARD_WC_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    # remove the git hash, then collapse a clean "-0" suffix only
    string(REGEX REPLACE "-[^-]+(-dirty|)$" "\\1" PARTY_BOARD_WC_DESCRIBE "${PARTY_BOARD_WC_DESCRIBE}")
    string(REGEX REPLACE "-0$" "" PARTY_BOARD_WC_DESCRIBE "${PARTY_BOARD_WC_DESCRIBE}")

    # defines PARTY_BOARD_WC_BRANCH
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE PARTY_BOARD_WC_BRANCH
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    # defines PARTY_BOARD_WC_DATE
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} log -1 --format=%ad
            OUTPUT_VARIABLE PARTY_BOARD_WC_DATE
            OUTPUT_STRIP_TRAILING_WHITESPACE)
else ()
    message(STATUS "Unable to find git, commit information will not be available")
endif ()

if (PARTY_BOARD_WC_DESCRIBE MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+).*)?$")
    set(PARTY_BOARD_SHORT_VERSION_STRING "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    if (CMAKE_MATCH_5)
        set(PARTY_BOARD_VERSION_STRING "${PARTY_BOARD_SHORT_VERSION_STRING}.${CMAKE_MATCH_5}")
    else ()
        set(PARTY_BOARD_VERSION_STRING "${PARTY_BOARD_SHORT_VERSION_STRING}.0")
    endif ()
else ()
    set(PARTY_BOARD_WC_DESCRIBE "UNKNOWN-VERSION")
    set(PARTY_BOARD_VERSION_STRING "0.0.0.0")
    set(PARTY_BOARD_SHORT_VERSION_STRING "0.0.0")
endif ()

# Add version information to CI environment variables
if(DEFINED ENV{GITHUB_ENV})
    file(APPEND "$ENV{GITHUB_ENV}" "PARTY_BOAD_VERSION=${PARTY_BOARD_WC_DESCRIBE}\n")
endif()

message(STATUS "Party Board version set to ${PARTY_BOARD_WC_DESCRIBE}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
project(partyboard LANGUAGES C CXX VERSION ${PARTY_BOARD_VERSION_STRING})
if (APPLE)
    enable_language(OBJC)
endif ()
if (APPLE AND NOT TVOS AND CMAKE_SYSTEM_NAME STREQUAL tvOS)
    # ios.toolchain.cmake hack for SDL
    set(TVOS ON)
    set(IOS OFF)
endif ()

if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    # If the Xcode SDK is lagging behind system version, CMake needs this done first
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

# Aurora doesn't have prebuilts for x86_64 macOS and arm64 Windows yet
if (APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    set(AURORA_DAWN_PROVIDER "vendor" CACHE STRING "Dawn provider" FORCE)
endif ()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_cmake")

include(FetchContent)

FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

set(VERSION 1)
set(PARTY_BOARD_BUNDLE_NAME PartyBoard)
set(PARTY_BOARD_BUNDLE_IDENTIFIER com.mariopartyrd.partyboard)
set(PARTY_BOARD_COMPANY_NAME "Mario Party R&D")
set(PARTY_BOARD_FILE_DESCRIPTION "Party Board")
set(PARTY_BOARD_PRODUCT_NAME "PartyBoard")
set(PARTY_BOARD_COPYRIGHT "Copyright (C) Mario Party R&D contributors")

option(USE_ASAN "Enable address sanitization" OFF)
option(SKIP_HU_ALLOC "Skip the game's built in allocator" OFF)
option(RUNTIME_SANITIZE "Runtime (stack) sanitization" OFF)
option(PARTYBOARD_EXPERIMENTAL_MUSYX_AUDIO "Enable the experimental MusyX audio backend" OFF)

# ASan stays off by default, but the switch has to be reachable: this line used
# to FORCE it off, which silently discarded -DUSE_ASAN=ON and made the option
# below decorative. A build that asks for it explicitly now gets it.
if (NOT USE_ASAN)
    set(USE_ASAN OFF CACHE BOOL "Disable ASan" FORCE)
endif ()

if (USE_ASAN AND MSVC)
    # /fsanitize=address turns on the MSVC STL container annotations, and that
    # choice has to match across every object in the link. Only the game, the
    # port and the overlays are instrumented, so abseil, aurora, fmt and rmlui
    # disagreed and the link failed with LNK2038 mismatches on annotate_string,
    # annotate_vector and annotate_optional.
    #
    # _DISABLE_STL_ANNOTATION turns all of them off on both sides at once,
    # which is the documented remedy and covers any container Microsoft
    # annotates later. It costs exactly one class of detection: overflows
    # INSIDE a std::string, std::vector or std::optional buffer. Nothing this
    # investigation needs is affected, namely heap-buffer-overflow,
    # stack-buffer-overflow, global-buffer-overflow, use-after-free,
    # double-free and invalid-free. The corruption being hunted is in the
    # game's own arrays and in HuMem, not in an STL container.
    #
    # Set before any add_subdirectory or FetchContent call, so the dependencies
    # built from source pick it up too.
    add_compile_definitions(_DISABLE_STL_ANNOTATION=1)

    # Instrument everything that is built from source, not just our own code.
    # abseil changes the layout of raw_hash_set under
    # ABSL_HAVE_ADDRESS_SANITIZER, so an instrumented translation unit that
    # inlines the header and an uninstrumented library that defines the
    # out-of-line parts disagree about the object. That is an ODR violation,
    # and it showed up as a null dereference inside find_non_soo the moment
    # partyboard::config::Register touched the map at start-up. The same hazard
    # exists for any header-heavy dependency, so the flag goes on globally
    # before the subdirectories rather than target by target.
    #
    # Prebuilt packages (nod) and anything shipped as a binary cannot be
    # instrumented; that residual inconsistency is documented rather than
    # hidden, and it does not involve shared C++ templates.
    add_compile_options("/fsanitize=address")
endif ()
set(AURORA_ENABLE_DVD ON)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "Enable RmlUi UI support" FORCE)
set(RUNTIME_SANITIZE OFF)
set(SKIP_HU_ALLOC OFF CACHE BOOL "Skip the built in allocator" FORCE)

# Set build type to RelWithDebInfo if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()

if ((CMAKE_SYSTEM_NAME STREQUAL Linux) OR APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_BUILD_RPATH "$ORIGIN")
elseif (MSVC)
    add_compile_options(/bigobj)
endif ()

add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
add_subdirectory(extern/musyx EXCLUDE_FROM_ALL)
# MusyX carries the bank lifetime diagnostic's hooks, so it needs the port
# header that declares them. Added from here rather than inside the submodule so
# the submodule keeps building on its own.
target_include_directories(musyx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# dol is a SHARED library, so every static library linked into it has to be
# position independent. The submodule's own CMakeLists does not ask for that,
# and on Linux the link fails the moment MusyX's objects are really pulled in:
#
#   libmusyxRD.a(hw_pc.c.o): relocation R_X86_64_PC32 against symbol
#   `salAudioDumpFrameCount' can not be used when making a shared object
#
# Set from here, like the include path above, so the submodule keeps building
# on its own. A no-op on Windows.
set_property(TARGET musyx PROPERTY POSITION_INDEPENDENT_CODE ON)

# The pinned Tracy dependency already provides stb_image, which is used by the
# portable THP movie player for the JPEG video component.
FetchContent_GetProperties(tracy)

include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)

configure_file(${CMAKE_SOURCE_DIR}/version.h.in ${CMAKE_BINARY_DIR}/partyboard_version.h)
include(files.cmake)

source_group("Game" FILES ${GAME_FILES})
source_group("Port" FILES ${PORT_FILES})

add_library(dol SHARED ${GAME_FILES} ${PORT_FILES} extern/libco/libco.c)
set_target_properties(dol PROPERTIES C_VISIBILITY_PRESET default CXX_VISIBILITY_PRESET default)
target_compile_definitions(dol PRIVATE TARGET_PC NON_MATCHING OPTIMIZED_TEXTURE_LOADING BYTESWAPPING TARGET_DOL VERSION=${VERSION} MUSY_VERSION_MAJOR=1 MUSY_VERSION_MINOR=5 MUSY_VERSION_PATCH=4)
target_include_directories(dol PRIVATE include extern extern/aurora/include extern/aurora/include/dolphin ${CMAKE_BINARY_DIR})
if (tracy_SOURCE_DIR)
    target_include_directories(dol PRIVATE ${tracy_SOURCE_DIR}/profiler/src)
endif()
target_link_libraries(dol PUBLIC aurora::core aurora::card aurora::dvd aurora::gx aurora::mtx aurora::os aurora::si aurora::vi aurora::pad musyx absl::flat_hash_map nlohmann_json::nlohmann_json fmt::fmt rmlui_debugger)
if (WIN32)
    target_link_libraries(dol PUBLIC ws2_32)
    # dbghelp is loaded lazily by the crash reporter for MiniDumpWriteDump, but
    # StackWalk64 and the symbol helpers are linked directly.
    target_link_libraries(dol PUBLIC dbghelp)
    # Give every libco coroutine stack its own guarded reservation instead of a
    # bare malloc block. libco falls back to malloc only when these are
    # undefined (extern/libco/settings.h), so overriding them here needs no
    # change inside libco itself. See include/port/coroutine_stack.h.
    # libco.c is C and would otherwise have no prototype for the replacements,
    # so the declarations are force-included into that one translation unit.
    if (MSVC)
        set(PARTY_BOARD_LIBCO_FORCE_INCLUDE "/FIport/coroutine_stack_alloc.h")
    else ()
        set(PARTY_BOARD_LIBCO_FORCE_INCLUDE "-include" "port/coroutine_stack_alloc.h")
    endif ()
    set_source_files_properties(extern/libco/libco.c PROPERTIES
            COMPILE_DEFINITIONS "LIBCO_MALLOC=PartyBoard_CoroutineStackAlloc;LIBCO_FREE=PartyBoard_CoroutineStackFree"
            COMPILE_OPTIONS "${PARTY_BOARD_LIBCO_FORCE_INCLUDE}")
endif ()
# TODO it doesn't work on Linux yet
if (WIN32)
    if(USE_ASAN)
        target_compile_definitions(dol PRIVATE USE_ASAN)
        # The instrumentation flag itself is applied globally above, so the
        # game, the port, the overlays and every dependency built from source
        # are consistent. Only the USE_ASAN define is target-scoped.
    endif()
    if(SKIP_HU_ALLOC)
        target_compile_definitions(dol PRIVATE SKIP_HU_ALLOC)
    endif()
endif()
if (MSVC)
    target_link_options(dol PRIVATE "/DEF:${CMAKE_SOURCE_DIR}/dol.def")
    # Without this, editing the export list does not relink dol and the
    # executable fails on a symbol that looks like it was just added.
    set_property(TARGET dol APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/dol.def")
    # target_compile_options(dol PRIVATE "/WX")
    target_compile_options(dol PRIVATE "/wd4244") # implicit narrowing conversion
    target_compile_options(dol PRIVATE "/wd4101") # unreferenced local variable
    target_compile_options(dol PRIVATE "/wd4102") # unreferenced label
    target_compile_options(dol PRIVATE "/wd4305") # truncation
    target_compile_options(dol PRIVATE "/wd4018") # signed/unsigned mismatch
    target_compile_options(dol PRIVATE "/wd4996") # unsafe sprintf, etc
    target_compile_options(dol PRIVATE "/wd4146") # unary minus operator applied to unsigned type, result still unsigned
    target_compile_options(dol PRIVATE "/wd4552") # result of expression not used
    target_compile_options(dol PRIVATE "/wd4553") # result of expression not used: == vs =
    target_compile_options(dol PRIVATE "/wd4113") # function pointer parameter list mismatch
    # target_compile_options(dol PRIVATE "/Zi")
    #    target_compile_options(dol PRIVATE "/Ob0")
    target_compile_options(dol PRIVATE "/Od")
    if(NOT USE_ASAN AND RUNTIME_SANITIZE)
        target_compile_options(dol PRIVATE "/GS")
        target_compile_options(dol PRIVATE "/RTC1")  # incompatible with asan
    endif()
else ()
    # target_compile_options(dol PRIVATE "-fvisibility=hidden")
    # target_link_options(dol PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/dol.map")
endif ()

if(ANDROID)
    add_library(partyboard SHARED src/port/entry.cpp)
    set_target_properties(partyboard PROPERTIES OUTPUT_NAME main)
else ()
    add_executable(partyboard src/port/entry.cpp)
endif ()
target_compile_definitions(partyboard PRIVATE TARGET_PC NON_MATCHING VERSION=${VERSION})
target_include_directories(partyboard PRIVATE include ${CMAKE_BINARY_DIR})
target_link_libraries(partyboard PRIVATE dol aurora::main)
aurora_copy_runtime_dlls(partyboard dol)

if (USE_ASAN AND MSVC)
    # The launcher itself is instrumented too, so the sanitizer runtime is
    # initialised before any game module loads rather than by whichever DLL
    # happens to come first.
    # ASan cannot coexist with incremental linking, and CMake turns that on by
    # default for RelWithDebInfo. Without this the link fails outright.
    foreach (asan_target dol partyboard)
        target_link_options(${asan_target} PRIVATE "/INCREMENTAL:NO")
    endforeach ()
    # Ship the sanitizer runtime beside the executable: the game is launched
    # from its output directory, not from a developer command prompt, so the
    # DLL is not otherwise on the search path.
    get_filename_component(PARTY_BOARD_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(PARTY_BOARD_ASAN_DLL "${PARTY_BOARD_MSVC_BIN}/clang_rt.asan_dynamic-x86_64.dll")
    if (EXISTS "${PARTY_BOARD_ASAN_DLL}")
        add_custom_command(TARGET partyboard POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PARTY_BOARD_ASAN_DLL}" "$<TARGET_FILE_DIR:partyboard>"
                COMMENT "Copying the AddressSanitizer runtime")
    else ()
        message(WARNING "AddressSanitizer runtime not found at ${PARTY_BOARD_ASAN_DLL};"
                " the instrumented build will not start.")
    endif ()
endif ()

list(APPEND BINARY_TARGETS partyboard dol)

set(REL_TARGETS "")
foreach (rel_file ${REL_FILES})
    get_filename_component(rel_dir ${rel_file} DIRECTORY)
    get_filename_component(rel_target ${rel_dir} NAME)
    list(APPEND REL_TARGETS ${rel_target})
endforeach ()
list(REMOVE_DUPLICATES REL_TARGETS)

# Add RELs
foreach (dir ${REL_TARGETS})
    if (IS_DIRECTORY ${CMAKE_SOURCE_DIR}/src/REL/${dir})
        set(REL_SOURCE_FILES ${REL_FILES})
        list(FILTER REL_SOURCE_FILES INCLUDE REGEX "^src/REL/${dir}/.*\\.c$")

        if (REL_SOURCE_FILES)
            add_library(${dir} SHARED ${REL_SOURCE_FILES})
            list(APPEND BINARY_TARGETS ${dir})
            if (USE_ASAN)
                target_compile_definitions(${dir} PRIVATE USE_ASAN)
                if (MSVC)
                    # Same incompatibility as the main targets: ASan and
                    # incremental linking cannot both be on.
                    target_link_options(${dir} PRIVATE "/INCREMENTAL:NO")
                endif ()
            endif()
            if (dir MATCHES "^w..Dll")
                target_sources(${dir} PRIVATE src/REL/board_executor.c)
            endif()
            target_compile_definitions(${dir} PRIVATE TARGET_PC OPTIMIZED_TEXTURE_LOADING BYTESWAPPING NON_MATCHING VERSION=${VERSION})
            target_include_directories(${dir} PRIVATE include extern extern/aurora/include extern/aurora/include/dolphin)
            if (MSVC)
                set_target_properties(${dir} PROPERTIES LINK_FLAGS "/EXPORT:ObjectSetup")
                # target_compile_options(${dir} PRIVATE "/WX")
                target_compile_options(${dir} PRIVATE "/wd4244") # implicit narrowing conversion
                target_compile_options(${dir} PRIVATE "/wd4101") # unreferenced local variable
                target_compile_options(${dir} PRIVATE "/wd4102") # unreferenced label
                target_compile_options(${dir} PRIVATE "/wd4305") # truncation
                target_compile_options(${dir} PRIVATE "/wd4018") # signed/unsigned mismatch
                target_compile_options(${dir} PRIVATE "/wd4996") # unsafe sprintf, etc
                target_compile_options(${dir} PRIVATE "/wd4146") # unary minus operator applied to unsigned type, result still unsigned
                target_compile_options(${dir} PRIVATE "/wd4552") # result of expression not used
                target_compile_options(${dir} PRIVATE "/wd4553") # result of expression not used: == vs =
                target_compile_options(${dir} PRIVATE "/wd4113") # function pointer parameter list mismatch
                target_compile_options(${dir} PRIVATE "/wd4068") # unknown pragma
                # target_compile_options(${dir} PRIVATE "/Zi")
                #                target_compile_options(${dir} PRIVATE "/Ob0")
                target_compile_options(${dir} PRIVATE "/Od")
                if(NOT USE_ASAN AND RUNTIME_SANITIZE)
                    target_compile_options(${dir} PRIVATE "/GS")
                    target_compile_options(${dir} PRIVATE "/RTC1")  # incompatible with asan
                endif()
            elseif (NOT WIN32)
                # target_compile_options(${dir} PRIVATE "-fvisibility=hidden")
                if (APPLE)
                    target_link_options(${dir} PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/rel_symbols.txt")
                else()
                    target_link_options(${dir} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/rel.map")
                endif()
                set_target_properties(${dir} PROPERTIES PREFIX "")
            endif ()
            target_link_libraries(${dir} PRIVATE dol musyx aurora::mtx aurora::gx)
            add_dependencies(partyboard ${dir})
        endif ()
    endif ()
endforeach ()

if (ANDROID)
    # SDLActivity loads SDL_main via dlsym on Android. Since aurora::main is a static
    # archive, force an undefined reference so the linker keeps the SDL_main object.
    target_link_options(partyboard PRIVATE "-Wl,-u,SDL_main")
endif ()

if (NOT APPLE)
    add_custom_command(TARGET partyboard POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/res"
            "$<TARGET_FILE_DIR:partyboard>/res"
            COMMENT "Copying resources"
    )
endif ()
#
if (WIN32)
    set(PARTY_BOARD_WINDOWS_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows)
    set(PARTY_BOARD_WINDOWS_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/res/icon.png)
    set(PARTY_BOARD_WINDOWS_ICON_ICO ${CMAKE_CURRENT_BINARY_DIR}/partyboard.ico)
    set(PARTY_BOARD_WINDOWS_RC ${CMAKE_CURRENT_BINARY_DIR}/partyboard.rc)
    set(PARTY_BOARD_WINDOWS_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/partyboard.manifest)

    add_custom_command(
            OUTPUT ${PARTY_BOARD_WINDOWS_ICON_ICO}
            COMMAND powershell -ExecutionPolicy Bypass -File
            ${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/Create-IcoFromPng.ps1
            -InputPng ${PARTY_BOARD_WINDOWS_ICON_PNG}
            -OutputIco ${PARTY_BOARD_WINDOWS_ICON_ICO}
            DEPENDS ${PARTY_BOARD_WINDOWS_ICON_PNG} ${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/Create-IcoFromPng.ps1
            VERBATIM
            COMMENT "Generating Windows icon"
    )

    configure_file(${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/partyboard.manifest.in ${PARTY_BOARD_WINDOWS_MANIFEST} @ONLY)
    configure_file(${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/partyboard.rc.in ${PARTY_BOARD_WINDOWS_RC} @ONLY)

    target_sources(partyboard PRIVATE ${PARTY_BOARD_WINDOWS_ICON_ICO} ${PARTY_BOARD_WINDOWS_RC})
    #    set_target_properties(partyboard PROPERTIES WIN32_EXECUTABLE TRUE)

    if (MSVC)
        target_link_options(partyboard PRIVATE /MANIFEST:NO)
    endif ()
endif ()

if (APPLE)
    if (IOS)
        set(PARTY_BOARD_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/ios)
    elseif (TVOS)
        set(PARTY_BOARD_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/tvos)
    else ()
        set(PARTY_BOARD_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/macos)
    endif ()
    set(PARTY_BOARD_INFO_PLIST ${PARTY_BOARD_RESOURCE_DIR}/Info.plist.in)
    file(GLOB_RECURSE PARTY_BOARD_RESOURCE_FILES
            "${PARTY_BOARD_RESOURCE_DIR}/Assets.car"
            "${PARTY_BOARD_RESOURCE_DIR}/Base.lproj/*"
            "${PARTY_BOARD_RESOURCE_DIR}/PartyBoard.icns")
    file(GLOB_RECURSE PARTY_BOARD_APP_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/res/*")
    target_sources(partyboard PRIVATE ${PARTY_BOARD_RESOURCE_FILES})
    target_sources(partyboard PRIVATE ${PARTY_BOARD_APP_RESOURCE_FILES})
    foreach (FILE ${PARTY_BOARD_RESOURCE_FILES})
        file(RELATIVE_PATH NEW_FILE "${PARTY_BOARD_RESOURCE_DIR}" ${FILE})
        get_filename_component(NEW_FILE_PATH ${NEW_FILE} DIRECTORY)
        set_property(SOURCE ${FILE} PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${NEW_FILE_PATH}")
    endforeach ()
    foreach (FILE ${PARTY_BOARD_APP_RESOURCE_FILES})
        file(RELATIVE_PATH NEW_FILE "${CMAKE_CURRENT_SOURCE_DIR}" ${FILE})
        get_filename_component(NEW_FILE_PATH ${NEW_FILE} DIRECTORY)
        set_property(SOURCE ${FILE} PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${NEW_FILE_PATH}")
    endforeach ()
    set_target_properties(
            partyboard PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME ${PARTY_BOARD_BUNDLE_NAME}
            MACOSX_BUNDLE_GUI_IDENTIFIER ${PARTY_BOARD_BUNDLE_IDENTIFIER}
            MACOSX_BUNDLE_BUNDLE_VERSION ${PARTY_BOARD_VERSION_STRING}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PARTY_BOARD_SHORT_VERSION_STRING}
            MACOSX_BUNDLE_INFO_PLIST ${PARTY_BOARD_INFO_PLIST}
            OUTPUT_NAME PartyBoard
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
    )
endif ()

if (IOS)
    find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
    find_library(UNIFORM_TYPE_IDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    target_sources(partyboard PRIVATE src/port/ios/FileSelectDialog.m)
    set_source_files_properties(src/port/ios/FileSelectDialog.m PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(partyboard PRIVATE ${UIKIT_FRAMEWORK} ${UNIFORM_TYPE_IDENTIFIERS_FRAMEWORK})
endif ()


# Packaging logic
function(get_target_output_name target result_var)
    get_target_property(output_name ${target} OUTPUT_NAME)
    if (NOT output_name)
        set(${result_var} "${target}" PARENT_SCOPE)
    else ()
        set(${result_var} "${output_name}" PARENT_SCOPE)
    endif ()
endfunction()

function(get_target_prefix target result_var)
    set(${result_var} "" PARENT_SCOPE)
    if (APPLE)
        # Have to recreate some bundle logic here, since CMake can't tell us
        get_target_property(is_bundle ${target} MACOSX_BUNDLE)
        if (is_bundle)
            get_target_output_name(${target} output_name)
            if (CMAKE_SYSTEM_NAME STREQUAL Darwin)
                set(${result_var} "${output_name}.app/Contents/MacOS/" PARENT_SCOPE)
            else ()
                set(${result_var} "${output_name}.app/" PARENT_SCOPE)
            endif ()
        endif ()
    endif ()
endfunction()

set(EXTRA_TARGETS "")
if (TARGET crashpad_handler)
    list(APPEND EXTRA_TARGETS crashpad_handler)
endif ()

install(TARGETS ${BINARY_TARGETS} ${EXTRA_TARGETS} DESTINATION ${CMAKE_INSTALL_PREFIX})
aurora_install_runtime_dlls(partyboard ${CMAKE_INSTALL_PREFIX})

if (NOT APPLE)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/res DESTINATION ${CMAKE_INSTALL_PREFIX})
endif ()

# Aurora's seed_pipeline_cache() reads this from beside the executable and merges
# it into the live cache. (src/port/portmain.cpp has an EnsureInitialPipelineCache()
# that copies it too, but into SDL_GetPrefPath("MarioPartyRD", ...) while Aurora
# keeps its cache in SDL_GetPrefPath(nullptr, ...) -- one level shallower -- so that
# copy lands where nothing reads it. Aurora's own path is the one that works.)
# Without a seed every
# player starts from an empty cache, and a pipeline that is still compiling does not
# delay its draw -- it drops it (extern/aurora/lib/gx/pipeline.cpp), so a scene seen
# for the first time is missing geometry for a few seconds.
#
# OPTIONAL because the file is a recording, not a build product: produce one with
# tools/capture_pipeline_cache.ps1 after playing through the scenes worth seeding.
# A tree without one still builds, and the port logs that none was found.
install(FILES ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
        DESTINATION ${CMAKE_INSTALL_PREFIX} OPTIONAL)

# The install rule alone only helps a packaged build. A developer running from
# the build directory gets whatever copy happens to sit there, which is how a
# stale 207-pipeline seed was still being loaded after the recording had been
# widened to 437 -- silently, since the engine reports how many rows merged but
# not which generation of the file they came from. Keep the build directory in
# step instead of relying on remembering to copy it by hand.
if (EXISTS ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db)
    # Depends on the seed itself rather than hanging off partyboard's POST_BUILD:
    # a POST_BUILD only fires when the executable relinks, so recapturing the seed
    # without touching any source would leave the stale copy in place -- which is
    # precisely the failure this is here to prevent.
    set(PIPELINE_SEED_STAGED ${CMAKE_BINARY_DIR}/initial_pipeline_cache.db)
    add_custom_command(OUTPUT ${PIPELINE_SEED_STAGED}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
                ${PIPELINE_SEED_STAGED}
        DEPENDS ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
        COMMENT "Staging the pipeline cache seed next to partyboard")
    add_custom_target(stage_pipeline_seed ALL DEPENDS ${PIPELINE_SEED_STAGED})
    add_dependencies(partyboard stage_pipeline_seed)
endif()

# TODO uncomment
if (CMAKE_BUILD_TYPE STREQUAL Debug) #OR CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo)
    set(DEBUG_FILES_LIST "")
    foreach (target IN LISTS BINARY_TARGETS EXTRA_TARGETS)
        get_target_output_name(${target} output_name)
        if (WIN32)
            install(FILES $<TARGET_PDB_FILE:${target}> DESTINATION ${CMAKE_INSTALL_PREFIX} OPTIONAL)
        elseif (APPLE)
            get_target_prefix(${target} target_prefix)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND rm -fr \"$<TARGET_FILE_NAME:${target}>.dSYM\")")
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND dsymutil \"${target_prefix}$<TARGET_FILE_NAME:${target}>\")")
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND strip -S \"${target_prefix}$<TARGET_FILE_NAME:${target}>\")")
            if (NOT target_prefix STREQUAL "")
                install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND mv \"${target_prefix}$<TARGET_FILE_NAME:${target}>.dSYM\" .)")
            endif ()
        elseif (UNIX)
            get_target_prefix(${target} target_prefix)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND objcopy --only-keep-debug \"${target_prefix}$<TARGET_FILE_NAME:${target}>\" \"${target_prefix}$<TARGET_FILE_NAME:${target}>.dbg\")")
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND objcopy --strip-debug --add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.dbg \"${target_prefix}$<TARGET_FILE_NAME:${target}>\")")
        endif ()
        list(APPEND DEBUG_FILES_LIST "${output_name}")
    endforeach ()
    # This is a terrible hack to only run this on CI
    # until I turn this into a script or something
    if(DEFINED ENV{GITHUB_ENV})
        if (WIN32)
            list(TRANSFORM DEBUG_FILES_LIST APPEND ".pdb")
            list(JOIN DEBUG_FILES_LIST " " DEBUG_FILES)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND 7z a -t7z \"${CMAKE_INSTALL_PREFIX}/debug.7z\" ${DEBUG_FILES})")
        elseif (APPLE)
            list(TRANSFORM DEBUG_FILES_LIST APPEND ".dSYM")
            list(JOIN DEBUG_FILES_LIST " " DEBUG_FILES)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND tar acfv \"${CMAKE_INSTALL_PREFIX}/debug.tar.xz\" ${DEBUG_FILES})")
        elseif (UNIX)
            list(TRANSFORM DEBUG_FILES_LIST APPEND ".dbg")
            list(JOIN DEBUG_FILES_LIST " " DEBUG_FILES)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND tar -I \"xz -9 -T0\" -cvf \"${CMAKE_INSTALL_PREFIX}/debug.tar.xz\" ${DEBUG_FILES})")
        endif ()
    endif ()
endif ()
foreach (target IN LISTS BINARY_TARGETS)
    get_target_prefix(${target} target_prefix)
    foreach (extra_target IN LISTS EXTRA_TARGETS)
        get_target_prefix(${extra_target} extra_prefix)
        if (NOT "${target_prefix}" STREQUAL "${extra_prefix}")
            # Copy extra target to target prefix
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND cp \"${extra_prefix}$<TARGET_FILE_NAME:${extra_target}>\" \"${target_prefix}$<TARGET_FILE_NAME:${extra_target}>\")")
        endif ()
    endforeach ()
endforeach ()
