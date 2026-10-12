# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Turns ABR_DISK_TARGET into a toolchain, before the first project() -- the one
# moment CMake reads a toolchain file, which is why CMakeLists.txt includes
# this ahead of everything else. Anything here that ran after project() would
# be recorded in the cache and then quietly ignored.
#
# Only a standalone configure gets here. A tool that adds the library with
# add_subdirectory() has settled its own toolchain already, and the library is
# built for whatever that targets.
#
#   auto            what the host builds natively, or whatever a toolchain file
#                   named on the command line targets (the default)
#   linux           Linux, natively
#   windows         Windows, natively (MSVC, or MinGW in an MSYS2 shell)
#   windows-cross   Windows, from any other host, with MinGW-w64

set(ABR_DISK_TARGET "auto" CACHE STRING "What to build for: auto, linux, windows or windows-cross")
set_property(CACHE ABR_DISK_TARGET PROPERTY STRINGS auto linux windows windows-cross)

if(ABR_DISK_TARGET STREQUAL "auto")
    # Nothing to decide: the host's own compiler, or the caller's toolchain.

elseif(ABR_DISK_TARGET STREQUAL "linux")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "ABR_DISK_TARGET=linux is a native build and this host is "
            "${CMAKE_HOST_SYSTEM_NAME}; there is no cross toolchain for Linux here.")
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR
            "ABR_DISK_TARGET=linux is the host's own compiler, but CMAKE_TOOLCHAIN_FILE names "
            "${CMAKE_TOOLCHAIN_FILE}. Drop one of the two -- in a build directory configured "
            "for another target before, that means starting a fresh one.")
    endif()

elseif(ABR_DISK_TARGET STREQUAL "windows")
    if(NOT CMAKE_HOST_WIN32)
        message(FATAL_ERROR
            "ABR_DISK_TARGET=windows is a native build and this host is "
            "${CMAKE_HOST_SYSTEM_NAME}; use -DABR_DISK_TARGET=windows-cross here.")
    endif()

elseif(ABR_DISK_TARGET STREQUAL "windows-cross")
    if(CMAKE_HOST_WIN32)
        message(FATAL_ERROR
            "ABR_DISK_TARGET=windows-cross is for building on another host; "
            "on Windows use -DABR_DISK_TARGET=windows.")
    endif()
    # A toolchain file the caller named stands: another MinGW, or a clang
    # targeting Windows, is as good a cross compiler as the one in the tree.
    if(NOT CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/mingw-w64-x86_64.cmake"
            CACHE FILEPATH "Toolchain file" FORCE)
    endif()

else()
    message(FATAL_ERROR
        "ABR_DISK_TARGET is '${ABR_DISK_TARGET}'; it has to be one of "
        "auto, linux, windows or windows-cross.")
endif()
