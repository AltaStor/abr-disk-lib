// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// File- and block-device-backed disk source/target, the POSIX half
//
#include "disk_device.h"

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>

#include "disk_detail.h"

namespace {

// What a plain file gets: a regular file has no geometry of its own, and 512 is
// the sector size every consumer of a raw image assumes.
constexpr uint32_t kFileSectorSize = 512;

// Logical and physical sector size of whatever backs path: the host's answer
// for a device, the convention above for a file. Worth asking rather than
// assuming because the backup records what this reports -- a 4Kn disk is 4096
// logical, a 512e one 512 over 4096, and a backup that claims 512 for either
// describes a disk it will then be refused a restore to.
void querySectorSize(const std::string& path, uint32_t& logical, uint32_t& physical)
{
    logical = kFileSectorSize;
    physical = kFileSectorSize;

    if (Disk::deviceSectorSize(path, logical, physical)) {
        return;
    }

    // A device the host would not describe keeps the file default, which is a
    // guess at that point and worth a word -- though not an "Error: " and not a
    // refusal: the query is as old as the block layer's notion of a sector size,
    // and 512 is what this assumed outright until now. The prefix is the whole
    // distinction, since ErrorCollector keeps the first line carrying it for the
    // --json failure document, and this is not a failure. Telling the two apart
    // for the message needs no host call of its own; the standard library has
    // it.
    std::error_code ec;
    if (std::filesystem::is_block_file(std::filesystem::path(path), ec)) {
        std::cerr << "Warning: cannot query the sector size of '" << path << "'; assuming "
                  << kFileSectorSize << " bytes\n";
    }
}

class FileStreamReader : public IDiskReader
{
public:
    bool open(const std::string& path)
    {
        m_stream.open(path, std::ios::binary);
        if (!m_stream) {
            std::cerr << "Error: failed to open source '" << path << "'\n";
            return false;
        }

        m_stream.seekg(0, std::ios::end);
        m_size = static_cast<uint64_t>(m_stream.tellg());

        querySectorSize(path, m_geometry.sectorSize, m_geometry.phySectorSize);
        m_geometry.sectors = (m_size + m_geometry.sectorSize - 1) / m_geometry.sectorSize;
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
        m_stream.seekg(static_cast<std::streamoff>(offset));
        m_stream.read(static_cast<char*>(pBuf), length);
        if (m_stream.gcount() != static_cast<std::streamsize>(length)) {
            std::cerr << "Error: failed to read " << length << " bytes at offset " << offset << "\n";
            return false;
        }
        return true;
    }

private:
    std::ifstream m_stream;
    DiskGeometry m_geometry;
    uint64_t m_size = 0;
};

class FileStreamWriter : public IDiskWriter
{
public:
    bool open(const std::string& path, bool keepExisting)
    {
        m_path = path;

        // in|out rather than a plain out: without it the stream truncates on
        // open, which would wipe the full restore an incremental is meant to
        // be applied on top of.
        std::ios::openmode mode = std::ios::binary | std::ios::out;
        mode |= keepExisting ? std::ios::in : std::ios::trunc;

        m_stream.open(path, mode);
        if (!m_stream) {
            if (keepExisting) {
                std::cerr << "Error: target '" << path
                          << "' does not exist; restore the full backup to it first\n";
            } else {
                std::cerr << "Error: failed to open target '" << path << "'\n";
            }
            return false;
        }
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
        m_stream.seekp(static_cast<std::streamoff>(offset));
        m_stream.write(static_cast<const char*>(pBuf), length);
        if (!m_stream) {
            std::cerr << "Error: failed to write to target '" << m_path << "'\n";
            return false;
        }
        return true;
    }

    bool finish() override
    {
        m_stream.flush();
        if (!m_stream) {
            std::cerr << "Error: failed to flush target '" << m_path << "'\n";
            return false;
        }
        if (m_imageSize == 0) {
            return true;
        }

        // Sizing the file happens after the data is on disk, and closing first
        // keeps the stream from writing past the new end afterwards. The result
        // is sparse: the tail costs nothing until something writes to it.
        m_stream.close();

        std::error_code ec;

        // A block device is as large as it is, and resizing one is an error
        // rather than a no-op; whether the image fits is the caller's to have
        // checked.
        if (std::filesystem::is_block_file(std::filesystem::path(m_path), ec)) {
            return true;
        }

        std::filesystem::resize_file(m_path, m_imageSize, ec);
        if (ec) {
            std::cerr << "Error: failed to size target '" << m_path << "' to " << m_imageSize
                      << " bytes: " << ec.message() << "\n";
            return false;
        }
        return true;
    }

private:
    std::fstream m_stream;
    DiskGeometry m_geometry;
    std::string m_path;
    uint64_t m_imageSize = 0;
};

} // namespace

std::unique_ptr<IDiskReader> Disk::Detail::openFileOrDevice(const std::string& path)
{
    auto reader = std::make_unique<FileStreamReader>();
    if (!reader->open(path)) {
        return nullptr;
    }
    return reader;
}

std::unique_ptr<IDiskWriter> Disk::Detail::openFileOrDevice(const std::string& path, bool keepExisting)
{
    auto writer = std::make_unique<FileStreamWriter>();
    if (!writer->open(path, keepExisting)) {
        return nullptr;
    }
    return writer;
}

bool Disk::deviceSectorSize([[maybe_unused]] const std::string& path,
                            [[maybe_unused]] uint32_t& logical,
                            [[maybe_unused]] uint32_t& physical)
{
#if defined(__linux__)
    // O_NONBLOCK because this descriptor only ever carries the two ioctls --
    // nothing is read through it -- and opening a removable or an unreachable
    // device must not hang here. A regular file opens just as happily and then
    // answers ENOTTY below, which is the no this is looking for.
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    int value = 0;
    const bool known = ::ioctl(fd, BLKSSZGET, &value) == 0 && value > 0;
    if (known) {
        logical = static_cast<uint32_t>(value);

        // A device that reports a logical size but no physical one is described
        // by the one it did report, not by a size nobody asked for.
        physical = logical;
        if (::ioctl(fd, BLKPBSZGET, &value) == 0 && value > 0) {
            physical = static_cast<uint32_t>(value);
        }
    }
    ::close(fd);
    return known;
#else
    // Every other POSIX host spells this its own way -- DIOCGSECTORSIZE on the
    // BSDs, DKIOCGETBLOCKSIZE on macOS -- and the library is not built for one
    // yet, so there is nothing to ask with until it is. The caller's own
    // default stands in the meantime.
    return false;
#endif
}
