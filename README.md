# abr-disk-lib

Disk sources and targets for the AltaStor backup tools, behind two small
interfaces: `IDiskReader` for the read side of a backup and `IDiskWriter` for
the write side of a restore. 
Offsets and lengths are bytes, so a copy loop is the same whatever it is copying.

It is the bottom layer of `abr-vhdx-tool`, `abr-vmdk-tool` and other utils.

## What it reads and writes

| Disk | Classes | Header | Local | Remote | Targets |
|---|---|---|---|---|---|
| Plain file, disk or volume device | `FileDiskReader`, `FileDiskWriter` | `disk_device.h` | a path or a device (`/dev/sdb`, `\\.\PhysicalDrive2`, `\\.\E:`) | a path on an NFS mount or an SMB share | all |
| Hyper-V VHD, VHDX, AVHDX | `Vhdx::DiskReader`, `Vhdx::DiskWriter` | `vhdx_disk.h` | a path on the host | a UNC path on an SMB 3 share, with `Vhdx::ShareCredentials` or the process's own logon | Windows |
| VMware VMDK | `Vddk::DiskReader`, `Vddk::DiskWriter` (on a `Vddk::Session`) | `vddk_disk.h` | a `.vmdk` file | a VM's disk on an ESXi host or a vCenter (`Vddk::ConnectParams::host`) | Linux; Windows on request |

Every class is default constructed and then `open()`ed; a failure is reported
on `std::cerr` behind `Error: ` and `open()` returns false. `openFileReader`,
`openFileWriter`, `Vhdx::openReader`, `Vhdx::openWriter` and
`Vddk::Session::openReader`/`openWriter` are the same classes behind a
`std::unique_ptr` to the interface, for a caller that only wants the interface.

```cpp
#include "vhdx_disk.h"

Vhdx::DiskReader disk;
if (!disk.open(R"(\\hv01\vms\web\web.vhdx)", /* changeId */ {}, {"CORP\\backup", password})) {
    return 1;
}

std::vector<ByteRange> ranges;
disk.dataRanges(ranges);              // the whole disk, or what changed since changeId
disk.read(ranges[0].offset, 4096, buffer);
```

A Hyper-V disk is reached through the Windows virtual disk service — opened,
attached, and read as the disk device it surfaces as — so it needs Windows and
an elevated process. There is no VHDX parser here, and so no Hyper-V backend
on Linux: reading the format behind the service's back would also mean walking
differencing chains and replaying logs the way Hyper-V does, and getting that
subtly wrong in a backup is worse than not offering it.

The public headers name nothing of Windows' or of the VDDK's (`win_util.h` and
`win_disk.h` aside, which are the Windows helpers themselves), so
`vhdx_disk.h` and `vddk_disk.h` can be included on any target. Whether the
library behind them carries the backend is `ABR_DISK_HAVE_VHDX` /
`ABR_DISK_HAVE_VDDK` at compile time — defined for whoever links the library —
and `Disk::hasVhdx()` / `Disk::hasVddk()` at run time.

## Building

Three targets, chosen with one parameter:

```bash
cmake -S . -B build -DABR_DISK_TARGET=linux            # Linux, natively
cmake -S . -B build -DABR_DISK_TARGET=windows          # Windows, natively (MSVC, or MinGW under MSYS2)
cmake -S . -B build -DABR_DISK_TARGET=windows-cross    # Windows, from Linux, with MinGW-w64
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

or the presets of the same names, each with a build directory of its own:

```bash
cmake --preset windows-cross && cmake --build --preset windows-cross
```

| Parameter | Values | Default | |
|---|---|---|---|
| `ABR_DISK_TARGET` | `auto`, `linux`, `windows`, `windows-cross` | `auto` | `auto` is the host's own compiler, or whatever a `CMAKE_TOOLCHAIN_FILE` on the command line targets. A target the host cannot build is refused, not approximated. |
| `ABR_DISK_WITH_VHDX` | `AUTO`, `ON`, `OFF` | `AUTO` | The Hyper-V backend. `AUTO` is on for a Windows target. `ON` for any other target is an error. |
| `ABR_DISK_WITH_VDDK` | `AUTO`, `ON`, `OFF` | `AUTO` | The VMware backend. `AUTO` is on for a Linux target when `VDDK_DIR` has the headers; on Windows it has to be asked for. |
| `VDDK_DIR` | a path | empty | An extracted VDDK. Only `include/vixDiskLib.h` is needed to build: the library is bound at run time. |
| `ABR_DISK_BUILD_TESTS` | `ON`, `OFF` | `ON` standalone, `OFF` in a tool | The self-test. |

`ABR_DISK_TARGET` changes the toolchain, which CMake reads once per build
directory: use a fresh directory per target, as the presets do.

`windows-cross` needs `x86_64-w64-mingw32-g++` on `PATH` (the `-posix` flavour
on Debian and Ubuntu, `g++-mingw-w64-x86-64-posix`). A cross-built self-test
is a Windows program; it is built, and run by `ctest` only when
`CMAKE_CROSSCOMPILING_EMULATOR` names something like `wine`.

## Using it from a tool

In the tool's `CMakeLists.txt`, after the directory-level settings its targets inherit:

```cmake
include("${ABR_TOOLS_LIB_DIR}/cmake/backuplib.cmake")
include("${ABR_TOOLS_LIB_DIR}/cmake/disklib.cmake")       # declares ABR_DISK_LIB_DIR

set(ABR_DISK_WITH_VHDX ON CACHE STRING "" FORCE)          # the backend this tool is for

add_subdirectory("${ABR_BACKUPLIB_DIR}" backuplib_build)
add_subdirectory("${ABR_DISK_LIB_DIR}" abr_disk_lib_build)
add_subdirectory("${ABR_TOOLS_LIB_DIR}" abr_tools_lib_build)
```

Added that way the library has no `project()` of its own and decides nothing
about the toolchain: it is built for whatever the tool targets.
`ABR_DISK_TARGET` is not read.

`ABR_DISK_LIB_DIR` is `<tool repo>/abr-disk-lib`, or failing that a checkout
beside the tool's repository, or whatever `-DABR_DISK_LIB_DIR=` names.

Anything else needs only the target:

```cmake
add_subdirectory(/path/to/abr-disk-lib abr_disk_lib_build)
target_link_libraries(my-program PRIVATE abr-disk-lib)
```

## Layout

```
inc/  disk_device.h      IDiskReader / IDiskWriter, the file classes, Disk::
      vhdx_disk.h        Vhdx:: -- Hyper-V
      vddk_disk.h        Vddk:: -- VMware
      win_util.h         Win:: helpers (Windows only)
      win_disk.h         Win::RawDisk, raw disk and volume access (Windows only)
src/  file_disk.cpp           the exported file classes
      file_disk_posix.cpp     ... over a stream, and the Linux sector size query
      file_disk_windows.cpp   ... over a handle, files and devices
      win_util.cpp, win_disk.cpp
      vhdx_disk.cpp, virtdisk_api.{h,cpp}        virtdisk.dll, bound at run time
      vddk_disk.cpp, vddk_api.{h,cpp}            the VDDK, bound at run time
      vddk_loader.h, vddk_loader_{posix,windows}.cpp
      disk_detail.h           what the exported classes share
cmake/select-target.cmake     ABR_DISK_TARGET -> a toolchain
      mingw-w64-x86_64.cmake  the cross toolchain
test/selftest.cpp
```

## What has been tested

- **Linux, native:** the self-test; `abr-vmdk-tool` built on the library, with
  a file backup and restore and a VDDK backup and restore of a local VMDK.
- **Windows, cross:** everything compiles and links, including
  `abr-vhdx-tool.exe`.
- **Windows, native (MSVC):** not built yet.
