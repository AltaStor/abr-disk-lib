// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Runtime binding of the Windows virtual disk API (virtdisk.dll)
//
#pragma once

#include <windows.h>

#include <virtdisk.h>

namespace VirtDisk
{
    // Whether the library can do without an entry point. A required one missing
    // means the library is unusable and binding fails; an optional one is left
    // null, for the one feature that needs it to report as unavailable.
    enum class Need { Required, Optional };

    //
    // The entry points this library uses, named once each.
    //
    // Every row below becomes a member of Api, a GetProcAddress lookup, and the
    // message printed if the symbol turns out to be missing. Keeping the three
    // in one list is what stops them drifting apart as the library grows.
    //
    // QueryChangesVirtualDisk -- resilient change tracking -- arrived with
    // Windows 10 1607 and Server 2016. Earlier builds of Windows 10 still do
    // full backups and restores, so it is the one entry point that may be absent.
    //
#define VIRTDISK_ENTRY_POINTS(X)                                            \
    X(openVirtualDisk,            OpenVirtualDisk,            Required)     \
    X(createVirtualDisk,          CreateVirtualDisk,          Required)     \
    X(attachVirtualDisk,          AttachVirtualDisk,          Required)     \
    X(detachVirtualDisk,          DetachVirtualDisk,          Required)     \
    X(getVirtualDiskPhysicalPath, GetVirtualDiskPhysicalPath, Required)     \
    X(getVirtualDiskInformation,  GetVirtualDiskInformation,  Required)     \
    X(queryChangesVirtualDisk,    QueryChangesVirtualDisk,    Optional)

    //
    // Each member takes its type from the matching declaration in virtdisk.h,
    // so the SDK header remains the only description of any signature.
    // Spelling the prototypes out again here would let a mismatch compile, and
    // then corrupt the call at run time.
    //
    struct Api
    {
#define VIRTDISK_DECLARE_ENTRY_POINT(member, symbol, need) decltype(&::symbol) member = nullptr;
        VIRTDISK_ENTRY_POINTS(VIRTDISK_DECLARE_ENTRY_POINT)
#undef VIRTDISK_DECLARE_ENTRY_POINT
    };

    // Loads virtdisk.dll from the system directory and binds every entry point
    // above. Bound at run time rather than linked, so that the commands that
    // never touch a virtual disk start on any Windows, and so that one missing
    // entry point is a diagnosis instead of a process that will not start.
    //
    // Calling it again once loaded is a no-op, so callers that cannot easily
    // tell whether they are first do not have to.
    bool loadLibrary();

    // The bound entry points. Every member is null until loadLibrary succeeds,
    // and an optional one may stay null after it, so nothing here may be called
    // before then. The reference itself is stable for the life of the process
    // and may be taken early.
    const Api& api();
}
