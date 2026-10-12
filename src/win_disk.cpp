// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Raw access to disk and volume devices
//
#include "win_disk.h"

#include <windows.h>

#include <winioctl.h>

#include <cstring>
#include <iostream>

namespace {

// winioctl.h has these from the Windows 7 SDK on, but MinGW-w64's does not, so
// the tool carries its own copy under names that cannot collide with either.
constexpr DWORD kIoctlDiskGetDiskAttributes =
    CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlDiskSetDiskAttributes =
    CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS);

constexpr DWORDLONG kDiskAttributeOffline = 0x0000000000000001;

struct GetDiskAttributes {
    DWORD Version;
    DWORD Reserved1;
    DWORDLONG Attributes;
};

struct SetDiskAttributes {
    DWORD Version;
    BOOLEAN Persist;
    BYTE Reserved1[3];
    DWORDLONG Attributes;
    DWORDLONG AttributesMask;
    DWORD Reserved2[4];
};

static_assert(sizeof(GetDiskAttributes) == 16, "GET_DISK_ATTRIBUTES layout");
static_assert(sizeof(SetDiskAttributes) == 40, "SET_DISK_ATTRIBUTES layout");

// Buffers are allocated in whole megabytes, so the copy loops' 4 MiB chunks and
// the few sizes around them share one allocation instead of each growing it.
constexpr uint32_t kBufferGranularity = 1024 * 1024;

bool control(HANDLE handle, DWORD code, const void* pIn = nullptr, DWORD inSize = 0,
             void* pOut = nullptr, DWORD outSize = 0)
{
    DWORD bytes = 0;
    return DeviceIoControl(handle, code, const_cast<void*>(pIn), inSize, pOut, outSize, &bytes, nullptr) != FALSE;
}

// A control code that answers with one fixed-size structure.
template <typename Out>
bool query(HANDLE handle, DWORD code, Out& out, const void* pIn = nullptr, DWORD inSize = 0)
{
    return control(handle, code, pIn, inSize, &out, sizeof(out));
}

// A volume answers for the extents it occupies on its disks; a disk has none.
// A volume spread over more extents than one fits reports that as
// ERROR_MORE_DATA, which is still an answer.
bool isVolume(HANDLE handle)
{
    VOLUME_DISK_EXTENTS extents = {};
    return query(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, extents)
        || GetLastError() == ERROR_MORE_DATA;
}

bool setOffline(HANDLE handle, bool offline)
{
    SetDiskAttributes attributes = {};
    attributes.Version = sizeof(attributes);

    // Not persisted: the state is gone with the next reboot, or with the device
    // itself when it is a virtual disk that is about to be detached, so nothing
    // this tool does can leave a disk offline for good.
    attributes.Persist = FALSE;
    attributes.Attributes = offline ? kDiskAttributeOffline : 0;
    attributes.AttributesMask = kDiskAttributeOffline;

    return control(handle, kIoctlDiskSetDiskAttributes, &attributes, sizeof(attributes));
}

} // namespace

DWORD Win::openDevice(const std::string& path, bool writable, Handle& handle)
{
    // Shared both ways, since the storage stack and whatever else looks at the
    // device keep handles of their own; what keeps them from writing to it
    // underneath a restore is RawDisk::claim, not the share mode.
    *handle.put() = CreateFileW(widen(path).c_str(),
                                GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_NO_BUFFERING | (writable ? FILE_FLAG_WRITE_THROUGH : 0),
                                nullptr);
    return handle.valid() ? ERROR_SUCCESS : GetLastError();
}

bool Win::deviceSize(HANDLE handle, uint64_t& size)
{
    // Only disks and volumes answer this; a file system turns it down for a
    // file, which is how a device is told from a file whatever it is called.
    GET_LENGTH_INFORMATION length = {};
    if (!query(handle, IOCTL_DISK_GET_LENGTH_INFO, length)) {
        return false;
    }
    size = static_cast<uint64_t>(length.Length.QuadPart);
    return true;
}

Win::RawDisk::~RawDisk()
{
    close();

    if (m_pBuffer) {
        VirtualFree(m_pBuffer, 0, MEM_RELEASE);
    }
}

bool Win::RawDisk::adopt(Handle handle, const std::string& path, bool writable)
{
    m_handle = std::move(handle);
    m_path = path;

    if (!deviceSize(m_handle.get(), m_size)) {
        std::cerr << "Error: '" << path << "' is not a disk or a volume: "
                  << errorText(GetLastError()) << "\n";
        return false;
    }

    // A device that answers neither query keeps the 512 every consumer of a
    // raw image assumes. The POSIX half asks the same question of its own
    // host, in Disk::deviceSectorSize.
    m_geometry.sectorSize = 512;
    m_geometry.phySectorSize = 512;
    Win::sectorSizes(m_handle.get(), m_geometry.sectorSize, m_geometry.phySectorSize);

    m_geometry.sectors = m_size / m_geometry.sectorSize;
    m_isVolume = isVolume(m_handle.get());

    // Nothing checks a read-only handle further, but a device reached through
    // a write handle is about to be restored onto, and a size that is not a
    // whole number of sectors means the geometry above is not to be trusted.
    if (writable && m_size % m_geometry.sectorSize != 0) {
        std::cerr << "Error: '" << path << "' holds " << m_size
                  << " bytes, which is not a whole number of " << m_geometry.sectorSize
                  << "-byte sectors\n";
        return false;
    }
    return true;
}

bool Win::RawDisk::claim(bool giveBack)
{
    if (m_isVolume) {
        // The lock is what fails while files are open on the volume, so it is
        // taken first: dismounting without it would pull the file system out
        // from under whoever has them open.
        if (!control(m_handle.get(), FSCTL_LOCK_VOLUME)) {
            std::cerr << "Error: failed to lock volume '" << m_path
                      << "' for the restore: " << errorText(GetLastError())
                      << "; close whatever has files open on it\n";
            return false;
        }
        m_locked = true;

        if (!control(m_handle.get(), FSCTL_DISMOUNT_VOLUME)) {
            std::cerr << "Error: failed to dismount volume '" << m_path
                      << "': " << errorText(GetLastError()) << "\n";
            return false;
        }
        return true;
    }

    GetDiskAttributes current = {};
    if (!query(m_handle.get(), kIoctlDiskGetDiskAttributes, current)) {
        std::cerr << "Error: failed to read the attributes of disk '" << m_path
                  << "': " << errorText(GetLastError()) << "\n";
        return false;
    }

    if (current.Attributes & kDiskAttributeOffline) {
        return true;
    }

    // Refused for the disk Windows runs from and for one holding a page file,
    // which is as it should be: neither is a restore target.
    if (!setOffline(m_handle.get(), true)) {
        std::cerr << "Error: failed to take disk '" << m_path
                  << "' offline for the restore: " << errorText(GetLastError()) << "\n";
        return false;
    }
    m_giveBackOnline = giveBack;
    return true;
}

bool Win::RawDisk::checkAlignment(const char* op, uint64_t offset, uint32_t length) const
{
    if (offset % m_geometry.sectorSize != 0 || length % m_geometry.sectorSize != 0) {
        std::cerr << "Error: unaligned disk " << op << " of " << length << " bytes at offset "
                  << offset << " (sector size " << m_geometry.sectorSize << ")\n";
        return false;
    }
    return true;
}

bool Win::RawDisk::reserve(uint32_t length)
{
    if (length <= m_bufferSize) {
        return true;
    }

    if (m_pBuffer) {
        VirtualFree(m_pBuffer, 0, MEM_RELEASE);
        m_pBuffer = nullptr;
        m_bufferSize = 0;
    }

    // Page aligned, which covers the alignment any storage adapter asks for.
    const uint32_t size = (length + kBufferGranularity - 1) / kBufferGranularity * kBufferGranularity;
    m_pBuffer = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m_pBuffer) {
        std::cerr << "Error: failed to allocate a " << size << "-byte transfer buffer\n";
        return false;
    }
    m_bufferSize = size;
    return true;
}

bool Win::RawDisk::read(uint64_t offset, uint32_t length, void* pBuf)
{
    if (!checkAlignment("read", offset, length) || !reserve(length)) {
        return false;
    }

    const DWORD error = readAt(m_handle.get(), offset, m_pBuffer, length);
    if (error != ERROR_SUCCESS) {
        std::cerr << "Error: failed to read " << length << " bytes at offset " << offset
                  << " of '" << m_path << "': " << errorText(error) << "\n";
        return false;
    }

    std::memcpy(pBuf, m_pBuffer, length);
    return true;
}

bool Win::RawDisk::write(uint64_t offset, uint32_t length, const void* pBuf)
{
    if (!checkAlignment("write", offset, length) || !reserve(length)) {
        return false;
    }

    std::memcpy(m_pBuffer, pBuf, length);

    const DWORD error = writeAt(m_handle.get(), offset, m_pBuffer, length);
    if (error != ERROR_SUCCESS) {
        std::cerr << "Error: failed to write " << length << " bytes at offset " << offset
                  << " of '" << m_path << "': " << errorText(error) << "\n";
        return false;
    }
    return true;
}

bool Win::RawDisk::flush()
{
    if (!FlushFileBuffers(m_handle.get())) {
        std::cerr << "Error: failed to flush '" << m_path << "': " << errorText(GetLastError()) << "\n";
        return false;
    }
    return true;
}

bool Win::RawDisk::close()
{
    if (!m_handle.valid()) {
        return true;
    }

    bool ok = true;

    if (m_giveBackOnline) {
        m_giveBackOnline = false;

        if (setOffline(m_handle.get(), false)) {
            // Online again, the disk is read afresh, restored partition table
            // included, rather than as it was before the restore.
            control(m_handle.get(), IOCTL_DISK_UPDATE_PROPERTIES);
        } else {
            std::cerr << "Error: failed to put disk '" << m_path << "' back online: "
                    << errorText(GetLastError()) << "; bring it online in Disk Management\n";
            ok = false;
        }
    }

    // Closing the handle would release the lock too; saying so explicitly
    // keeps the order of the two obvious.
    if (m_locked) {
        m_locked = false;
        control(m_handle.get(), FSCTL_UNLOCK_VOLUME);
    }

    m_handle.close();
    return ok;
}
