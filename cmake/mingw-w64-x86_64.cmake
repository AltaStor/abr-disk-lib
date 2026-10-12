# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Cross compiling abr-disk-lib for 64-bit Windows with MinGW-w64.
#
# -DABR_DISK_TARGET=windows-cross selects this file by itself; it can also be
# named outright:
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#
# A tool that adds this library as a subdirectory brings a toolchain of its
# own, and this file is then not read at all.
#
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(ABR_MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "Target triple of the MinGW-w64 toolchain")

# The unsuffixed names. Debian and Ubuntu make them alternatives for a -win32
# and a -posix flavour; the posix one is what both tools are built with, and a
# library and the program it is linked into have to come from one compiler.
set(CMAKE_C_COMPILER "${ABR_MINGW_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${ABR_MINGW_TRIPLE}-g++")
set(CMAKE_RC_COMPILER "${ABR_MINGW_TRIPLE}-windres")

# Looked up here so that a MinGW that is not installed stops the configure with
# a sentence, rather than with CMake's account of a compiler it could not run.
# Searched afresh every time, since a result left in the cache would outlive
# the PATH it was found on.
unset(ABR_MINGW_CXX CACHE)
find_program(ABR_MINGW_CXX "${ABR_MINGW_TRIPLE}-g++")
if(NOT ABR_MINGW_CXX)
    message(FATAL_ERROR
        "${ABR_MINGW_TRIPLE}-g++ is not on PATH, and a Windows cross build needs it. "
        "On Debian and Ubuntu the package is g++-mingw-w64-x86-64-posix.")
endif()

# Libraries and headers come from the target's side only: nothing of the build
# host's may end up in a Windows binary.
#
# The sysroot sits beside the compiler's bin directory -- /usr/<triple> for the
# distribution's packages, somewhere else for a toolchain unpacked by hand --
# so it is found from the compiler, through the alternatives links, rather
# than assumed.
#
# Appended to, never assigned: CMake reads a toolchain file more than once, and
# whatever else has put a tree in the list by then has to stay in it.
get_filename_component(_abr_mingw_bin "${ABR_MINGW_CXX}" REALPATH)
get_filename_component(_abr_mingw_bin "${_abr_mingw_bin}" DIRECTORY)
get_filename_component(_abr_mingw_sysroot "${_abr_mingw_bin}/../${ABR_MINGW_TRIPLE}" ABSOLUTE)
if(NOT _abr_mingw_sysroot IN_LIST CMAKE_FIND_ROOT_PATH)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_abr_mingw_sysroot}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
