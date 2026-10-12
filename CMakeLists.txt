# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# abr-disk-lib: disk sources and targets behind IDiskReader/IDiskWriter.
#
# Configured two ways:
#
#   on its own       cmake -S . -B build [-DABR_DISK_TARGET=...]
#                    The library, its self-test, and a toolchain chosen here.
#
#   from a tool      add_subdirectory("${ABR_DISK_LIB_DIR}" abr_disk_lib_build)
#                    The library only, built with whatever the tool has
#                    settled: its toolchain, build type, MSVC runtime and the
#                    directory-level link options every target below inherits.
#
# What is built is decided by parameters, never by guessing past them:
#
#   ABR_DISK_TARGET      auto | linux | windows | windows-cross  (standalone only)
#   ABR_DISK_WITH_VHDX   AUTO | ON | OFF   Hyper-V VHD/VHDX; Windows targets only
#   ABR_DISK_WITH_VDDK   AUTO | ON | OFF   VMware VMDK through the VDDK
#   VDDK_DIR             an extracted VDDK, for its headers
#   ABR_DISK_BUILD_TESTS ON | OFF          the self-test (default: ON standalone)
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ABR_DISK_STANDALONE TRUE)

    # Has to come before project(), which is where the toolchain is read.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/select-target.cmake")

    project(abr-disk-lib VERSION 1.0.0 LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()

    # The toolchain has been read by now, so what was asked for can be held
    # against what it actually targets: a toolchain file named beside
    # ABR_DISK_TARGET wins the choice of compiler, and must not win it silently.
    if(ABR_DISK_TARGET STREQUAL "linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ABR_DISK_TARGET=linux, but the toolchain targets ${CMAKE_SYSTEM_NAME}.")
    endif()
    if(ABR_DISK_TARGET MATCHES "^windows" AND NOT WIN32)
        message(FATAL_ERROR "ABR_DISK_TARGET=${ABR_DISK_TARGET}, but the toolchain targets ${CMAKE_SYSTEM_NAME}.")
    endif()
    if(ABR_DISK_TARGET STREQUAL "windows-cross" AND NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "ABR_DISK_TARGET=windows-cross, but this is not a cross build.")
    endif()

    # libstdc++, libgcc and winpthread linked into the self-test, so that it
    # runs on a Windows host with nothing beside it. A tool sets the same for
    # its own executables, at its own directory level.
    if(MINGW)
        add_link_options(-static)
    endif()
else()
    set(ABR_DISK_STANDALONE FALSE)
endif()

if(NOT WIN32 AND NOT UNIX)
    message(FATAL_ERROR "abr-disk-lib has a Windows half and a POSIX half; ${CMAKE_SYSTEM_NAME} is neither.")
endif()

# --- Which backends -----------------------------------------------------------
#
# Three states rather than an option(): an option's default is fixed by the
# first configure, so a VDDK pointed at afterwards would stay switched off by a
# default nobody chose. AUTO is re-evaluated every time.

set(ABR_DISK_WITH_VHDX "AUTO" CACHE STRING "Hyper-V VHD/VHDX backend: AUTO, ON or OFF")
set(ABR_DISK_WITH_VDDK "AUTO" CACHE STRING "VMware VDDK backend: AUTO, ON or OFF")
set_property(CACHE ABR_DISK_WITH_VHDX PROPERTY STRINGS AUTO ON OFF)
set_property(CACHE ABR_DISK_WITH_VDDK PROPERTY STRINGS AUTO ON OFF)

set(VDDK_DIR "" CACHE PATH "Path to the VMware VDDK root (its include/ is what the build needs)")

foreach(_abr_disk_backend IN ITEMS VHDX VDDK)
    if(NOT ABR_DISK_WITH_${_abr_disk_backend} MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR
            "ABR_DISK_WITH_${_abr_disk_backend} is '${ABR_DISK_WITH_${_abr_disk_backend}}'; "
            "it has to be AUTO, ON or OFF.")
    endif()
endforeach()

# Hyper-V: the Windows virtual disk service, so wherever the target is Windows
# and nowhere else. There is no VHDX reader of the library's own to fall back
# on -- see README.md for why.
if(ABR_DISK_WITH_VHDX STREQUAL "AUTO")
    set(_abr_disk_vhdx ${WIN32})
else()
    set(_abr_disk_vhdx ${ABR_DISK_WITH_VHDX})
endif()
if(_abr_disk_vhdx AND NOT WIN32)
    message(FATAL_ERROR
        "ABR_DISK_WITH_VHDX=ON needs a Windows target: a VHD or VHDX is reached through "
        "the Windows virtual disk service. Build with -DABR_DISK_TARGET=windows-cross.")
endif()

# VMware: only the VDDK's headers are needed to build -- the library itself is
# bound at run time -- so AUTO means "where the headers are". On a Windows
# target that takes asking for, since nothing there has run against a VDDK yet.
set(_abr_disk_vddk_header "${VDDK_DIR}/include/vixDiskLib.h")
if(ABR_DISK_WITH_VDDK STREQUAL "AUTO")
    if(VDDK_DIR AND EXISTS "${_abr_disk_vddk_header}" AND NOT WIN32)
        set(_abr_disk_vddk TRUE)
    else()
        set(_abr_disk_vddk FALSE)
    endif()
else()
    set(_abr_disk_vddk ${ABR_DISK_WITH_VDDK})
endif()
if(_abr_disk_vddk AND NOT EXISTS "${_abr_disk_vddk_header}")
    message(FATAL_ERROR
        "ABR_DISK_WITH_VDDK=ON, but the VDDK headers are not at ${_abr_disk_vddk_header}.\n"
        "Point CMake at an extracted VDDK with -DVDDK_DIR=/path/to/vddk")
endif()

# --- The library --------------------------------------------------------------

add_library(abr-disk-lib STATIC
    src/file_disk.cpp
)

# One half of each host-specific piece, never both: separate translation units
# rather than one file behind #ifdef, so that neither host's headers have to be
# reachable from the other's build.
if(WIN32)
    target_sources(abr-disk-lib PRIVATE
        src/file_disk_windows.cpp
        src/win_disk.cpp
        src/win_util.cpp
    )
else()
    target_sources(abr-disk-lib PRIVATE src/file_disk_posix.cpp)
endif()

if(_abr_disk_vhdx)
    target_sources(abr-disk-lib PRIVATE src/vhdx_disk.cpp src/virtdisk_api.cpp)

    # PUBLIC, so that a tool can compile its --vhdx half in or out on the same
    # answer the library was built with.
    target_compile_definitions(abr-disk-lib PUBLIC ABR_DISK_HAVE_VHDX)

    # mpr is WNetAddConnection2, for a disk on a share reached under
    # credentials of the caller's choosing.
    target_link_libraries(abr-disk-lib PUBLIC mpr)
endif()

if(_abr_disk_vddk)
    target_sources(abr-disk-lib PRIVATE src/vddk_disk.cpp src/vddk_api.cpp)
    if(WIN32)
        target_sources(abr-disk-lib PRIVATE src/vddk_loader_windows.cpp)
    else()
        target_sources(abr-disk-lib PRIVATE src/vddk_loader_posix.cpp)
    endif()

    target_compile_definitions(abr-disk-lib PUBLIC ABR_DISK_HAVE_VDDK)

    # PRIVATE: vddk_disk.h names nothing of the VDDK's, so nothing that
    # includes it needs the vendor headers. SYSTEM keeps their warnings out.
    target_include_directories(abr-disk-lib SYSTEM PRIVATE "${VDDK_DIR}/include")

    # dlopen, where that is a library of its own.
    target_link_libraries(abr-disk-lib PUBLIC ${CMAKE_DL_LIBS})
endif()

target_include_directories(abr-disk-lib PUBLIC "${CMAKE_CURRENT_LIST_DIR}/inc")
target_include_directories(abr-disk-lib PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")

target_compile_features(abr-disk-lib PUBLIC cxx_std_20)

if(WIN32)
    # Windows 10 is the oldest the library runs on, and the headers only
    # declare what arrived by the version named here: change tracking
    # (QueryChangesVirtualDisk) needs NTDDI_WIN10_RS1. A binary still starts on
    # older builds of Windows 10, since virtdisk.dll is bound at run time and a
    # missing entry point is found there rather than by the loader.
    #
    # PRIVATE: win_util.h and win_disk.h include <windows.h>, but leave what it
    # is configured with to whoever includes them.
    target_compile_definitions(abr-disk-lib
        PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            NTDDI_VERSION=0x0A000002
    )

    # advapi32 for the token and privilege calls, shell32 for
    # CommandLineToArgvW; both in win_util.cpp.
    target_link_libraries(abr-disk-lib PUBLIC advapi32 shell32)
endif()

if(MSVC)
    # The sources are UTF-8 (the copyright lines, at least), which MSVC reads
    # as the ANSI code page unless told otherwise.
    target_compile_options(abr-disk-lib PRIVATE /utf-8 /W3)
else()
    target_compile_options(abr-disk-lib PRIVATE -Wall -Wextra "$<$<CONFIG:Debug>:-g;-O0>")
endif()

# Said once per configure, since which backends a build carries is the first
# thing anyone debugging "--vhdx is not supported" needs to know.
set(_abr_disk_backends "file/device")
if(_abr_disk_vhdx)
    string(APPEND _abr_disk_backends ", Hyper-V VHD/VHDX")
endif()
if(_abr_disk_vddk)
    string(APPEND _abr_disk_backends ", VMware VDDK (${VDDK_DIR})")
endif()
message(STATUS "abr-disk-lib: ${CMAKE_SYSTEM_NAME} target, backends: ${_abr_disk_backends}")

# --- The self-test ------------------------------------------------------------

option(ABR_DISK_BUILD_TESTS "Build abr-disk-lib's self-test" ${ABR_DISK_STANDALONE})

if(ABR_DISK_BUILD_TESTS)
    add_executable(abr-disk-selftest test/selftest.cpp)
    target_link_libraries(abr-disk-selftest PRIVATE abr-disk-lib)

    if(MSVC)
        target_compile_options(abr-disk-selftest PRIVATE /utf-8 /W3)
    endif()

    # A cross-built test is a Windows program, which this host runs only
    # through an emulator such as wine; without one it is still built, to be
    # copied to a Windows host and run there.
    if(ABR_DISK_STANDALONE)
        enable_testing()
    endif()
    if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
        add_test(NAME abr-disk-selftest
            COMMAND abr-disk-selftest "${CMAKE_CURRENT_BINARY_DIR}/selftest-scratch")
    endif()
endif()
