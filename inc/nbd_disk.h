// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Disk source exported over NBD, read with libnbd
//
// Built for Linux, where libnbd is packaged. The header names nothing of libnbd's, so it can be included anywhere.
// Whether the library carries the backend is ABR_DISK_HAVE_NBD at compile time and Disk::hasNbd() at run time.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "disk_device.h"

namespace Nbd
{
    // Where the export is and how to read it.
    struct ReaderParams {
        // A Unix socket path, or an NBD URI such as nbd+unix:///drive-scsi0?socket=/run/x.sock or nbd://host/disk.
        std::string server;

        // The export to open on a socket; empty opens the server's default export.
        // A URI names its own export, so the two are not given together.
        std::string exportName;

        // The dirty bitmap an incremental reads, as QEMU names it.
        // Empty reads the whole disk, skipping what the server reports as reading zero.
        std::string bitmap;

        // Read rate limit in bytes per second, 0 for none.
        uint64_t bytesPerSecond = 0;

        // Discard each range after it is read.
        // On the backup export of a Proxmox VE VM this frees the fleecing image early.
        // On an export of a disk itself it erases the data, which is why it has to be asked for.
        bool discard = false;
    };

    // What the export says about itself, for the tool to report.
    struct ExportProperties {
        // The block status context the ranges come from: "qemu:dirty-bitmap:<name>", "base:allocation", or empty
        // when the server offers no allocation status and the whole disk is read.
        std::string context;

        uint64_t size = 0;
        uint32_t preferredBlockSize = 0;
        uint32_t maximumBlockSize = 0;
        bool readOnly = false;
        bool canTrim = false;
    };

    //
    // One export opened read-only for a backup, over one connection.
    //
    // A connection negotiates one block status context only, the dirty bitmap or base:allocation.
    // QEMU broke a connection that negotiated both when the bitmap was clean.
    //
    class DiskReader final : public IDiskReader
    {
    public:
        DiskReader();
        ~DiskReader() override;

        bool open(const ReaderParams& params);

        // Filled in by open().
        const ExportProperties& properties() const;

        // Bytes discarded so far; stays 0 unless ReaderParams::discard was set and the export can trim.
        uint64_t discardedBytes() const;

        const DiskGeometry& geometry() const override;
        bool dataRanges(std::vector<ByteRange>& ranges) override;
        bool read(uint64_t offset, uint32_t length, void* pBuf) override;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // The class above, opened; null when the open failed.
    std::unique_ptr<DiskReader> openReader(const ReaderParams& params);
}
