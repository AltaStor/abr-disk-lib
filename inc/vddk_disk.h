// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// VMware VDDK disk source/target
//
// The header names nothing of the VDDK's, so it can be included anywhere;
// whether the library behind it has the backend is ABR_DISK_HAVE_VDDK at
// compile time and Disk::hasVddk() at run time.
//
#pragma once

#include "disk_device.h"

namespace Vddk
{
    struct ConnectParams {
        // Empty host means the VMDK is opened from the local filesystem; a host
        // makes it a remote ESXi/vCenter disk and then vmxSpec is required.
        std::string host;
        std::string user;
        std::string password;
        std::string thumbPrint;
        std::string vmxSpec;
        std::string snapshotRef;
        std::string transportModes;
        uint32_t port = 0;
        uint32_t nfcHostPort = 0;

        // The name the VM's access lock is taken and released under
        // (VixDiskLib_PrepareForAccess). A lock left behind by a run that died
        // can only be cleared under the name that took it, so a program keeps
        // to one name for good -- which is why the default is the name
        // abr-vmdk-tool has always used rather than this library's.
        std::string identity = "abr-vmdk-tool";

        // Directory holding the VDDK's libraries (its lib64 on Linux, its bin
        // on Windows). Empty leaves the choice to the loader's own search
        // path -- LD_LIBRARY_PATH on Linux, which is also the only search path
        // that reaches the plugins the VDDK goes on to load for itself.
        std::string libraryDir;
    };

    //
    // Owns the VixDiskLib_Init/Exit and Connect/Disconnect lifecycle. Exactly one
    // instance per process; every disk opened through it must be released
    // before it is destroyed.
    //
    class Session
    {
    public:
        Session() = default;
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        bool connect(const ConnectParams& params, bool readOnly);

        // DiskReader and DiskWriter below, opened on this session; null when
        // the open failed.
        std::unique_ptr<IDiskReader> openReader(const std::string& diskPath,
                                                const std::string& changeId,
                                                uint64_t queryChunkSize);

        std::unique_ptr<IDiskWriter> openWriter(const std::string& diskPath);

    private:
        friend class DiskReader;
        friend class DiskWriter;

        // Holds the strings that VixDiskLibConnectParams points at.
        ConnectParams m_params;

        void* m_connection = nullptr;
        void* m_connectParams = nullptr;
        bool m_initialized = false;
        bool m_accessPrepared = false;
    };

    //
    // A VMDK opened read-only on a connected session: a file of this host's
    // when the session has no host, a disk of a VM on an ESXi host or a vCenter
    // when it has one.
    //
    class DiskReader final : public IDiskReader
    {
    public:
        // The allocated-block query granularity a caller with no opinion uses,
        // in sectors.
        static constexpr uint64_t kDefaultQueryChunk = 2048;

        DiskReader();
        ~DiskReader() override;

        // changeId selects changed-block tracking: a change id (or "*" for the
        // whole disk) restricts the reader's ranges to areas changed since that
        // snapshot; empty reads all allocated blocks.
        // queryChunkSize is the allocated-block query granularity, in sectors.
        bool open(Session& session, const std::string& diskPath, const std::string& changeId = {},
                  uint64_t queryChunkSize = kDefaultQueryChunk);

        const DiskGeometry& geometry() const override;
        bool dataRanges(std::vector<ByteRange>& ranges) override;
        bool read(uint64_t offset, uint32_t length, void* pBuf) override;

    private:
        std::unique_ptr<IDiskReader> m_impl;
    };

    // A VMDK opened for a restore. The disk has to exist: the VDDK writes to
    // disks, and creating one on a host is the hypervisor's business.
    class DiskWriter final : public IDiskWriter
    {
    public:
        DiskWriter();
        ~DiskWriter() override;

        bool open(Session& session, const std::string& diskPath);

        const DiskGeometry& geometry() const override;
        bool setImageSize(uint64_t byteSize) override;
        bool write(uint64_t offset, uint32_t length, const void* pBuf) override;
        bool finish() override;

    private:
        std::unique_ptr<IDiskWriter> m_impl;
    };
}
