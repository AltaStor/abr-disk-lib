// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// What the exported classes share, and the per-host entry points behind them
//
#pragma once

#include <iostream>

#include "disk_device.h"

namespace Disk::Detail
{
    // The file and device backends, one implementation per host: exactly one
    // of file_disk_posix.cpp and file_disk_windows.cpp is compiled. Null, with
    // the reason already on std::cerr, when the open failed.
    std::unique_ptr<IDiskReader> openFileOrDevice(const std::string& path);
    std::unique_ptr<IDiskWriter> openFileOrDevice(const std::string& path, bool keepExisting);

    // What an exported class answers with before open() has succeeded. A
    // zeroed geometry is what "no fixed layout" already looks like to a
    // caller, and a transfer through a disk that is not open is a bug in the
    // caller worth a line rather than a null dereference.
    inline const DiskGeometry& noGeometry()
    {
        static const DiskGeometry geometry;
        return geometry;
    }

    inline bool notOpen(const char* what)
    {
        std::cerr << "Error: " << what << " on a disk that is not open\n";
        return false;
    }
}

//
// The forwarding half of an exported reader or writer: every one of them holds
// its backend as m_impl and answers through it. Spelled once here so that the
// classes cannot drift apart in what an unopened one does.
//
#define ABR_DISK_FORWARD_READER(Class)                                              \
    const DiskGeometry& Class::geometry() const                                     \
    {                                                                               \
        return m_impl ? m_impl->geometry() : Disk::Detail::noGeometry();            \
    }                                                                               \
    bool Class::dataRanges(std::vector<ByteRange>& ranges)                          \
    {                                                                               \
        return m_impl ? m_impl->dataRanges(ranges) : Disk::Detail::notOpen("range query"); \
    }                                                                               \
    bool Class::read(uint64_t offset, uint32_t length, void* pBuf)                  \
    {                                                                               \
        return m_impl ? m_impl->read(offset, length, pBuf) : Disk::Detail::notOpen("read"); \
    }

#define ABR_DISK_FORWARD_WRITER(Class)                                              \
    const DiskGeometry& Class::geometry() const                                     \
    {                                                                               \
        return m_impl ? m_impl->geometry() : Disk::Detail::noGeometry();            \
    }                                                                               \
    bool Class::setImageSize(uint64_t byteSize)                                     \
    {                                                                               \
        return m_impl ? m_impl->setImageSize(byteSize) : Disk::Detail::notOpen("resize"); \
    }                                                                               \
    bool Class::write(uint64_t offset, uint32_t length, const void* pBuf)           \
    {                                                                               \
        return m_impl ? m_impl->write(offset, length, pBuf) : Disk::Detail::notOpen("write"); \
    }                                                                               \
    bool Class::finish()                                                            \
    {                                                                               \
        return m_impl ? m_impl->finish() : Disk::Detail::notOpen("finish");         \
    }
