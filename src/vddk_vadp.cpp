// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Changed areas of a VDDK disk through the vSphere API
//
#include "vddk_vadp.h"

#include <algorithm>
#include <iostream>
#include <string_view>

#include <vsphere-soap/vim/CbtQuery.hpp>
#include <vsphere-soap/vim/HttpTransportAdapter.hpp>
#include <vsphere-soap/vim/SoapClient.hpp>
#include <vsphere-soap/vim/VimInventory.hpp>
#include <vsphere-soap/vim/VimSession.hpp>

#include "soap_detail.h"

namespace {

using namespace VSphereSoap::Vim;

constexpr uint16_t kDefaultPort = 443;

bool failed(const std::string& what, const VSphereSoap::Error& error)
{
    std::cerr << "Error: " << what << ": " << error.message() << "\n";
    return false;
}

// "moref=vm-12", the VDDK's spelling of a VM spec, or a bare "vm-12", to "vm-12".
// Empty for a spec of any other form, which names nothing the vSphere API can be asked about.
std::string vmMoref(const std::string& vmxSpec)
{
    constexpr std::string_view kPrefix = "moref=";
    const std::string moref = vmxSpec.starts_with(kPrefix) ? vmxSpec.substr(kPrefix.size()) : vmxSpec;
    return moref.find_first_of("=/[]?&") == std::string::npos ? moref : std::string();
}

// Whether two "[datastore] dir/name.vmdk" paths name one file.
// Datastore and path are compared apart, so that the spacing after the bracket does not matter.
bool sameFile(const std::string& a, const std::string& b)
{
    const auto parsedA = parseDatastorePath(a);
    const auto parsedB = parseDatastorePath(b);
    return parsedA && parsedB ? *parsedA == *parsedB : a == b;
}

// The disk that has the file `diskPath` anywhere in its backing chain; null when none has.
// The VDDK is given whichever file of the chain the caller had at hand, the snapshot's own or the current delta,
// and the device it belongs to is the same for all of them.
const VimDiskInfo* findDisk(const std::vector<VimDiskInfo>& disks, const std::string& diskPath)
{
    for (const auto& disk : disks) {
        if (sameFile(disk.rawFileName, diskPath)) {
            return &disk;
        }
        for (const auto& parent : disk.parents) {
            if (sameFile(parent.fileName, diskPath)) {
                return &disk;
            }
        }
    }
    return nullptr;
}

// Adds one changed area, widened to whole sectors and cut at the end of the disk.
// The areas come sorted and merged, but widening can make two of them meet, and those are joined.
void appendArea(std::vector<ByteRange>& ranges, const Extent& area, uint32_t sectorSize, uint64_t capacityBytes)
{
    const uint64_t begin = area.start - area.start % sectorSize;
    uint64_t end = std::min(area.start + area.length, capacityBytes);
    end = std::min(end + (sectorSize - end % sectorSize) % sectorSize, capacityBytes);
    if (end <= begin) {
        return;
    }

    if (!ranges.empty()) {
        ByteRange& last = ranges.back();
        if (begin >= last.offset && begin <= last.offset + last.length) {
            last.length = std::max(last.offset + last.length, end) - last.offset;
            return;
        }
    }
    ranges.push_back({begin, end - begin});
}

} // namespace

bool Vddk::Vadp::queryChangedAreas(const ConnectParams& params, const std::string& diskPath,
                                   const std::string& changeId, uint32_t sectorSize, uint64_t capacityBytes,
                                   std::vector<ByteRange>& ranges)
{
    if (params.host.empty()) {
        std::cerr << "Error: a local VMDK has no changed areas to query: they come from vSphere\n";
        return false;
    }
    const std::string vm = vmMoref(params.vmxSpec);
    if (vm.empty()) {
        std::cerr << "Error: the changed areas are queried through the vSphere API, which needs the VM as "
                     "moref=<id>, not '" << params.vmxSpec << "'\n";
        return false;
    }

    std::cerr << "Changed areas of disk '" << diskPath << "' are queried through the vSphere API\n";
    Soap::Detail::routeLibraryLog();

    // The thumbprint the VDDK checks the host against is the same SHA-1 one, and replaces the chain check here too.
    HttpTransportAdapter transport(params.host, params.port == 0 ? kDefaultPort : static_cast<uint16_t>(params.port),
                                   true, params.thumbPrint);
    SoapClient soap(transport, params.host);
    auto session = VimSession::open(soap, params.user, params.password);
    if (!session) {
        return failed("failed to log in to '" + params.host + "' through the vSphere API", session.error());
    }
    VimInventory inventory(*session.value());

    const MoRef vmRef{"VirtualMachine", vm};
    auto vmInfo = inventory.describeVm(vmRef);
    if (!vmInfo) {
        return failed("failed to read the configuration of VM '" + vm + "'", vmInfo.error());
    }

    // The snapshot's disks first, since that is where a backup at a snapshot names its disk, and then the VM's own,
    // whose chain also holds a file the snapshot has since been followed by.
    MoRef snapshot;
    std::vector<VimDiskInfo> snapshotDisks;
    if (!params.snapshotRef.empty()) {
        snapshot = MoRef{"VirtualMachineSnapshot", params.snapshotRef};
        auto disks = inventory.describeSnapshotDisks(snapshot);
        if (!disks) {
            return failed("failed to read the disks of snapshot '" + params.snapshotRef + "'", disks.error());
        }
        snapshotDisks = std::move(disks).value();
    }
    const VimDiskInfo* disk = findDisk(snapshotDisks, diskPath);
    if (!disk) {
        disk = findDisk(vmInfo.value().disks, diskPath);
    }
    if (!disk) {
        std::cerr << "Error: VM '" << vm << "' has no disk with the file '" << diskPath << "'\n";
        return false;
    }

    // Change tracking switched off, an RDM or an independent disk would otherwise fail as an opaque fault.
    CbtQuery cbt(*session.value());
    if (const VSphereSoap::Error eligible = cbt.checkEligible(vmInfo.value(), *disk); !eligible.ok()) {
        return failed("no changed areas of '" + diskPath + "'", eligible);
    }

    // Without a snapshot vSphere answers for the VM as it is now, which it does only for a powered-off VM.
    auto areas = changeId == "*" ? cbt.queryAllocatedExtents(vmRef, snapshot, disk->key, capacityBytes)
                                 : cbt.queryChangedExtents(vmRef, snapshot, disk->key, changeId, capacityBytes);
    if (!areas) {
        return failed("failed to query the changed areas of '" + diskPath + "' through the vSphere API",
                      areas.error());
    }
    for (const Extent& area : mergeExtents(std::move(areas).value())) {
        appendArea(ranges, area, sectorSize, capacityBytes);
    }
    return true;
}
