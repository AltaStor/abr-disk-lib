// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Changed areas of a VDDK disk through the vSphere API
//
// The VDDK reads a disk but is not asked what changed on it: that is vSphere's VirtualMachine.QueryChangedDiskAreas,
// the query VADP backups are built around, and the answer is then read through the VDDK. This is that query, made
// with vsphere-soap-lib's CbtQuery and the credentials the VDDK session was connected with.
//
// Compiled only when the library has both the VDDK and the SOAP backend.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vddk_disk.h"

namespace Vddk::Vadp
{
    // The areas of the disk `diskPath` changed since `changeId`, or all of its allocated areas for "*".
    //
    // The disk is a disk of the VM `params.vmxSpec` names, as `params.snapshotRef` holds it when one is given and
    // as the VM is now otherwise, which vSphere answers only for a powered-off VM. `diskPath` may name any file of
    // the disk's backing chain, the way the VDDK is given it. The query takes a login of its own on `params.host`,
    // with the same user, password, port and thumbprint, and ends it before returning.
    //
    // vSphere counts in bytes, and the ranges are bytes too, widened to whole `sectorSize` sectors and cut at
    // `capacityBytes` so that every one of them can be read through the VDDK as it stands.
    //
    // A disk without changed block tracking, such as one of a VM with CBT off or an RDM, fails with the reason.
    // A failure is reported on std::cerr behind "Error: ".
    bool queryChangedAreas(const ConnectParams& params, const std::string& diskPath, const std::string& changeId,
                           uint32_t sectorSize, uint64_t capacityBytes, std::vector<ByteRange>& ranges);
}
