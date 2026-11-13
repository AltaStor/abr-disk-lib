// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Hyper-V virtual disk (VHD/VHDX) source/target
//
// Windows only: the disk is reached through the host's own virtual disk
// service. The header itself names nothing of Windows', so it can be included
// anywhere; whether the library behind it has the backend is ABR_DISK_HAVE_VHDX
// at compile time and Disk::hasVhdx() at run time.
//
#pragma once

#include "disk_device.h"

namespace Vhdx
{
    // What a virtual disk says about itself beyond its geometry, for the tool to report.
    struct DiskProperties
    {
        std::string format; // "VHD" or "VHDX"
        std::string type;   // "fixed", "dynamic" or "differencing"

        // The disk's resilient change tracking (RCT) id as of the backup, when
        // tracking is on; empty when it is off or the disk cannot have it (a VHD).
        // It is the id a later incremental is taken against, with --change-id.
        std::string changeTrackingId;
    };

    //
    // A virtual disk is local or remote by its path: C:\VMs\disk.vhdx is on
    // this host, \\server\share\disk.vhdx on an SMB share -- which the
    // virtual disk service opens and attaches like any other, as long as the
    // share speaks SMB 3.
    //
    // These are for the share. Left empty, the process's own logon is what the
    // server sees, which is the usual arrangement in a domain. With a user
    // name, the library connects the share under that account for as long as
    // the disk is open and drops the connection afterwards -- unless the host
    // was already connected to it, and then the existing connection is left
    // exactly as it was found.
    //
    // Ignored for a path that is not a UNC path.
    //
    struct ShareCredentials
    {
        std::string user;     // "DOMAIN\\user" or "user@domain"
        std::string password;
    };

    // Whether a path names a disk on another machine: \\server\share\...,
    // or its long form \\?\UNC\server\share\.... A device path (\\.\...)
    // is not one.
    bool isRemotePath(const std::string& path);

    //
    // A VHD, VHDX or AVHDX opened read-only for a backup, and attached so that
    // it can be read.
    //
    // A disk a running VM is writing to cannot be opened; the disk a checkpoint
    // of that VM refers to can, since nothing writes to it any more.
    //
    class DiskReader final : public IDiskReader
    {
    public:
        DiskReader();
        ~DiskReader() override;

        // changeId selects resilient change tracking: a change id restricts
        // the reader's ranges to what changed since the disk had that id;
        // empty reads the whole disk.
        bool open(const std::string& path, const std::string& changeId = {},
                  const ShareCredentials& credentials = {});

        // Filled in by open().
        const DiskProperties& properties() const { return m_properties; }

        const DiskGeometry& geometry() const override;
        bool dataRanges(std::vector<ByteRange>& ranges) override;
        bool read(uint64_t offset, uint32_t length, void* pBuf) override;

    private:
        // Declared ahead of the disk so that it outlives it: the disk is
        // detached, and one that was not finished deleted, over the connection.
        std::shared_ptr<void> m_share;
        std::unique_ptr<IDiskReader> m_impl;
        DiskProperties m_properties;
    };

    //
    // A virtual disk opened for a restore.
    //
    // The disk is left offline and is detached by finish(), so that Windows
    // never mounts -- and writes to -- the restored file systems. One this
    // object created and did not finish() is removed again when it goes.
    //
    class DiskWriter final : public IDiskWriter
    {
    public:
        DiskWriter();
        ~DiskWriter() override;

        // Creates a dynamic disk with the image's geometry when nothing is at
        // path yet -- a VHDX, or a VHD when the name ends in .vhd. keepExisting
        // refuses to create one: an incremental is applied to the disk its
        // full backup was restored to.
        bool open(const std::string& path, bool keepExisting, const DiskGeometry& image,
                  const ShareCredentials& credentials = {});

        // Whether open() created the disk.
        bool created() const { return m_created; }

        const DiskGeometry& geometry() const override;
        bool setImageSize(uint64_t byteSize) override;
        bool write(uint64_t offset, uint32_t length, const void* pBuf) override;
        bool finish() override;

    private:
        std::shared_ptr<void> m_share;
        std::unique_ptr<IDiskWriter> m_impl;
        bool m_created = false;
    };

    // The two classes above, opened; null when the open failed. `properties`
    // and `created` are what the classes' own accessors answer.
    std::unique_ptr<IDiskReader> openReader(const std::string& path, const std::string& changeId,
                                            DiskProperties& properties,
                                            const ShareCredentials& credentials = {});

    std::unique_ptr<IDiskWriter> openWriter(const std::string& path, bool keepExisting,
                                            const DiskGeometry& image, bool& created,
                                            const ShareCredentials& credentials = {});
}
