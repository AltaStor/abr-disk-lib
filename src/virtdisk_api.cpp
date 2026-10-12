// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Runtime binding of the Windows virtual disk API (virtdisk.dll)
//
#include "virtdisk_api.h"

#include <iostream>

#include "win_util.h"

namespace {

constexpr const wchar_t* kLibraryName = L"virtdisk.dll";

VirtDisk::Api g_api;
HMODULE g_module = nullptr;

// GetProcAddress hands back a FARPROC, which is no more the entry point's type
// than a void* is; the detour through void* is what keeps GCC from warning
// about the cast between function types. The destination type comes from the
// caller, so nothing is asserted here beyond what virtdisk.h already declares.
template <typename Fn>
bool bindEntryPoint(Fn& entryPoint, const char* symbol, VirtDisk::Need need)
{
    entryPoint = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(g_module, symbol)));
    if (!entryPoint && need == VirtDisk::Need::Required) {
        std::cerr << "Error: virtdisk.dll does not export '" << symbol << "'\n";
        return false;
    }
    return true;
}

// Binding stops at the first missing required symbol: a half-bound Api would
// leave a null in place of a function and fail as a crash somewhere inside a
// backup rather than as a diagnosis here.
bool bindAll()
{
    bool bound = true;

#define VIRTDISK_BIND_ENTRY_POINT(member, symbol, need) \
    bound = bound && bindEntryPoint(g_api.member, #symbol, VirtDisk::Need::need);

    VIRTDISK_ENTRY_POINTS(VIRTDISK_BIND_ENTRY_POINT)

#undef VIRTDISK_BIND_ENTRY_POINT

    return bound;
}

} // namespace

bool VirtDisk::loadLibrary()
{
    if (g_module) {
        return true;
    }

    // Use the dll from System32 and nowhere else.
    // The default search would try the directory the tool was started from first, and
    // whoever can write a virtdisk.dll there would then run code in a process that holds
    // the backup repository's keys and administrator rights over the host's disks.
    //
    g_module = LoadLibraryExW(kLibraryName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_module) {
        std::cerr << "Error: failed to load 'virtdisk.dll': " << Win::errorText(GetLastError()) << "\n";
        return false;
    }

    if (!bindAll()) {
        FreeLibrary(g_module);
        g_module = nullptr;
        g_api = Api{};
        return false;
    }
    return true;
}

const VirtDisk::Api& VirtDisk::api()
{
    return g_api;
}
