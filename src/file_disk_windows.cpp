// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// File- and device-backed disk source/target, the Windows half
//
#include "disk_device.h"

#include <windows.h>

#include <winioctl.h>

#include <iostream>

#include "disk_detail.h"
#include "win_disk.h"
#include "win_util.h"

namespace {

constexpr uint32_t kFileSectorSize = 512;

// A path spelled as a device -- \\.\PhysicalDrive1, \\.\E: -- rather than as a
// file. Only used to word a failure; what a handle actually is gets decided by
// asking it (see Win::deviceSize).
bool looksLikeDevice(const std::string& path)
{
    return path.rfind("\\\\.\\", 0) == 0;
}

// Raw disks and volumes are closed to anyone without a full administrator
// token, which is worth saying when that is why an open failed.
void reportOpenFailure(const char* what, const std::string& path, DWORD error)
{
    std::cerr << "Error: failed to open " << what << " '" << path << "': " << Win::errorText(error) << "\n";
    if (error == ERROR_ACCESS_DENIED && looksLikeDevice(path) && !Win::isElevated()) {
        std::cerr << "Disks and volumes can only be opened from an elevated (administrator) process.\n";
    }
}

class FileHandleReader : public IDiskReader
{
public:
    bool open(Win::Handle handle, const std::string& path)
    {
        m_handle = std::move(handle);
        m_path = path;

        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(m_handle.get(), &size)) {
            std::cerr << "Error: failed to read the size of source '" << path
                      << "': " << Win::errorText(GetLastError()) << "\n";
            return false;
        }
        m_size = static_cast<uint64_t>(size.QuadPart);

        m_geometry.sectorSize = kFileSectorSize;
        m_geometry.phySectorSize = kFileSectorSize;
        m_geometry.sectors = (m_size + kFileSectorSize - 1) / kFileSectorSize;
        return true;
    }

    const DiskGeometry& geometry() const override { return m_geometry; }

    bool dataRanges(std::vector<ByteRange>& ranges) override
    {
        ranges.clear();
        if (m_size > 0) {
            ranges.push_back({0, m_size});
        }
        return true;
    }

    bool read(uint64_t offset, uint32_t length, void* pBuf) override
    {
        const DWORD error = Win::readAt(m_handle.get(), offset, pBuf, length);
        if (error != ERROR_SUCCESS) {
            std::cerr << "Error: failed to read " << length << " bytes at offset " << offset
                      << ": " << Win::errorText(error) << "\n";
            return false;
        }
        return true;
    }

private:
    Win::Handle m_handle;
    std::string m_path;
    DiskGeometry m_geometry;
    uint64_t m_size = 0;
};

// A disk or a volume. The whole device is read: there is no allocation map to
// consult below a file system, so what is unused goes to backuplib as it is,
// and zeroed blocks are dropped there rather than stored.
class DeviceDiskReader : public IDiskReader
{
public:
    bool open(Win::Handle handle, const std::string& path)
    {
        return m_disk.adopt(std::move(handle), path, false);
    }

    const DiskGeometry& geometry() const override { return m_disk.geometry(); }

    bool dataRanges(std::vector<ByteRange>& ranges) override
    {
        ranges.clear();
        if (m_disk.size() > 0) {
            ranges.push_back({0, m_disk.size()});
        }
        return true;
    }

    bool read(uint64_t offset, uint32_t length, void* pBuf) override
    {
        return m_disk.read(offset, length, pBuf);
    }

private:
    Win::RawDisk m_disk;
};

class FileHandleWriter : public IDiskWriter
{
public:
    bool open(Win::Handle handle, const std::string& path, bool keepExisting)
    {
        m_handle = std::move(handle);
        m_path = path;

        if (keepExisting) {
            return true;
        }

        // Emptied first, like any target a full restore is written to: only
        // what the backup holds is written, so whatever was there before would
        // otherwise survive in between.
        FILE_END_OF_FILE_INFO endOfFile = {};
        if (!SetFileInformationByHandle(m_handle.get(), FileEndOfFileInfo, &endOfFile, sizeof(endOfFile))) {
            std::cerr << "Error: failed to empty target '" << path
                      << "': " << Win::errorText(GetLastError()) << "\n";
            return false;
        }

        // Sparse, so that the gaps between the ranges a restore writes, and the
        // tail it sizes the file to, take no space until something writes them -- which is what
        // a file on Linux gets without asking.
        // A file system that cannot do it (FAT, exFAT) fills them with zeros instead, which
        // reads back the same.
        DWORD bytes = 0;
        DeviceIoControl(m_handle.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr);
        return true;
    }

    bool setImageSize(uint64_t byteSize) override
    {
        m_imageSize = byteSize;
        return true;
    }

    const DiskGeometry& geometry() const override { return m_geometry; }

    bool write(uint64_t offset, uint32_t length, const void* pBuf) override
    {
        const DWORD error = Win::writeAt(m_handle.get(), offset, pBuf, length);
        if (error != ERROR_SUCCESS) {
            std::cerr << "Error: failed to write to target '" << m_path
                      << "': " << Win::errorText(error) << "\n";
            return false;
        }
        return true;
    }

    bool finish() override
    {
        if (!FlushFileBuffers(m_handle.get())) {
            std::cerr << "Error: failed to flush target '" << m_path
                      << "': " << Win::errorText(GetLastError()) << "\n";
            return false;
        }
        if (m_imageSize == 0) {
            return true;
        }

        // Sized after the data is on disk. Growing it leaves a sparse tail; on
        // an incremental applied to an already sized file it changes nothing.
        FILE_END_OF_FILE_INFO endOfFile = {};
        endOfFile.EndOfFile.QuadPart = static_cast<LONGLONG>(m_imageSize);
        if (!SetFileInformationByHandle(m_handle.get(), FileEndOfFileInfo, &endOfFile, sizeof(endOfFile))) {
            std::cerr << "Error: failed to size target '" << m_path << "' to " << m_imageSize
                      << " bytes: " << Win::errorText(GetLastError()) << "\n";
            return false;
        }
        return true;
    }

private:
    Win::Handle m_handle;
    DiskGeometry m_geometry;
    std::string m_path;
    uint64_t m_imageSize = 0;
};

// A disk or a volume, with a layout of its own that the backup has to fit.
class DeviceDiskWriter : public IDiskWriter
{
public:
    bool open(Win::Handle handle, const std::string& path)
    {
        // A disk that was online is put back online afterwards, with the
        // restored partitions in place of the old ones.
        return m_disk.adopt(std::move(handle), path, true) && m_disk.claim(true);
    }

    const DiskGeometry& geometry() const override { return m_disk.geometry(); }

    // The device is as large as it is; whether the image fits it exactly is
    // the caller's to check against geometry() before it writes.
    bool setImageSize(uint64_t) override { return true; }

    bool write(uint64_t offset, uint32_t length, const void* pBuf) override
    {
        return m_disk.write(offset, length, pBuf);
    }

    bool finish() override { return m_disk.flush() && m_disk.close(); }

private:
    Win::RawDisk m_disk;
};

} // namespace

std::unique_ptr<IDiskReader> Disk::Detail::openFileOrDevice(const std::string& path)
{
    // Shared for writing too, so that a file some other process holds open can
    // still be read; keeping it still while it is backed up is the caller's
    // business, as it is on Linux.
    Win::Handle handle(CreateFileW(Win::widen(path).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) {
        reportOpenFailure("source", path, GetLastError());
        return nullptr;
    }

    uint64_t size = 0;
    if (Win::deviceSize(handle.get(), size)) {
        auto reader = std::make_unique<DeviceDiskReader>();
        if (!reader->open(std::move(handle), path)) {
            return nullptr;
        }
        return reader;
    }

    auto reader = std::make_unique<FileHandleReader>();
    if (!reader->open(std::move(handle), path)) {
        return nullptr;
    }
    return reader;
}

std::unique_ptr<IDiskWriter> Disk::Detail::openFileOrDevice(const std::string& path, bool keepExisting)
{
    const std::wstring widePath = Win::widen(path);

    // Opened as it is first, since only the open handle can tell a device from
    // a file, and a device is never created or emptied.
    Win::Handle handle(CreateFileW(widePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));

    if (!handle.valid()) {
        const DWORD error = GetLastError();

        if (error != ERROR_FILE_NOT_FOUND) {
            reportOpenFailure("target", path, error);
            return nullptr;
        }
        if (keepExisting) {
            std::cerr << "Error: target '" << path << "' does not exist; restore the full backup to it first\n";
            return nullptr;
        }

        *handle.put() = CreateFileW(widePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
        if (!handle.valid()) {
            reportOpenFailure("target", path, GetLastError());
            return nullptr;
        }
    }

    uint64_t size = 0;
    if (Win::deviceSize(handle.get(), size)) {
        auto writer = std::make_unique<DeviceDiskWriter>();
        if (!writer->open(std::move(handle), path)) {
            return nullptr;
        }
        return writer;
    }

    auto writer = std::make_unique<FileHandleWriter>();
    if (!writer->open(std::move(handle), path, keepExisting)) {
        return nullptr;
    }
    return writer;
}

bool Disk::deviceSectorSize(const std::string& path, uint32_t& logical, uint32_t& physical)
{
    // FILE_READ_ATTRIBUTES, not GENERIC_READ: the queries behind
    // Win::sectorSizes are informational and need no access to the data, while
    // opening a disk or a volume for reading needs an elevated token -- which
    // asking for a sector size has no business requiring.
    Win::Handle handle(CreateFileW(Win::widen(path).c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        return false;
    }

    // A regular file opens just as happily and then answers neither query,
    // which is the false the caller wants: what a device is gets settled by
    // asking the handle, not by reading the name.
    return Win::sectorSizes(handle.get(), logical, physical);
}
