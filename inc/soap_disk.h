// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware disk source/target over the vSphere Web Services API
//
// The other way to a VMware disk beside vddk_disk.h: SOAP and HTTPS through vsphere-soap-lib, with no VMware
// library on the host. The header names nothing of that library's, so it can be included anywhere; whether the
// library behind it has the backend is ABR_DISK_HAVE_SOAP at compile time and Disk::hasSoap() at run time.
//
#pragma once

#include "disk_device.h"

namespace Soap
{
    namespace Detail
    {
        class WriterImpl;
    }

    struct ConnectParams {
        // An ESXi host or a vCenter; a SOAP disk is always remote.
        std::string host;
        std::string user;
        std::string password;

        // SHA-1 of the host's certificate, colon-hex as vSphere prints it; it replaces the check of the
        // certificate chain. Empty checks the chain.
        std::string thumbPrint;
        uint32_t port = 0; // 0 is 443

        // The VM, "vm-1234" or "moref=vm-1234". A restore without one leaves the new disk unattached.
        std::string vm;

        // A snapshot to back the disk up as it was then, "snapshot-12". Empty backs up the disk as it is now,
        // through a snapshot taken for the run and removed after it.
        std::string snapshot;

        // The name of that snapshot. A snapshot left by a run that died is removed under this name, so a program
        // keeps to one name for good.
        std::string identity = "abr-vmdk-tool";

        // Quiesce the guest through VMware Tools when snapshotting a running VM.
        bool quiesce = false;

        // Where the temporary VM a new disk is uploaded through runs: a cluster or host and a resource pool,
        // by name. Empty picks the one the datastore allows, and fails when there is more than one.
        std::string compute;
        std::string resourcePool;
    };

    //
    // A VM's disk, read through the SOAP API.
    //
    // A powered-off VM whose disk has no snapshots is read directly, in any order. A running VM, a disk with
    // snapshots or a given snapshot is read through vSphere's export of a snapshot, which streams the disk whole:
    // reads have to ascend, which is what a backup does, and every run downloads all of the disk's data even when
    // only its changes are kept.
    //
    class DiskReader final : public IDiskReader
    {
    public:
        DiskReader();
        ~DiskReader() override;

        // diskPath is "[datastore] dir/name.vmdk", the disk's file or one of its snapshot chain.
        // changeId restricts dataRanges() to what changed since that change tracking id, and then needs changed
        // block tracking on the VM; empty reads the allocated blocks, or the whole disk without tracking.
        bool open(const ConnectParams& params, const std::string& diskPath, const std::string& changeId = {});

        const DiskGeometry& geometry() const override;
        bool dataRanges(std::vector<ByteRange>& ranges) override;
        bool read(uint64_t offset, uint32_t length, void* pBuf) override;

        // The change id the next incremental is taken against; empty without change tracking.
        const std::string& nextChangeId() const;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };

    //
    // A disk restored through the SOAP API, one of three ways decided by the VM and vmdk given:
    //
    //   a VM and "[datastore] dir/name.vmdk" naming one of its disks: that disk is overwritten in place. The VM
    //       has to be powered off and have no snapshots. The disk ends up fully allocated, and the VM's change
    //       tracking is reset, so every disk of the VM needs a full backup next.
    //   a VM and any other name: a new disk is created and attached to the VM, which has to be powered off.
    //       A bare name puts it in the VM's folder.
    //   no VM and "[datastore] dir/name.vmdk": a new disk is created at that path, attached to nothing.
    //
    // A new disk is thin, uploaded through a temporary VM that is deleted afterwards. Nothing is written over an
    // existing file other than the disk being overwritten.
    // There is no writing into part of a disk: writes have to ascend, and what lies between them is zero.
    //
    class DiskWriter final : public IDiskWriter
    {
    public:
        DiskWriter();
        ~DiskWriter() override;

        // sizeBytes is the size a new disk is created with; an overwritten disk keeps its own.
        bool open(const ConnectParams& params, const std::string& vmdk, uint64_t sizeBytes);

        // Whether this restore creates the disk rather than overwriting one.
        bool creates() const;

        // A new disk has no geometry of its own until it is created, so it answers with a zeroed one.
        const DiskGeometry& geometry() const override;
        bool setImageSize(uint64_t byteSize) override;
        bool write(uint64_t offset, uint32_t length, const void* pBuf) override;

        // Ends the upload and attaches or places the disk. Without it the destructor removes whatever a new disk
        // left behind; an overwritten disk stays partly written until a restore of it succeeds.
        bool finish() override;

    private:
        // One of the three ways above.
        std::unique_ptr<Detail::WriterImpl> m_impl;
    };
}
