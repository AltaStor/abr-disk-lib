// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Loading the VDDK on a POSIX host
//
#include "vddk_loader.h"

#include <dlfcn.h>
#include <climits>

namespace {

constexpr const char* kLibraryName = "libvixDiskLib.so";

void* g_handle = nullptr;

} // namespace

const char* Vddk::Loader::libraryName()
{
    return kLibraryName;
}

bool Vddk::Loader::open(const std::string& directory, std::string& reason)
{
    // The bare name unless told otherwise: which installation it resolves to
    // is then settled by LD_LIBRARY_PATH, the only search path that also
    // reaches the plugins the VDDK goes on to dlopen for itself. A directory
    // finds the library but not those, so a caller that names one still has to
    // have the bundle on the search path for anything but a local disk.
    const std::string name = directory.empty() ? std::string(kLibraryName)
                                               : directory + "/" + kLibraryName;

    // RTLD_GLOBAL because those plugins resolve against this library rather
    // than loading a second copy.
    g_handle = dlopen(name.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        const char* error = dlerror();
        reason = error ? error : "unknown error";
        return false;
    }
    return true;
}

// dlsym hands back a void*, which is what the caller wants: the function type
// it is cast to comes from vixDiskLib.h, over in vddk_api.cpp.
void* Vddk::Loader::symbol(const char* name)
{
    return dlsym(g_handle, name);
}

// Worked out from the library that was just loaded rather than told to us: the
// loader has already resolved exactly which installation the search path
// pointed it at. Asking it beats asking the caller.
std::string Vddk::Loader::origin()
{
#if defined(RTLD_DI_ORIGIN)
    char origin[PATH_MAX] = {};

    if (dlinfo(g_handle, RTLD_DI_ORIGIN, origin) == 0) {
        return origin;
    }
#endif
    return {};
}

const char* Vddk::Loader::searchHint()
{
    return "No VDDK installation is on the library search path (LD_LIBRARY_PATH); "
           "a launcher such as abr-vmdk-tool.sh is what puts one there.\n";
}

void Vddk::Loader::close()
{
    if (g_handle) {
        dlclose(g_handle);
        g_handle = nullptr;
    }
}
