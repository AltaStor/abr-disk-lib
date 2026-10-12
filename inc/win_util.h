// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Small Win32 helpers shared by the disk layers and the tools above them
//
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Win
{
    // The tool keeps every string in UTF-8 -- the command line, paths, messages,
    // the JSON it writes -- and converts only at the Win32 boundary, where the
    // wide entry points are the ones that take any name the system can hold.
    std::wstring widen(const std::string& text);
    std::string narrow(const wchar_t* text, size_t length);
    std::string narrow(const std::wstring& text);

    // The message the system has for an error code, followed by the code:
    // "Access is denied (error 5)". English when the system has it in English,
    // since what the tool prints ends up in a backup server's logs; otherwise in
    // the system's own language.
    std::string errorText(DWORD code);

    // The command line, one argument per entry, in UTF-8. Read in its wide form
    // so that no argument is squeezed through the ANSI code page on the way in.
    std::vector<std::string> commandLineArguments();

    // Whether the process runs with a full administrator token. Attaching a
    // virtual disk and opening a disk device both need one; a filtered token
    // (UAC, or a remote session of a local account) has the Administrators group
    // for deny only, which is as good as not having it.
    bool isElevated();

    // Enables a privilege the token already holds. Administrators hold several
    // that start out disabled, and a kernel check on one of those fails until it
    // is switched on. Returns false when the token does not hold it at all.
    bool enablePrivilege(const wchar_t* name);

    // The logical and physical sector sizes of the disk or volume behind a
    // handle, as the storage stack reports them. False when nothing answered
    // -- a handle on a plain file, where neither query applies -- and both
    // sizes are then left alone for the caller's own default.
    //
    // Takes a handle rather than a path because every caller here already has
    // one open on the device, and a disk that has just surfaced, or one about
    // to be taken offline for a restore, is better not opened twice.
    // Disk::deviceSectorSize is the portable way in, for a caller that has
    // only a path.
    bool sectorSizes(HANDLE handle, uint32_t& logical, uint32_t& physical);

    // Reads or writes exactly `length` bytes at `offset` of a synchronous
    // handle, in as many calls as the handle needs. Returns ERROR_SUCCESS, the
    // system's error code, or ERROR_HANDLE_EOF for a transfer that ran out.
    DWORD readAt(HANDLE handle, uint64_t offset, void* pBuf, uint32_t length);
    DWORD writeAt(HANDLE handle, uint64_t offset, const void* pBuf, uint32_t length);

    //
    // Owns a HANDLE and closes it once. Win32 has two spellings of "no handle"
    // -- NULL from most calls, INVALID_HANDLE_VALUE from CreateFile -- and both
    // count as empty here.
    //
    class Handle
    {
    public:
        Handle() = default;
        explicit Handle(HANDLE handle)
            : m_handle(handle)
        {}

        ~Handle() { close(); }

        Handle(Handle&& other) noexcept
            : m_handle(other.release())
        {}

        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other) {
                close();
                m_handle = other.release();
            }
            return *this;
        }

        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        HANDLE get() const { return m_handle; }

        bool valid() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

        HANDLE release()
        {
            HANDLE handle = m_handle;
            m_handle = nullptr;
            return handle;
        }

        // For the calls that hand a new handle back through a pointer.
        HANDLE* put()
        {
            close();
            return &m_handle;
        }

        void close()
        {
            if (valid()) {
                CloseHandle(m_handle);
            }
            m_handle = nullptr;
        }

    private:
        HANDLE m_handle = nullptr;
    };
}
