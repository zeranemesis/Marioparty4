# PartyBoard Nintendo Switch / libnx CMake toolchain.
# This is intentionally small: it is used by the native bring-up target first.
# Full PartyBoard will only move onto this toolchain as desktop-only dependencies
# are replaced or made optional.

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO is not set. Install devkitPro/devkitA64 first.")
endif()

set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro root")
set(DEVKITA64 "${DEVKITPRO}/devkitA64" CACHE PATH "devkitA64 root")
set(LIBNX "${DEVKITPRO}/libnx" CACHE PATH "libnx root")
set(PORTLIBS "${DEVKITPRO}/portlibs/switch" CACHE PATH "Switch portlibs root")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   "${DEVKITA64}/bin/aarch64-none-elf-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITA64}/bin/aarch64-none-elf-g++")
set(CMAKE_ASM_COMPILER "${DEVKITA64}/bin/aarch64-none-elf-gcc")

# Avoid host executable probes during configure.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PARTYBOARD_SWITCH TRUE CACHE BOOL "Build for Nintendo Switch/libnx" FORCE)
set(PARTYBOARD_SWITCH_BOOTSTRAP TRUE CACHE BOOL "Build only the Switch bring-up target" FORCE)

set(_PB_SWITCH_ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft")
set(CMAKE_C_FLAGS_INIT   "${_PB_SWITCH_ARCH} -fPIE -D__SWITCH__")
set(CMAKE_CXX_FLAGS_INIT "${_PB_SWITCH_ARCH} -fPIE -D__SWITCH__")
set(CMAKE_ASM_FLAGS_INIT "${_PB_SWITCH_ARCH} -fPIE -D__SWITCH__")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${LIBNX}/switch.specs ${_PB_SWITCH_ARCH}")

set(CMAKE_FIND_ROOT_PATH "${DEVKITA64}" "${LIBNX}" "${PORTLIBS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
