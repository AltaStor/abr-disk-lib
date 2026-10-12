// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Byte-oriented disk source/target abstraction
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DiskGeometry {
    uint64_t sectors = 0;
    uint32_t sectorSize = 0;
    uint32_t phySectorSize = 0;
};

struct ByteRange {
    uint64_t offset;
    uint64_t length;
};

//
// Read side of a backup source. Offsets and lengths are byte counts so the
// backup loop is identical for a plain file, a disk device and a virtual disk
// behind a hypervisor's own disk API -- Hyper-V's virtual disk service
// (vhdx_disk.h), VMware's VDDK (vddk_disk.h).
//
class IDiskReader
{
public:
    virtual ~IDiskReader() = default;

    virtual const DiskGeometry& geometry() const = 0;

    // Regions worth reading: for an incremental of a virtual disk, the blocks
    // the hypervisor reports changed since a change tracking id; the allocated
    // blocks where it can say which those are; the whole extent otherwise.
    virtual bool dataRanges(std::vector<ByteRange>& ranges) = 0;

    virtual bool read(uint64_t offset, uint32_t length, void* pBuf) = 0;
};

//
// Write side of a restore target.
//
class IDiskWriter
{
public:
    virtual ~IDiskWriter() = default;

    // A zeroed geometry means the target has no fixed layout (a plain file),
    // so no compatibility check against the backup applies.
    virtual const DiskGeometry& geometry() const = 0;

    // A restore writes only the ranges the backup holds, so a target that
    // starts out empty would otherwise end at the last of them -- short of the
    // original disk, losing whatever sits in the unwritten tail. Declaring the
    // image size lets a file target match the disk it came from; a target with
    // a fixed layout of its own has nothing to do here.
    virtual bool setImageSize(uint64_t byteSize) = 0;

    virtual bool write(uint64_t offset, uint32_t length, const void* pBuf) = 0;

    virtual bool finish() = 0;
};

//
// A plain file, or a disk or volume named as a device -- /dev/sdb on Linux,
// \\.\PhysicalDrive2 or \\.\E: on Windows. Which of the two a path is gets
// worked out by asking what was opened, not by reading the name.
//
// "Local or remote" is the path's business here: a file on an NFS mount or a
// UNC path on an SMB share is opened exactly like one on a local disk, with
// whatever credentials the host already holds for it.
//
// Every class this library exports has the same shape: default constructed,
// then open(), which reports a failure on std::cerr behind "Error: " and
// returns false. Until open() succeeds the object answers with a zeroed
// geometry and fails every transfer. The backend itself stays behind a pointer,
// so that no public header has to pull in a host's or a hypervisor's own.
//
class FileDiskReader final : public IDiskReader
{
public:
    FileDiskReader();
    ~FileDiskReader() override;

    bool open(const std::string& path);

    const DiskGeometry& geometry() const override;
    bool dataRanges(std::vector<ByteRange>& ranges) override;
    bool read(uint64_t offset, uint32_t length, void* pBuf) override;

private:
    std::unique_ptr<IDiskReader> m_impl;
};

class FileDiskWriter final : public IDiskWriter
{
public:
    FileDiskWriter();
    ~FileDiskWriter() override;

    // keepExisting appends to whatever is already at path instead of emptying
    // it, which is what applying an incremental onto an already restored full
    // needs.
    bool open(const std::string& path, bool keepExisting = false);

    const DiskGeometry& geometry() const override;
    bool setImageSize(uint64_t byteSize) override;
    bool write(uint64_t offset, uint32_t length, const void* pBuf) override;
    bool finish() override;

private:
    std::unique_ptr<IDiskWriter> m_impl;
};

// The two classes above, opened; null when the open failed. What both tools
// called before the classes were exported, and still the short way in.
std::unique_ptr<IDiskReader> openFileReader(const std::string& path);
std::unique_ptr<IDiskWriter> openFileWriter(const std::string& path, bool keepExisting = false);

namespace Disk
{
    // The virtual disk backends this build of the library carries. Which ones
    // it does is decided when the library is configured (ABR_DISK_WITH_VHDX,
    // ABR_DISK_WITH_VDDK), and a caller that includes a backend's header also
    // sees it at compile time, as ABR_DISK_HAVE_VHDX and ABR_DISK_HAVE_VDDK.
    bool hasVhdx();
    bool hasVddk();

    // The logical and physical sector size of the disk or volume at path, as
    // the host's storage stack reports them -- BLKSSZGET and BLKPBSZGET on
    // Linux, Win::sectorSizes on Windows. Both are left alone unless this
    // returns true.
    //
    // Which of a device and a plain file the path named is settled by asking
    // it, not by reading the name: the queries fail on a file, and false is
    // then the right answer -- a regular file has no geometry of its own. A
    // device the host declines to describe is the same false, silently: what
    // to assume instead, and whether to say so, is the caller's to decide.
    bool deviceSectorSize(const std::string& path, uint32_t& logical, uint32_t& physical);
}
