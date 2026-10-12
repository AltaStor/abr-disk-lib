// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Runtime binding of the VMware VDDK
//
#pragma once

#include <string>

#include "vixDiskLib.h"

namespace Vddk
{
    //
    // The entry points this library uses, named once each.
    //
    // Every row below becomes a member of Api, a dlsym lookup, and the message
    // printed if the symbol turns out to be missing. Keeping the three in one
    // list is what stops them drifting apart as the library grows.
    //
#define VDDK_ENTRY_POINTS(X)                                    \
    X(init,                  VixDiskLib_Init)                   \
    X(exit,                  VixDiskLib_Exit)                   \
    X(allocateConnectParams, VixDiskLib_AllocateConnectParams)  \
    X(freeConnectParams,     VixDiskLib_FreeConnectParams)      \
    X(connectEx,             VixDiskLib_ConnectEx)              \
    X(disconnect,            VixDiskLib_Disconnect)             \
    X(prepareForAccess,      VixDiskLib_PrepareForAccess)       \
    X(endAccess,             VixDiskLib_EndAccess)              \
    X(openEx,                VixDiskLib_OpenEx)                 \
    X(close,                 VixDiskLib_Close)                  \
    X(getInfo,               VixDiskLib_GetInfo)                \
    X(freeInfo,              VixDiskLib_FreeInfo)               \
    X(getTransportMode,      VixDiskLib_GetTransportMode)       \
    X(read,                  VixDiskLib_Read)                   \
    X(write,                 VixDiskLib_Write)                  \
    X(queryAllocatedBlocks,  VixDiskLib_QueryAllocatedBlocks)   \
    X(freeBlockList,         VixDiskLib_FreeBlockList)          \
    X(queryChangedAreas,     VixDiskLib_QueryChangedAreas)      \
    X(freeChangedAreas,      VixDiskLib_FreeChangedAreas)       \
    X(getErrorText,          VixDiskLib_GetErrorText)           \
    X(freeErrorText,         VixDiskLib_FreeErrorText)

    //
    // Each member takes its type from the matching declaration in vixDiskLib.h,
    // so the vendor header remains the only description of any signature.
    // Spelling the prototypes out again here would let a VDDK whose arguments
    // differ still compile, and then corrupt the call at run time.
    //
    struct Api
    {
#define VDDK_DECLARE_ENTRY_POINT(member, symbol) decltype(&::symbol) member = nullptr;
        VDDK_ENTRY_POINTS(VDDK_DECLARE_ENTRY_POINT)
#undef VDDK_DECLARE_ENTRY_POINT
    };

    // Loads the VDDK -- libvixDiskLib.so, or vixDiskLib.dll on Windows -- and
    // binds every entry point above.
    //
    // With an empty directory the library is found through the loader's own
    // search path, and which installation that is is the launcher's call, made
    // by exporting LD_LIBRARY_PATH (PATH on Windows) before the process starts.
    // A directory names the installation outright.
    //
    // Calling it again once loaded is a no-op, so callers that cannot easily
    // tell whether they are first do not have to.
    bool loadLibrary(const std::string& directory = {});

    // Root of the installation the library was actually loaded from, for
    // VixDiskLib_Init to find the transport plugins under. Empty until
    // loadLibrary succeeds, and also if the loader declines to say.
    const std::string& installRoot();

    // The bound entry points. Every member is null until loadLibrary succeeds,
    // so nothing here may be called before then. The reference itself is stable
    // for the life of the process and may be taken early.
    const Api& api();
}
