// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Raw access to disk and volume devices
//
#pragma once

#include "disk_device.h"
#include "win_util.h"

namespace Win
{
    // Opens a disk or volume device for raw access. Returns the system's error
    // code rather than reporting it: a caller waiting for a just-attached disk
    // to show up expects ERROR_FILE_NOT_FOUND a few times before it succeeds.
    DWORD openDevice(const std::string& path, bool writable, Handle& handle);

    // Whether an open handle is on a disk or a volume rather than on a file of
    // some file system, and how many bytes the device holds if it is.
    bool deviceSize(HANDLE handle, uint64_t& size);

    //
    // Raw access to a disk or a volume: \\.\PhysicalDrive2, \\.\E:, or the disk
    // a virtual disk is attached as.
    //
    // A device is never cached, so every transfer has to start and end on a
    // sector boundary and go through a buffer aligned the way the device asks.
    // Keeping to sectors is the caller's part, which the copy loops do anyway;
    // the buffer is this class's, so they can go on passing whatever they hold.
    //
    class RawDisk
    {
    public:
        RawDisk() = default;
        ~RawDisk();

        RawDisk(const RawDisk&) = delete;
        RawDisk& operator=(const RawDisk&) = delete;

        // Takes over a handle openDevice returned, and reads the device's size
        // and sector sizes.
        bool adopt(Handle handle, const std::string& path, bool writable);

        const DiskGeometry& geometry() const { return m_geometry; }

        uint64_t size() const { return m_size; }

        // Gets what Windows has mounted from the device out of the way of a raw
        // write. Since Vista a write to sectors that belong to a mounted volume
        // fails, however the device was opened, so a volume is locked and
        // dismounted, and a disk is taken offline -- which dismounts every
        // volume on it and keeps new ones from appearing while it is written.
        //
        // giveBack puts a disk back online when it is closed, for a disk that is
        // meant to be used afterwards. Only a disk that was online to begin with
        // is put back.
        bool claim(bool giveBack);

        bool read(uint64_t offset, uint32_t length, void* pBuf);
        bool write(uint64_t offset, uint32_t length, const void* pBuf);
        bool flush();

        // Undoes what claim() did and closes the device. Returns false when the
        // disk could not be put back online.
        bool close();

    private:
        bool checkAlignment(const char* op, uint64_t offset, uint32_t length) const;
        bool reserve(uint32_t length);

        Handle m_handle;
        std::string m_path;
        DiskGeometry m_geometry;
        uint64_t m_size = 0;
        bool m_isVolume = false;

        // What claim() did, for close() to undo.
        bool m_locked = false;
        bool m_giveBackOnline = false;

        void* m_pBuffer = nullptr;
        uint32_t m_bufferSize = 0;
    };
}
