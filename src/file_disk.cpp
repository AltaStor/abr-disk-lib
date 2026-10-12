// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// File- and device-backed disk source/target: the exported classes
//
#include "disk_device.h"

#include "disk_detail.h"

FileDiskReader::FileDiskReader() = default;
FileDiskReader::~FileDiskReader() = default;

bool FileDiskReader::open(const std::string& path)
{
    m_impl = Disk::Detail::openFileOrDevice(path);
    return m_impl != nullptr;
}

ABR_DISK_FORWARD_READER(FileDiskReader)

FileDiskWriter::FileDiskWriter() = default;
FileDiskWriter::~FileDiskWriter() = default;

bool FileDiskWriter::open(const std::string& path, bool keepExisting)
{
    m_impl = Disk::Detail::openFileOrDevice(path, keepExisting);
    return m_impl != nullptr;
}

ABR_DISK_FORWARD_WRITER(FileDiskWriter)

std::unique_ptr<IDiskReader> openFileReader(const std::string& path)
{
    auto reader = std::make_unique<FileDiskReader>();
    if (!reader->open(path)) {
        return nullptr;
    }
    return reader;
}

std::unique_ptr<IDiskWriter> openFileWriter(const std::string& path, bool keepExisting)
{
    auto writer = std::make_unique<FileDiskWriter>();
    if (!writer->open(path, keepExisting)) {
        return nullptr;
    }
    return writer;
}

// Decided by the build, which defines each macro on this library's target when
// the backend is compiled in.
bool Disk::hasVhdx()
{
#ifdef ABR_DISK_HAVE_VHDX
    return true;
#else
    return false;
#endif
}

bool Disk::hasVddk()
{
#ifdef ABR_DISK_HAVE_VDDK
    return true;
#else
    return false;
#endif
}
